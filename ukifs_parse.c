/*
 * PE/COFF Parsing for UKI Files
 *
 * The PE/COFF headers are decoded by ukiboot's PE parser, which reads
 * the sections of a UKI located at an arbitrary offset in a file. This
 * is what allows parsing a UKI stored in a ukiboot slot partition.
 */

#include <stdlib.h>
#include <string.h>

#include "ukifs_parse.h"
#include "ukifs_core.h"

#include "pe_parser_posix.h"
#include "utils.h"

/*
 * Create a UKI entry from a PE section
 */
static uki_entry_t *create_uki_entry(const PeSectionInfo *section,
                                     const struct stat *file_stat,
                                     uki_context_t *ctx) {
    DEBUG_ENTER();

    /* Sanitize name (strip leading '.') */
    char *name = sanitize_section_name(section->name, PE_SECTION_NAME_SIZE);
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
    entry->file_offset = section->file_offset;  /* Relative to the UKI */
    entry->size = section->size;
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
 * Parse PE sections from the UKI
 * Returns 0 on success, -1 on error
 */
int uki_parse_sections(uki_context_t *ctx) {
    DEBUG_ENTER();

    if (!ctx || ctx->uki_fd < 0) {
        ERROR_LOG("Invalid context");
        DEBUG_EXIT(-1);
        return -1;
    }

    /* Timestamps of the exposed files, from the source of the UKI */
    struct stat *file_stat = &ctx->src_stat;

    /* Read the section headers (ukiboot's PE parser) */
    PeSectionInfo *sections = NULL;
    uint32_t num_sections = 0;

    if (!pe_parse_sections(ctx->uki_fd, ctx->uki_offset,
                           &sections, &num_sections)) {
        ERROR_LOG("Failed to parse the PE sections of the UKI");
        DEBUG_EXIT(-1);
        return -1;
    }

    DEBUG_VAL("num_sections", num_sections);

    if (num_sections == 0) {
        ERROR_LOG("No sections found in UKI file");
        free(sections);
        DEBUG_EXIT(-1);
        return -1;
    }

    /* Create root directory */
    ctx->root = create_dir_node("/");
    if (!ctx->root) {
        ERROR_LOG("Failed to create root directory");
        free(sections);
        DEBUG_EXIT(-1);
        return -1;
    }

    /* Process each section */
    uint32_t valid_sections = 0;
    size_t extent = 0;
    for (uint32_t i = 0; i < num_sections; i++) {
        uki_entry_t *entry = create_uki_entry(&sections[i], file_stat, ctx);
        if (!entry) {
            continue;  /* Skip invalid sections */
        }

        /* Validate offset and size against the size of the UKI */
        if ((size_t)entry->file_offset + entry->size > ctx->uki_size) {
            ERROR_LOG("Warning: Section '%s' extends beyond the UKI, skipping",
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

        if ((size_t)entry->file_offset + entry->size > extent) {
            extent = (size_t)entry->file_offset + entry->size;
        }

        valid_sections++;
    }

    free(sections);

    ctx->num_sections = valid_sections;
    ctx->uki_extent = extent;
    DEBUG_VAL("uki_extent", extent);
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
