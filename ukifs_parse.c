/*
 * PE/COFF Parsing for UKI Files
 */

#include <stdlib.h>
#include <string.h>

#include "ukifs_parse.h"
#include "ukifs_core.h"
#include "ukifs_pe.h"

#include "utils.h"

/*
 * Read and validate PE headers (DOS + NT)
 * Returns 0 on success, -1 on error
 */
static int pe_read_headers(int fd,
                           off_t base_offset,
                           uint32_t *nt_offset_out,
                           ImageNtHeaders *nt_out) {
    DEBUG_ENTER();

    /* Read DOS header */
    ImageDosHeader dos_header;
    if (!full_pread(fd, &dos_header, sizeof(dos_header), base_offset)) {
        ERROR_LOG("Failed to read DOS header");
        DEBUG_EXIT(-1);
        return -1;
    }

    /* Validate DOS signature */
    if (le16toh(dos_header.e_magic) != IMAGE_DOS_SIGNATURE) {
        ERROR_LOG("Invalid DOS signature: 0x%x (expected 0x%x)",
                  le16toh(dos_header.e_magic), IMAGE_DOS_SIGNATURE);
        DEBUG_EXIT(-1);
        return -1;
    }

    uint32_t nt_offset = (uint32_t)le32toh(dos_header.e_lfanew);
    DEBUG_VAL("nt_offset", nt_offset);

    /* Read NT header */
    ImageNtHeaders nt_header;
    if (!full_pread(fd, &nt_header, sizeof(nt_header), base_offset + nt_offset)) {
        ERROR_LOG("Failed to read NT header at offset %u", nt_offset);
        DEBUG_EXIT(-1);
        return -1;
    }

    /* Validate PE signature */
    if (le32toh(nt_header.Signature) != IMAGE_NT_SIGNATURE) {
        ERROR_LOG("Invalid PE signature: 0x%x (expected 0x%x)",
                  le32toh(nt_header.Signature), IMAGE_NT_SIGNATURE);
        DEBUG_EXIT(-1);
        return -1;
    }

    *nt_offset_out = nt_offset;
    *nt_out = nt_header;

    DEBUG_EXIT(0);
    return 0;
}

/*
 * Create a UKI entry from a PE section
 */
static uki_entry_t *create_uki_entry(const ImageSectionHeader *section,
                                     off_t base_offset UNUSED,
                                     struct stat *file_stat,
                                     uki_context_t *ctx) {
    DEBUG_ENTER();

    /* Extract section name (8 bytes, not null-terminated) */
    char raw_name[PE_SECTION_NAME_SIZE + 1];
    memcpy(raw_name, section->Name, PE_SECTION_NAME_SIZE);
    raw_name[PE_SECTION_NAME_SIZE] = '\0';

    /* Sanitize name (strip leading '.') */
    char *name = sanitize_section_name(raw_name, PE_SECTION_NAME_SIZE);
    if (!name) {
        DEBUG_LOG("Failed to sanitize section name");
        DEBUG_EXIT(0);
        return NULL;
    }

    /* Skip sections with empty names */
    if (strlen(name) == 0) {
        DEBUG_LOG("Skipping section with empty name");
        free(name);
        DEBUG_EXIT(0);
        return NULL;
    }

    /* Allocate entry */
    uki_entry_t *entry = calloc(1, sizeof(uki_entry_t));
    if (!entry) {
        DEBUG_LOG("Failed to allocate UKI entry");
        free(name);
        DEBUG_EXIT(0);
        return NULL;
    }

    entry->name = name;
    entry->file_offset = le32toh(section->PointerToRawData);
    entry->size = le32toh(section->Misc.VirtualSize);
    entry->mode = S_IFREG | ctx->perm;  /* Regular file */
    entry->mtime = file_stat->st_mtime;

    /* Initialize iofunc attributes */
    iofunc_attr_init(&entry->attr, entry->mode, 0, 0);
    entry->attr.nbytes = entry->size;
    entry->attr.mtime = entry->mtime;
    entry->attr.ctime = file_stat->st_ctime;
    entry->attr.atime = file_stat->st_atime;
    entry->attr.mount = &ctx->mount;  /* Critical: needed for context recovery */

    DEBUG_STR("section name", entry->name);
    DEBUG_VAL("file_offset", entry->file_offset);
    DEBUG_VAL("size", entry->size);

    DEBUG_EXIT(0);
    return entry;
}

