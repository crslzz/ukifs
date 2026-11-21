/*
 * test_zboot.c - Host test program for the EFI zboot kernel support
 *
 * Looks at the kernel section of a UKI, which may be a plain UKI file
 * or the one in a ukiboot slot, and reports whether it is an EFI zboot
 * image. When it is, the kernel is unwrapped exactly as ukifs does to
 * serve its vmlinux file, and can be written out to be compared with
 * the kernel it was built from.
 *
 * Build: make test-zboot
 * Usage: ./test_zboot [options] <uki-file|ukiboot-image>
 */

#include <errno.h>
#include <fcntl.h>
#include <getopt.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <sys/stat.h>

#include "ukifs_slot.h"
#include "ukifs_utils.h"
#include "ukifs_zboot.h"

#include "pe_parser_posix.h"

/* Normally defined by ukifs_core.c */
int g_log_level = LOG_INFO;

#define OPT_BOOTCTL 256

static void usage(const char *argv0) {
    fprintf(stderr,
            "Usage: %s [options] <uki-file|ukiboot-image>\n"
            "\n"
            "Report whether the kernel section of a UKI is an EFI zboot image,\n"
            "and unwrap the kernel from it the way ukifs serves %s.\n"
            "\n"
            "Options:\n"
            "  -s, --slot <a|b|auto>  Read the UKI from a ukiboot slot\n"
            "  -A, --slot-a           Same as --slot a\n"
            "  -B, --slot-b           Same as --slot b\n"
            "      --bootctl <path>   Standalone ukibootctl partition to use\n"
            "  -o, --output <path>    Write the unwrapped kernel there\n"
            "  -d, --debug <level>    0=quiet, 1=errors, 2=info, 3=debug\n"
            "  -h, --help             Show this help\n"
            "\n"
            "Exit status: 0 if a kernel was unwrapped (and written),\n"
            "             1 on error, 2 if there is nothing to unwrap.\n",
            argv0, UKI_ZBOOT_NAME);
}

/* Exit status telling a caller that the kernel needs no unwrapping */
#define EXIT_NOT_ZBOOT 2

/*
 * Position and size of the kernel section of the UKI at uki_offset.
 * Returns false when the UKI has no such section.
 */
static bool find_kernel(int fd, off_t uki_offset, off_t *pos, uint32_t *size) {
    PeSectionInfo *sections = NULL;
    uint32_t n_sections = 0;

    if (!pe_parse_sections(fd, uki_offset, &sections, &n_sections)) {
        fprintf(stderr, "ERROR: Failed to parse the PE sections of the UKI\n");
        return false;
    }

    bool found = false;

    for (uint32_t i = 0; i < n_sections && !found; i++) {
        const char *name = sections[i].name;

        /* The parser keeps the leading '.' that ukifs strips */
        if (name[0] == '.') {
            name++;
        }

        if (strcmp(name, UKI_KERNEL_NAME) == 0) {
            *pos = uki_offset + (off_t)sections[i].file_offset;
            *size = sections[i].size;
            found = true;
        }
    }

    free(sections);

    if (!found) {
        fprintf(stderr, "ERROR: The UKI has no %s section\n", UKI_KERNEL_NAME);
    }

    return found;
}

/* Write the unwrapped kernel, '-' meaning standard output */
static bool write_image(const char *path, const char *image, uint32_t size) {
    FILE *out = (strcmp(path, "-") == 0) ? stdout : fopen(path, "wb");

    if (!out) {
        fprintf(stderr, "ERROR: Failed to create '%s': %s\n",
                path, strerror(errno));
        return false;
    }

    bool ok = fwrite(image, 1, size, out) == size;

    if (!ok) {
        fprintf(stderr, "ERROR: Failed to write '%s': %s\n",
                path, strerror(errno));
    }

    if (out != stdout) {
        fclose(out);
    } else {
        fflush(out);
    }

    return ok;
}

