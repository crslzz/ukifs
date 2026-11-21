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
 * Add a file that is not a section of the UKI to the filesystem.
 *
 * Its content is either held in memory or produced when it is first
 * read; 'size' is what the file reports, whichever way. The caller
 * fills data or zboot in once the entry is back.
 *
 * Returns the entry, NULL on error.
 */
static uki_entry_t *add_synth_entry(uki_context_t *ctx, const char *name,
                                    uint32_t size,
                                    const struct stat *file_stat) {
    DEBUG_ENTER();

    uki_entry_t *entry = calloc(1, sizeof(uki_entry_t));

    if (!entry) {
        ERROR_LOG("Failed to allocate the %s entry", name);
        DEBUG_EXIT(0);
        return NULL;
    }

    entry->name = strdup(name);

    if (!entry->name) {
        ERROR_LOG("Failed to allocate the name of %s", name);
        free(entry);
        DEBUG_EXIT(0);
        return NULL;
    }

    entry->size = size;
    entry->mode = S_IFREG | ctx->perm;
    entry->mtime = file_stat->st_mtime;

    iofunc_attr_init(&entry->attr, entry->mode, 0, 0);
    entry->attr.nbytes = entry->size;
    entry->attr.mtime = entry->mtime;
    entry->attr.ctime = file_stat->st_ctime;
    entry->attr.atime = file_stat->st_atime;
    entry->attr.mount = &ctx->mount;

    add_to_hash_table(ctx, entry);

    if (add_to_dir_tree(ctx->root, entry) != 0) {
        ERROR_LOG("Failed to add %s to the directory tree", name);
        /* The entry stays in the hash table, where it is reachable and
         * harmless; removing it would need a lookup it has no use for */
        DEBUG_EXIT(0);
        return NULL;
    }

    DEBUG_EXIT(0);
    return entry;
}

/*
 * Build the kernel command line of a ukiboot slot.
 *
 * ukiboot boots a slot by chain loading it as ukiboot_a.efi or
 * ukiboot_b.efi, which makes the stub load the matching addon and
 * appends androidboot.slot_suffix to the command line. Nothing loads
 * that addon here, so the extended command line is synthesized into
 * cmdline.ukiboot, next to the plain cmdline section, and a caller
 * passing it to a kernel gets what ukiboot would have booted with.
 */
static int add_slot_cmdline(uki_context_t *ctx, const uki_entry_t *cmdline,
                            const struct stat *file_stat) {
    DEBUG_ENTER();

    const char *suffix = (ctx->slot == 1) ? UKI_SLOT_SUFFIX_B : UKI_SLOT_SUFFIX_A;

    char *content = malloc((size_t)cmdline->size + strlen(suffix) + 1);
    if (!content) {
        ERROR_LOG("Failed to allocate the command line of slot %c",
                  uki_slot_letter(ctx->slot));
        DEBUG_EXIT(-1);
        return -1;
    }

    if (cmdline->size > 0 &&
        !full_pread(ctx->uki_fd, content, cmdline->size,
                    ctx->uki_offset + (off_t)cmdline->file_offset)) {
        ERROR_LOG("Failed to read the cmdline section");
        free(content);
        DEBUG_EXIT(-1);
        return -1;
    }

    /* A command line is a single line: drop what a section may carry
     * after it, so that the suffix is not appended behind a terminator */
    size_t len = cmdline->size;
    while (len > 0 && (content[len - 1] == '\0' || content[len - 1] == '\n' ||
                       content[len - 1] == '\r' || content[len - 1] == ' ')) {
        len--;
    }

    strcpy(content + len, suffix);

    uki_entry_t *entry = add_synth_entry(ctx, UKI_SLOT_CMDLINE_NAME,
                                         (uint32_t)strlen(content), file_stat);

    if (!entry) {
        free(content);
        DEBUG_EXIT(-1);
        return -1;
    }

    entry->data = content;

    INFO_LOG("Added %s for slot %c (%u bytes)", UKI_SLOT_CMDLINE_NAME,
             uki_slot_letter(ctx->slot), entry->size);

    DEBUG_EXIT(0);
    return 0;
}

/*
 * Expose the kernel of an EFI zboot image as vmlinux.
 *
 * A kernel section built with CONFIG_EFI_ZBOOT is an EFI application
 * carrying a compressed Image, which only an EFI firmware can boot.
 * The Image is unwrapped from it and served as vmlinux, so that a
 * loader which is not EFI, such as qvm, has a kernel it can start.
 *
 * Nothing is added when the kernel is a bare Image already, which is
 * the usual case: vmlinux would then be a second name for linux.
 *
 * A kernel that cannot be unwrapped is not an error: linux is still
 * served, and the reason is logged. Returns 0 unless the filesystem
 * itself could not be built.
 */
static int add_zboot_kernel(uki_context_t *ctx, const uki_entry_t *kernel,
                            const struct stat *file_stat) {
    DEBUG_ENTER();

    uki_zboot_t *zb = uki_zboot_probe(ctx->uki_fd,
                                      ctx->uki_offset +
                                      (off_t)kernel->file_offset,
                                      kernel->size);

    if (!zb) {
        DEBUG_EXIT(0);
        return 0;
    }

    uki_entry_t *entry = add_synth_entry(ctx, UKI_ZBOOT_NAME, zb->image_size,
                                         file_stat);

    if (!entry) {
        uki_zboot_free(zb);
        DEBUG_EXIT(-1);
        return -1;
    }

    /* The kernel is decompressed when vmlinux is first read, not here:
     * it is tens of megabytes, and a client that only reads cmdline or
     * uname should not pay for it */
    entry->zboot = zb;

    INFO_LOG("Added %s, the kernel unwrapped from the EFI zboot image "
             "(%u bytes)", UKI_ZBOOT_NAME, entry->size);

    DEBUG_EXIT(0);
    return 0;
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
    const uki_entry_t *cmdline = NULL;
    const uki_entry_t *kernel = NULL;
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

        if (strcmp(entry->name, "cmdline") == 0) {
            cmdline = entry;
        }

        if (strcmp(entry->name, UKI_KERNEL_NAME) == 0) {
            kernel = entry;
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

    /* A slot of a ukiboot image boots with a slot suffix appended to
     * its command line; expose what that command line would be */
    if (ctx->slot >= 0) {
        if (cmdline) {
            if (add_slot_cmdline(ctx, cmdline, file_stat) != 0) {
                DEBUG_EXIT(-1);
                return -1;
            }
        } else {
            INFO_LOG("No cmdline section, not adding %s",
                     UKI_SLOT_CMDLINE_NAME);
        }
    }

    /* A kernel that an EFI firmware would have to decompress is
     * unwrapped, so that a loader which is not EFI can boot it */
    if (kernel) {
        if (add_zboot_kernel(ctx, kernel, file_stat) != 0) {
            DEBUG_EXIT(-1);
            return -1;
        }
    } else {
        INFO_LOG("No %s section, not looking for an EFI zboot kernel",
                 UKI_KERNEL_NAME);
    }

    INFO_LOG("Parsed %u sections from UKI file", valid_sections);
    DEBUG_EXIT(0);
    return 0;
}