/*
 * Parse PE sections from UKI file
 * Returns 0 on success, -1 on error
 */
int uki_parse_sections(uki_context_t *ctx) {
    DEBUG_ENTER();

    if (!ctx || ctx->uki_fd < 0) {
        ERROR_LOG("Invalid context");
        DEBUG_EXIT(-1);
        return -1;
    }

    /* Get file stats for timestamps */
    struct stat file_stat;
    if (fstat(ctx->uki_fd, &file_stat) != 0) {
        ERROR_LOG("Failed to stat UKI file: %s", strerror(errno));
        DEBUG_EXIT(-1);
        return -1;
    }

    /* Read and validate PE headers */
    uint32_t nt_offset;
    ImageNtHeaders nt_header;
    if (pe_read_headers(ctx->uki_fd, 0, &nt_offset, &nt_header) != 0) {
        DEBUG_EXIT(-1);
        return -1;
    }

    /* Extract section information */
    uint16_t opt_header_size = le16toh(nt_header.FileHeader.SizeOfOptionalHeader);
    uint16_t num_sections = le16toh(nt_header.FileHeader.NumberOfSections);
    uint32_t sections_offset = nt_offset + sizeof(ImageNtHeaders) + opt_header_size;

    DEBUG_VAL("num_sections", num_sections);
    DEBUG_VAL("sections_offset", sections_offset);

    if (num_sections == 0) {
        ERROR_LOG("No sections found in UKI file");
        DEBUG_EXIT(-1);
        return -1;
    }

    /* Read all section headers */
    ImageSectionHeader *raw_sections = calloc(num_sections, sizeof(ImageSectionHeader));
    if (!raw_sections) {
        ERROR_LOG("Failed to allocate section headers");
        DEBUG_EXIT(-1);
        return -1;
    }

    if (!full_pread(ctx->uki_fd, raw_sections,
                    num_sections * sizeof(ImageSectionHeader),
                    sections_offset)) {
        ERROR_LOG("Failed to read section headers");
        free(raw_sections);
        DEBUG_EXIT(-1);
        return -1;
    }

    /* Create root directory */
    ctx->root = create_dir_node("/");
    if (!ctx->root) {
        ERROR_LOG("Failed to create root directory");
        free(raw_sections);
        DEBUG_EXIT(-1);
        return -1;
    }

    /* Process each section */
    uint32_t valid_sections = 0;
    for (uint16_t i = 0; i < num_sections; i++) {
        uki_entry_t *entry = create_uki_entry(&raw_sections[i], 0, &file_stat, ctx);
        if (!entry) {
            continue;  /* Skip invalid sections */
        }

        /* Validate offset and size against file size */
        if (entry->file_offset + entry->size > ctx->mmap_size) {
            ERROR_LOG("Warning: Section '%s' extends beyond file size, skipping",
                      entry->name);
            free(entry->name);
            free(entry);
            continue;
        }

        /* Add to hash table */
        add_to_hash_table(ctx, entry);

        /* Add to directory tree */
        if (add_to_dir_tree(ctx->root, entry) != 0) {
            ERROR_LOG("Failed to add section '%s' to directory tree",
                      entry->name);
            free(entry->name);
            free(entry);
            continue;
        }

        valid_sections++;
    }

    free(raw_sections);

    ctx->num_sections = valid_sections;
    DEBUG_VAL("valid_sections", valid_sections);

    if (valid_sections == 0) {
        ERROR_LOG("No valid sections found in UKI file");
        DEBUG_EXIT(-1);
        return -1;
    }

    INFO_LOG("Parsed %u sections from UKI file", valid_sections);
    DEBUG_EXIT(0);
    return 0;
}
