/*
 * ukifs_slot.c - ukiboot A/B slot support
 *
 * Finds the ukiboot_a / ukiboot_b / ukibootctl partitions of a ukiboot
 * disk image and selects the slot holding the UKI to expose.
 *
 * GPT images are parsed with ukiboot's gpt_parser, and the ukibootctl
 * content is interpreted with ukiboot's bootctl helpers, so that the
 * slot selected here is the one ukiboot would boot.
 */

#include "config.h"

#include <fcntl.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <sys/stat.h>

#include "ukifs_slot.h"
#include "ukifs_utils.h"

/* ukiboot */
#include "bootctl.h"
#include "gpt_parser.h"

#define SECTOR_SIZE	   512
#define MBR_SIGNATURE	   0xAA55
#define MBR_PART_OFFSET	   446
#define MBR_PART_ENTRIES   4
#define MBR_MAX_LOGICAL	   128 /* Guard against corrupted/looping EBR chains */
#define GPT_SIGNATURE_LE64 0x5452415020494645ULL /* "EFI PART" */

/* MBR partition types used for the extended partition holding the slots */
#define MBR_TYPE_EXTENDED_CHS 0x05
#define MBR_TYPE_EXTENDED_LBA 0x0f
#define MBR_TYPE_EXTENDED_LNX 0x85

/* A partition of the image */
typedef struct {
    bool found;
    uint64_t offset;  /* Byte offset in the image */
    uint64_t size;    /* Byte size */
} uki_partition_t;

/* The ukiboot partitions of the image */
typedef struct {
    uki_partition_t slot[2];   /* ukiboot_a, ukiboot_b */
    uki_partition_t bootctl;   /* ukibootctl */
    bool gpt;                  /* true: GPT image, false: MBR image */
} uki_partitions_t;

/* On-disk MBR partition entry */
typedef struct {
    uint8_t status;
    uint8_t chs_first[3];
    uint8_t type;
    uint8_t chs_last[3];
    uint32_t lba_first;
    uint32_t num_sectors;
} PACKED mbr_entry_t;

/*
 * Record a partition in the ukiboot partition set, given its ukiboot
 * MBR id (the ids are also used internally for the GPT case).
 */
static void set_partition(uki_partitions_t *parts, uint8_t id,
                          uint64_t offset, uint64_t size) {
    uki_partition_t *part;

    switch (id) {
    case UKI_SLOT_A_MBR_ID:
	part = &parts->slot[0];
	break;
    case UKI_SLOT_B_MBR_ID:
	part = &parts->slot[1];
	break;
    case UKI_BOOTCTL_MBR_ID:
	part = &parts->bootctl;
	break;
    default:
	return;
    }

    if (part->found) {
	DEBUG_LOG("Ignoring duplicate ukiboot partition id 0x%02x", id);
	return;
    }

    part->found = true;
    part->offset = offset;
    part->size = size;

    DEBUG_LOG("Found ukiboot partition id 0x%02x at offset %llu, size %llu",
              id, (unsigned long long)offset, (unsigned long long)size);
}

/* Return true if the image carries a GPT partition table */
static bool image_is_gpt(int fd) {
    uint64_t signature;

    if (!full_pread(fd, &signature, sizeof(signature), SECTOR_SIZE)) {
        return false;
    }

    return le64toh(signature) == GPT_SIGNATURE_LE64;
}

/* Collect the ukiboot partitions of a GPT image (uses ukiboot's parser) */
static bool parse_gpt_partitions(int fd, uki_partitions_t *parts) {
    GptPartitionTable table = {0, NULL};

    if (!gpt_parse(fd, &table)) {
        ERROR_LOG("Failed to parse the GPT partition table");
        return false;
    }

    static const struct {
	const char *name;
	uint8_t id;
    } wanted[] = {
	{UKI_SLOT_A_PART_NAME, UKI_SLOT_A_MBR_ID},
	{UKI_SLOT_B_PART_NAME, UKI_SLOT_B_MBR_ID},
	{UKI_BOOTCTL_PART_NAME, UKI_BOOTCTL_MBR_ID},
    };

    for (size_t i = 0; i < sizeof(wanted) / sizeof(wanted[0]); i++) {
        GptPartition *part = gpt_find_partition(&table, wanted[i].name);
        if (part) {
            set_partition(parts, wanted[i].id, part->offset, part->size);
        } else {
            DEBUG_LOG("No '%s' partition in the GPT", wanted[i].name);
        }
    }

    gpt_partition_table_free(&table);

    parts->gpt = true;
    return true;
}