int main(int argc, char *argv[]) {
    const char *bootctl_path = NULL;
    const char *output_path = NULL;
    uki_slot_sel_t sel = UKI_SLOT_SEL_NONE;
    int opt;

    static const struct option long_options[] = {
        {"slot",    required_argument, NULL, 's'},
        {"slot-a",  no_argument,       NULL, 'A'},
        {"slot-b",  no_argument,       NULL, 'B'},
        {"bootctl", required_argument, NULL, OPT_BOOTCTL},
        {"output",  required_argument, NULL, 'o'},
        {"debug",   required_argument, NULL, 'd'},
        {"help",    no_argument,       NULL, 'h'},
        {NULL,      0,                 NULL, 0},
    };

    while ((opt = getopt_long(argc, argv, "s:ABo:d:h",
                              long_options, NULL)) != -1) {
        switch (opt) {
        case 's':
            if (!uki_slot_parse_sel(optarg, &sel)) {
                fprintf(stderr, "ERROR: Invalid slot '%s'\n", optarg);
                return EXIT_FAILURE;
            }
            break;
        case 'A':
            sel = UKI_SLOT_SEL_A;
            break;
        case 'B':
            sel = UKI_SLOT_SEL_B;
            break;
        case OPT_BOOTCTL:
            bootctl_path = optarg;
            sel = UKI_SLOT_SEL_AUTO;
            break;
        case 'o':
            output_path = optarg;
            break;
        case 'd':
            g_log_level = atoi(optarg);
            break;
        case 'h':
        default:
            usage(argv[0]);
            return EXIT_FAILURE;
        }
    }

    if (optind + 1 != argc) {
        usage(argv[0]);
        return EXIT_FAILURE;
    }

    const char *path = argv[optind];
    int fd = open(path, O_RDONLY);

    if (fd < 0) {
        fprintf(stderr, "ERROR: Failed to open '%s': %s\n",
                path, strerror(errno));
        return EXIT_FAILURE;
    }

    /* Where the UKI starts: the whole file, or the selected slot */
    off_t uki_offset = 0;

    if (sel != UKI_SLOT_SEL_NONE) {
        uki_slot_info_t info;

        if (!uki_slot_locate(fd, sel, bootctl_path, &info)) {
            fprintf(stderr, "ERROR: Failed to locate the ukiboot slot\n");
            close(fd);
            return EXIT_FAILURE;
        }

        uki_offset = (off_t)info.offset;
    }

    off_t kernel_pos = 0;
    uint32_t kernel_size = 0;

    if (!find_kernel(fd, uki_offset, &kernel_pos, &kernel_size)) {
        close(fd);
        return EXIT_FAILURE;
    }

    printf("========================================\n");
    printf("EFI zboot kernel\n");
    printf("========================================\n");
    printf("UKI:            %s\n", path);
    printf("UKI offset:     %lld bytes\n", (long long)uki_offset);
    printf("Kernel section: %s, offset %lld, %u bytes\n", UKI_KERNEL_NAME,
           (long long)kernel_pos, kernel_size);
    printf("\n");

    uki_zboot_t *zb = uki_zboot_probe(fd, kernel_pos, kernel_size);

    if (!zb) {
        printf("EFI zboot:      no, nothing to unwrap\n");
        close(fd);
        return EXIT_NOT_ZBOOT;
    }

    printf("EFI zboot:      yes\n");
    printf("Compression:    %s\n", zb->comp_type);
    printf("Payload:        offset %lld, %u bytes\n",
           (long long)zb->payload_pos, zb->payload_size);
    printf("%-15s %u bytes\n", UKI_ZBOOT_NAME ":", zb->image_size);

    const char *image = uki_zboot_image(zb);
    int status = EXIT_SUCCESS;

    if (!image) {
        fprintf(stderr, "ERROR: Failed to unwrap the kernel\n");
        status = EXIT_FAILURE;
    } else {
        /* An arm64 Image carries 'ARMd' at offset 56 */
        printf("Unwrapped:      %s\n",
               memcmp(image + 56, "ARMd", 4) == 0 ? "an arm64 Image"
                                                  : "not an arm64 Image");

        if (output_path && !write_image(output_path, image, zb->image_size)) {
            status = EXIT_FAILURE;
        }
    }

    uki_zboot_free(zb);
    close(fd);

    return status;
}
