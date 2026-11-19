/*
 * test_slot.c - Host test program for the ukiboot slot selection
 *
 * Locates the UKI of a ukiboot slot inside a ukiboot disk image (GPT or
 * MBR) and lists the PE sections found at that location, exactly like
 * ukifs does before mounting them.
 *
 * Build: make test-slot
 * Usage: ./test_slot [options] <disk-image> [a|b|auto]
 */

#include <errno.h>
#include <fcntl.h>
#include <getopt.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <sys/stat.h>

#include "ukifs_size.h"
#include "ukifs_slot.h"
#include "ukifs_utils.h"

#include "pe_parser_posix.h"

/* Normally defined by ukifs_core.c */
int g_log_level = LOG_INFO;

#define OPT_BOOTCTL 256
#define OPT_CERT    257

static void usage(const char *argv0) {
    fprintf(stderr,
            "Usage: %s [options] <disk-image> [a|b|auto]\n"
            "\n"
            "Locate the UKI of a ukiboot slot in a disk image and list its sections.\n"
            "The slot defaults to 'auto' (active slot per the ukibootctl partition).\n"
            "\n"
            "Options:\n"
            "  --bootctl <path>  Standalone ukibootctl partition to use\n"
            "  --cert <cert.pem> Verify the signature of the UKI in the slot\n"
            "  -d <level>        Debug level: 0=quiet, 1=errors, 2=info, 3=debug\n"
            "  -h                Show this help\n",
            argv0);
}

int main(int argc, char *argv[]) {
    const char *bootctl_path = NULL;
    const char *cert_path = NULL;
    uki_slot_sel_t sel = UKI_SLOT_SEL_AUTO;
    int opt;

    static const struct option long_options[] = {
        {"bootctl", required_argument, NULL, OPT_BOOTCTL},
        {"cert",    required_argument, NULL, OPT_CERT},
        {"debug",   required_argument, NULL, 'd'},
        {"help",    no_argument,       NULL, 'h'},
        {NULL,      0,                 NULL, 0},
    };

    while ((opt = getopt_long(argc, argv, "d:h", long_options, NULL)) != -1) {
        switch (opt) {
        case OPT_BOOTCTL:
            bootctl_path = optarg;
            break;
        case OPT_CERT:
            cert_path = optarg;
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

    if (optind >= argc || optind + 2 < argc) {
        usage(argv[0]);
        return EXIT_FAILURE;
    }

    const char *image_path = argv[optind];

    if (optind + 1 < argc && !uki_slot_parse_sel(argv[optind + 1], &sel)) {
        fprintf(stderr, "ERROR: Invalid slot '%s' (expected a, b or auto)\n",
                argv[optind + 1]);
        return EXIT_FAILURE;
    }

    int fd = open(image_path, O_RDONLY);
    if (fd < 0) {
        fprintf(stderr, "ERROR: Failed to open '%s': %s\n",
                image_path, strerror(errno));
        return EXIT_FAILURE;
    }

    printf("========================================\n");
    printf("ukiboot slot selection\n");
    printf("========================================\n");
    printf("Image: %s\n", image_path);
    printf("Media size: %lld bytes\n", (long long)uki_media_size(fd));
    printf("Requested slot: %s\n",
           sel == UKI_SLOT_SEL_AUTO ? "auto" :
           sel == UKI_SLOT_SEL_A ? "a" : "b");
    if (bootctl_path) {
        printf("ukibootctl: %s\n", bootctl_path);
    }
    printf("\n");

    uki_slot_info_t info;
    if (!uki_slot_locate(fd, sel, bootctl_path, &info)) {
        fprintf(stderr, "ERROR: Failed to locate the ukiboot slot\n");
        close(fd);
        return EXIT_FAILURE;
    }

    printf("\n");
    printf("Selected slot:  %c\n", uki_slot_letter(info.slot));
    if (info.active_slot >= 0) {
        printf("Active slot:    %c\n", uki_slot_letter(info.active_slot));
    }
    printf("Slot offset:    %llu bytes\n", (unsigned long long)info.offset);
    printf("Slot size:      %llu bytes\n", (unsigned long long)info.size);

    /* Size of the UKI stored in the slot, padding excluded */
    uki_size_info_t size_info;
    if (uki_compute_size(fd, (off_t)info.offset, &size_info)) {
        printf("UKI size:       %u bytes (%u bytes of padding)\n",
               size_info.expected_size,
               (info.size > size_info.expected_size)
                   ? (unsigned)(info.size - size_info.expected_size) : 0);
        if (size_info.cert_table_size > 0) {
            printf("Certificate:    offset %u, size %u bytes\n",
                   size_info.cert_table_offset, size_info.cert_table_size);
        }
    }
    printf("\n");

    PeSectionInfo *sections = NULL;
    uint32_t n_sections = 0;

    if (!pe_parse_sections(fd, (off_t)info.offset, &sections, &n_sections)) {
        fprintf(stderr, "ERROR: Failed to parse the PE sections of slot %c\n",
                uki_slot_letter(info.slot));
        close(fd);
        return EXIT_FAILURE;
    }

    printf("%-16s %12s %12s %14s\n", "Section", "Offset", "Size", "Image offset");
    printf("--------------------------------------------------------------\n");
    for (uint32_t i = 0; i < n_sections; i++) {
        printf("%-16s %12u %12u %14llu\n",
               sections[i].name,
               sections[i].file_offset,
               sections[i].size,
               (unsigned long long)(info.offset + sections[i].file_offset));
    }

    free(sections);

    /* Verify the signature of the UKI where it sits in the image, which
     * is what ukifs does: the padding of the slot is not part of the
     * Authenticode digest. */
    if (cert_path) {
        printf("\n");
        if (!pe_verify_signature(fd, (off_t)info.offset, cert_path)) {
            fprintf(stderr, "SIGNATURE VERIFICATION FAILED\n");
            close(fd);
            return EXIT_FAILURE;
        }
        printf("Signature verification passed\n");
    }

    close(fd);

    return EXIT_SUCCESS;
}