/* Read one MBR/EBR sector and validate its boot signature */
static bool read_partition_sector(int fd, uint64_t lba, uint8_t *sector) {
    if (!full_pread(fd, sector, SECTOR_SIZE, (off_t)(lba * SECTOR_SIZE))) {
        return false;
    }

    uint16_t signature = (uint16_t)(sector[510] | (sector[511] << 8));
    if (signature != MBR_SIGNATURE) {
        DEBUG_LOG("No MBR signature in sector %llu", (unsigned long long)lba);
        return false;
    }

    return true;
}

/* Extract partition entry 'index' of an MBR/EBR sector */
static void get_partition_entry(const uint8_t *sector, int index,
                                mbr_entry_t *entry) {
    memcpy(entry, sector + MBR_PART_OFFSET + index * sizeof(mbr_entry_t),
           sizeof(mbr_entry_t));
    entry->lba_first = le32toh(entry->lba_first);
    entry->num_sectors = le32toh(entry->num_sectors);
}

static bool is_extended_partition(uint8_t type) {
    return type == MBR_TYPE_EXTENDED_CHS ||
           type == MBR_TYPE_EXTENDED_LBA ||
           type == MBR_TYPE_EXTENDED_LNX;
}

/*
 * Walk the EBR chain of an extended partition, collecting the ukiboot
 * logical partitions (ids 0x46, 0x47 and 0x48).
 */
static void parse_logical_partitions(int fd, uint64_t extended_lba,
                                     uki_partitions_t *parts) {
    uint64_t ebr_lba = extended_lba;

    for (int i = 0; i < MBR_MAX_LOGICAL; i++) {
        uint8_t sector[SECTOR_SIZE];

        if (!read_partition_sector(fd, ebr_lba, sector)) {
            return;
        }

        /* First entry describes the logical partition itself, its start
         * being relative to the EBR sector. */
        mbr_entry_t entry;
        get_partition_entry(sector, 0, &entry);
        if (entry.num_sectors != 0) {
            set_partition(parts, entry.type,
                          (ebr_lba + entry.lba_first) * SECTOR_SIZE,
                          (uint64_t)entry.num_sectors * SECTOR_SIZE);
        }

        /* Second entry points to the next EBR, relative to the start of
         * the extended partition. */
        get_partition_entry(sector, 1, &entry);
        if (!is_extended_partition(entry.type) || entry.num_sectors == 0) {
            return;
        }

        ebr_lba = extended_lba + entry.lba_first;
    }

    DEBUG_LOG("Stopped walking the EBR chain after %d entries", MBR_MAX_LOGICAL);
}

/* Collect the ukiboot partitions of an MBR image */
static bool parse_mbr_partitions(int fd, uki_partitions_t *parts) {
    uint8_t sector[SECTOR_SIZE];

    if (!read_partition_sector(fd, 0, sector)) {
        ERROR_LOG("No GPT and no valid MBR partition table found");
        return false;
    }

    for (int i = 0; i < MBR_PART_ENTRIES; i++) {
        mbr_entry_t entry;

        get_partition_entry(sector, i, &entry);
        if (entry.num_sectors == 0) {
            continue;
        }

        if (is_extended_partition(entry.type)) {
            /* ukiboot stores its partitions as logical partitions */
            parse_logical_partitions(fd, entry.lba_first, parts);
        } else {
            set_partition(parts, entry.type,
                          (uint64_t)entry.lba_first * SECTOR_SIZE,
                          (uint64_t)entry.num_sectors * SECTOR_SIZE);
        }
    }

    parts->gpt = false;
    return true;
}

/*
 * Read the ukibootctl content and return the active slot (0 or 1),
 * or -1 on error. Follows ukiboot's semantics: an uninitialized or
 * corrupted content is replaced by the ukiboot default (both slots
 * valid, slot a with the highest priority).
 */
