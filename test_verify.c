/*
 * Test UKI signature verification
 */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <fcntl.h>
#include <unistd.h>
#include <stdbool.h>
#include <stdarg.h>
#include <errno.h>
#include <sys/stat.h>

#include "ukifs_size.h"

#include "utils.h"
#include "pe_parser_posix.h"

static void print_usage(const char *prog) {
    fprintf(stderr, "Usage: %s [options] <uki-file> <cert.pem>\n", prog);
    fprintf(stderr, "\n");
    fprintf(stderr, "Test UKI signature verification.\n");
    fprintf(stderr, "\n");
    fprintf(stderr, "Options:\n");
    fprintf(stderr, "  -i    Ignore padding (verify only actual UKI, not extra padding)\n");
    fprintf(stderr, "  -h    Show this help\n");
    fprintf(stderr, "\n");
    fprintf(stderr, "Arguments:\n");
    fprintf(stderr, "  <uki-file>   Path to UKI file (PE executable)\n");
    fprintf(stderr, "  <cert.pem>   Path to X.509 PEM certificate\n");
    fprintf(stderr, "\n");
    fprintf(stderr, "Examples:\n");
    fprintf(stderr, "  %s /boot/archlinux.efi /etc/uki-cert.pem\n", prog);
    fprintf(stderr, "  %s -i ukiboot_a_signed.img rootCA.crt\n", prog);
    fprintf(stderr, "\n");
    fprintf(stderr, "The -i option computes the actual UKI size (excluding padding)\n");
    fprintf(stderr, "and verifies only that portion. Useful for UKI files that have\n");
    fprintf(stderr, "been padded to a fixed size (e.g., 128 MB) after signing.\n");
}

int main(int argc, char *argv[]) {
    bool ignore_padding = false;
    int opt;

    /* Parse options */
    while ((opt = getopt(argc, argv, "ih")) != -1) {
        switch (opt) {
            case 'i':
                ignore_padding = true;
                break;
            case 'h':
            default:
                print_usage(argv[0]);
                return (opt == 'h') ? EXIT_SUCCESS : EXIT_FAILURE;
        }
    }

    /* Check for required arguments */
    if (optind + 2 != argc) {
        print_usage(argv[0]);
        return EXIT_FAILURE;
    }

    const char *uki_path = argv[optind];
    const char *cert_path = argv[optind + 1];

    printf("========================================\n");
    printf("UKI Signature Verification Test\n");
    printf("========================================\n");
    printf("UKI file:     %s\n", uki_path);
    printf("Certificate:  %s\n", cert_path);
    if (ignore_padding) {
        printf("Mode:         Ignore padding (verify actual UKI only)\n");
    }
    printf("========================================\n\n");

    /* Open UKI file */
    int fd = open(uki_path, O_RDONLY);
    if (fd < 0) {
        fprintf(stderr, "ERROR: Failed to open UKI file '%s': %s\n",
                uki_path, strerror(errno));
        return EXIT_FAILURE;
    }

    /* Get actual file size */
    struct stat st;
    if (fstat(fd, &st) != 0) {
        fprintf(stderr, "ERROR: Failed to stat file: %s\n", strerror(errno));
        close(fd);
        return EXIT_FAILURE;
    }

    printf("Actual file size: %ld bytes (%.2f MB)\n",
           (long)st.st_size, st.st_size / 1024.0 / 1024.0);

    int verify_fd = fd;
    bool result = false;

    if (ignore_padding) {
        /* Compute UKI size */
        uki_size_info_t size_info;
        if (!uki_compute_size(fd, &size_info)) {
            fprintf(stderr, "ERROR: Failed to compute UKI size\n");
            close(fd);
            return EXIT_FAILURE;
        }

        printf("Computed UKI size: %u bytes (%.2f MB)\n",
               size_info.expected_size,
               size_info.expected_size / 1024.0 / 1024.0);

        if (size_info.cert_table_size > 0) {
            printf("  Certificate table: offset %u, size %u bytes\n",
                   size_info.cert_table_offset, size_info.cert_table_size);
        }

        if ((off_t)size_info.expected_size < st.st_size) {
            off_t padding = st.st_size - size_info.expected_size;
            printf("Extra padding: %ld bytes (%.2f MB, %.1f%% of file)\n",
                   (long)padding,
                   padding / 1024.0 / 1024.0,
                   (padding * 100.0) / st.st_size);
        }

        /* Create a temporary file with just the UKI data (no padding) */
        char temp_template[] = "/tmp/uki_verify_XXXXXX";
        int temp_fd = mkstemp(temp_template);
        if (temp_fd < 0) {
            fprintf(stderr, "ERROR: Failed to create temporary file: %s\n", strerror(errno));
            close(fd);
            return EXIT_FAILURE;
        }

        /* Unlink immediately so it's cleaned up on exit */
        unlink(temp_template);

        /* Copy UKI data to temporary file */
        printf("\nCopying UKI data (%u bytes) to temporary file for verification...\n",
               size_info.expected_size);

        off_t remaining = size_info.expected_size;
        off_t offset = 0;
        char buffer[64 * 1024];

        while (remaining > 0) {
            size_t to_read = (remaining > (off_t)sizeof(buffer)) ? sizeof(buffer) : (size_t)remaining;
            ssize_t nread = pread(fd, buffer, to_read, offset);
            if (nread <= 0) {
                fprintf(stderr, "ERROR: Failed to read from UKI file: %s\n", strerror(errno));
                close(temp_fd);
                close(fd);
                return EXIT_FAILURE;
            }

            ssize_t nwritten = write(temp_fd, buffer, nread);
            if (nwritten != nread) {
                fprintf(stderr, "ERROR: Failed to write to temporary file: %s\n", strerror(errno));
                close(temp_fd);
                close(fd);
                return EXIT_FAILURE;
            }

            offset += nread;
            remaining -= nread;
        }

        /* Use the temporary file for verification */
        verify_fd = temp_fd;
        lseek(verify_fd, 0, SEEK_SET);
    }

    /* Verify signature */
    printf("\nStarting signature verification...\n\n");
    result = pe_verify_signature(verify_fd, 0, cert_path);

    if (ignore_padding && verify_fd != fd) {
        close(verify_fd);
    }
    close(fd);

    printf("\n========================================\n");
    if (result) {
        printf("✓ SIGNATURE VERIFICATION PASSED\n");
        printf("========================================\n");
        printf("\nThe UKI file is properly signed and the signature is valid.\n");
        return EXIT_SUCCESS;
    } else {
        printf("✗ SIGNATURE VERIFICATION FAILED\n");
        printf("========================================\n");
        printf("\nThe UKI file is either not signed, improperly signed,\n");
        printf("or the signature does not match the provided certificate.\n");
        return EXIT_FAILURE;
    }
}
