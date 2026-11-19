/*
 * ukifs_slot.h - ukiboot A/B slot support
 *
 * Locates the UKI stored in one of the two slots (ukiboot_a / ukiboot_b)
 * of a ukiboot disk image, as defined by the ukiboot project:
 *   https://gitlab.com/CentOS/automotive/src/ukiboot
 *
 * Both partitioning schemes described by ukiboot are supported:
 *   - GPT: partitions are found by name ("ukiboot_a", "ukiboot_b",
 *     "ukibootctl"), using ukiboot's own GPT parser.
 *   - MBR: the slots are logical partitions of an extended partition,
 *     identified by their MBR partition id (0x46, 0x47 and 0x48).
 *
 * The active slot is selected from the content of the ukibootctl
 * partition, using ukiboot's UkiBootCtrl definitions and semantics.
 */

#ifndef UKIFS_SLOT_H
#define UKIFS_SLOT_H

#include <stdbool.h>
#include <stdint.h>
#include <sys/types.h>

/* Slot selection, as requested on the command line */
typedef enum {
    UKI_SLOT_SEL_NONE = -1,  /* Not a ukiboot image: the file is a plain UKI */
    UKI_SLOT_SEL_A    = 0,   /* Force slot A (ukiboot_a) */
    UKI_SLOT_SEL_B    = 1,   /* Force slot B (ukiboot_b) */
    UKI_SLOT_SEL_AUTO = 2,   /* Select active slot from ukibootctl */
} uki_slot_sel_t;

/* GPT partition names (ukiboot README) */
#define UKI_SLOT_A_PART_NAME  "ukiboot_a"
#define UKI_SLOT_B_PART_NAME  "ukiboot_b"
#define UKI_BOOTCTL_PART_NAME "ukibootctl"

/* MBR partition ids (ukiboot README) */
#define UKI_SLOT_A_MBR_ID  0x46
#define UKI_SLOT_B_MBR_ID  0x47
#define UKI_BOOTCTL_MBR_ID 0x48

/* Result of the slot lookup */
typedef struct {
    int slot;           /* Selected slot: 0 = a, 1 = b */
    uint64_t offset;    /* Byte offset of the slot partition in the image */
    uint64_t size;      /* Byte size of the slot partition */
    int active_slot;    /* Active slot per ukibootctl, -1 if not consulted */
} uki_slot_info_t;

/*
 * Locate the UKI of a ukiboot slot inside a disk image.
 *
 * Parameters:
 *   image_fd     - Open file descriptor on the ukiboot disk image (or device)
 *   sel          - Requested slot (A, B or AUTO)
 *   bootctl_path - Optional path to a standalone ukibootctl partition,
 *                  used instead of the one found in the image (may be NULL)
 *   info         - Output: selected slot, offset and size
 *
 * Returns:
 *   true on success, false on error (no partition table, missing partition,
 *   unreadable or slot-less ukibootctl content, ...)
 */
bool uki_slot_locate(int image_fd, uki_slot_sel_t sel,
                     const char *bootctl_path, uki_slot_info_t *info);

/*
 * Size in bytes of the media behind a file descriptor.
 *
 * A block device usually has no size in its stat data, so this falls
 * back to seeking to the end of the device. Returns 0 when the size
 * cannot be determined.
 */
off_t uki_media_size(int fd);

/* Parse a slot selection string ("a", "b", "auto", "0", "1") */
bool uki_slot_parse_sel(const char *str, uki_slot_sel_t *sel);

/* Slot name ('a' or 'b') for logging */
static inline char uki_slot_letter(int slot) {
    return (slot == 1) ? 'b' : 'a';
}

#endif /* UKIFS_SLOT_H */