static int read_active_slot(int image_fd, const uki_partitions_t *parts,
                            const char *bootctl_path) {
    UkiBootCtrl bootctl;
    int fd = image_fd;
    int bootctl_fd = -1;
    off_t offset = 0;

    if (bootctl_path) {
        bootctl_fd = open(bootctl_path, O_RDONLY);
        if (bootctl_fd < 0) {
            ERROR_LOG("Failed to open ukibootctl partition '%s': %s",
                      bootctl_path, strerror(errno));
            return -1;
        }
        fd = bootctl_fd;
    } else {
        if (!parts->bootctl.found) {
            ERROR_LOG("No '%s' partition in the image: "
                      "select a slot manually or use --bootctl",
                      UKI_BOOTCTL_PART_NAME);
            return -1;
        }
        if (parts->bootctl.size < sizeof(UkiBootCtrl)) {
            ERROR_LOG("The '%s' partition is too small (%llu bytes)",
                      UKI_BOOTCTL_PART_NAME,
                      (unsigned long long)parts->bootctl.size);
            return -1;
        }
        offset = (off_t)parts->bootctl.offset;
    }

    bool read_ok = full_pread(fd, &bootctl, sizeof(bootctl), offset);

    if (bootctl_fd >= 0) {
        close(bootctl_fd);
    }

    if (!read_ok) {
        ERROR_LOG("Failed to read the ukibootctl content");
        return -1;
    }

    if (!bootctl_validate(&bootctl)) {
        INFO_LOG("ukibootctl content is uninitialized or corrupted, "
                 "using ukiboot defaults");
        bootctl_init_default(&bootctl);
    }

    for (int i = 0; i < 2; i++) {
        INFO_LOG("ukibootctl slot %c: priority=%u tries_remaining=%u "
                 "successful_boot=%u",
                 uki_slot_letter(i),
                 bootctl.slots[i].priority,
                 bootctl.slots[i].tries_remaining,
                 bootctl.slots[i].successful_boot);
    }

    uint32_t active_slot;
    if (!bootctl_find_active_slot(&bootctl, &active_slot)) {
        ERROR_LOG("No valid slot in the ukibootctl content");
        return -1;
    }

    return (int)active_slot;
}

off_t uki_media_size(int fd) {
    struct stat st;

    /* st_size is only usable for regular files. Block devices either
     * report nothing, or something else entirely: QNX block devices
     * report their size in 512 byte blocks there (st_size == st_blocks),
     * which is 512 times smaller than the truth. */
    if (fstat(fd, &st) == 0 && S_ISREG(st.st_mode)) {
        return st.st_size;
    }

    off_t size = lseek(fd, 0, SEEK_END);

    if (size < 0) {
        DEBUG_LOG("lseek to the end failed: %s", strerror(errno));
        return 0;
    }

    (void)lseek(fd, 0, SEEK_SET);

    return size;
}

bool uki_slot_parse_sel(const char *str, uki_slot_sel_t *sel) {
    if (strcmp(str, "a") == 0 || strcmp(str, "A") == 0 ||
        strcmp(str, "0") == 0) {
        *sel = UKI_SLOT_SEL_A;
    } else if (strcmp(str, "b") == 0 || strcmp(str, "B") == 0 ||
               strcmp(str, "1") == 0) {
        *sel = UKI_SLOT_SEL_B;
    } else if (strcmp(str, "auto") == 0) {
        *sel = UKI_SLOT_SEL_AUTO;
    } else {
        return false;
    }

    return true;
}

bool uki_slot_locate(int image_fd, uki_slot_sel_t sel,
                     const char *bootctl_path, uki_slot_info_t *info) {
    DEBUG_ENTER();

    uki_partitions_t parts;
    bool parsed;

    memset(&parts, 0, sizeof(parts));
    memset(info, 0, sizeof(*info));
    info->active_slot = -1;

    if (image_is_gpt(image_fd)) {
        INFO_LOG("Reading GPT partition table");
        parsed = parse_gpt_partitions(image_fd, &parts);
    } else {
        INFO_LOG("No GPT found, reading MBR partition table");
        parsed = parse_mbr_partitions(image_fd, &parts);
    }

    if (!parsed) {
        DEBUG_EXIT(0);
        return false;
    }

    if (!parts.slot[0].found && !parts.slot[1].found) {
        ERROR_LOG("No '%s' or '%s' partition found in the image",
                  UKI_SLOT_A_PART_NAME, UKI_SLOT_B_PART_NAME);
        DEBUG_EXIT(0);
        return false;
    }

    int slot;

    if (sel == UKI_SLOT_SEL_AUTO) {
        info->active_slot = read_active_slot(image_fd, &parts, bootctl_path);
        if (info->active_slot < 0) {
            DEBUG_EXIT(0);
            return false;
        }
        slot = info->active_slot;
        INFO_LOG("Active ukiboot slot: %c", uki_slot_letter(slot));
    } else {
        slot = (int)sel;
        INFO_LOG("Selected ukiboot slot: %c", uki_slot_letter(slot));
    }

    if (!parts.slot[slot].found) {
        ERROR_LOG("No '%s' partition found in the image",
                  slot == 0 ? UKI_SLOT_A_PART_NAME : UKI_SLOT_B_PART_NAME);
        DEBUG_EXIT(0);
        return false;
    }

    info->slot = slot;
    info->offset = parts.slot[slot].offset;
    info->size = parts.slot[slot].size;

    INFO_LOG("ukiboot slot %c: offset %llu, size %llu bytes",
             uki_slot_letter(slot),
             (unsigned long long)info->offset,
             (unsigned long long)info->size);

    DEBUG_EXIT(1);
    return true;
}
