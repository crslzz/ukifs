/*
 * ukiextract.c - Extract the UKI of a ukiboot slot
 *
 * Standalone Linux utility that extracts the UKI stored in the
 * ukiboot_a or ukiboot_b slot of a ukiboot disk image, as defined by
 *   https://gitlab.com/CentOS/automotive/src/ukiboot
 *
 * The slot is located, selected and verified with the very same code as
 * the ukifs QNX filesystem: ukifs_slot.c for the partition table and the
 * ukibootctl content, ukifs_size.c for the size of the UKI, and
 * ukiboot's pe_parser_posix.c for the sections and the signature.
 *
 * Build: make ukiextract
 */

#include "config.h"

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

/* ukiboot */
#include "pe_parser_posix.h"
#include "utils.h"

/* Normally defined by ukifs_core.c */
int g_log_level = LOG_ERROR;

#define COPY_BUF_SIZE (64 * 1024)

#define OPT_BOOTCTL    256
#define OPT_WHOLE_SLOT 257

static void usage(const char *argv0) {
    fprintf(stderr,
            "Usage: %s [options] <ukiboot-image>\n"
            "\n"
            "Extract the UKI stored in a slot of a ukiboot disk image.\n"
            "The image may be a file or a block device, with a GPT or an\n"
            "MBR partition table.\n"
            "\n"
            "Options:\n"
            "  -s, --slot <a|b|auto>  Slot to extract (default: auto)\n"
            "  -A, --slot-a           Same as --slot a\n"
            "  -B, --slot-b           Same as --slot b\n"
            "      --bootctl <path>   Standalone ukibootctl partition to use\n"
            "                         for --slot auto\n"
            "  -o, --output <path>    Output file, '-' for stdout\n"
            "                         (default: %s / %s)\n"
            "  -c, --cert <cert.pem>  Verify the UKI signature before extracting\n"
            "      --whole-slot       Extract the whole slot partition,\n"
            "                         padding included\n"
            "  -v, --verbose          Verbose mode (info messages)\n"
            "  -d, --debug <level>    Debug level: 0=quiet, 1=errors, 2=info, 3=debug\n"
            "  -h, --help             Show this help\n"
            "\n"
            "Examples:\n"
            "  %s /dev/sda                        # active slot -> %s or %s\n"
            "  %s --slot-b -o uki.efi disk.img    # slot b -> uki.efi\n"
            "  %s -c cert.pem -o - disk.img | sha256sum\n",
            argv0,
            UKI_SLOT_A_PART_NAME ".efi", UKI_SLOT_B_PART_NAME ".efi",
            argv0,
            UKI_SLOT_A_PART_NAME ".efi", UKI_SLOT_B_PART_NAME ".efi",
            argv0, argv0);
}

/* Copy 'length' bytes at 'offset' of in_fd to out_fd */
static bool copy_range(int in_fd, off_t offset, uint64_t length, int out_fd) {
    char *buffer = malloc(COPY_BUF_SIZE);

    if (!buffer) {
        ERROR_LOG("Failed to allocate the copy buffer");
        return false;
    }

    while (length > 0) {
        size_t chunk = (length > COPY_BUF_SIZE) ? COPY_BUF_SIZE : (size_t)length;

        if (!full_pread(in_fd, buffer, chunk, offset)) {
            ERROR_LOG("Failed to read the UKI at offset %lld", (long long)offset);
            free(buffer);
            return false;
        }

        if (!full_write(out_fd, buffer, chunk)) {
            ERROR_LOG("Failed to write the UKI: %s", strerror(errno));
            free(buffer);
            return false;
        }

        offset += chunk;
        length -= chunk;
    }

    free(buffer);
    return true;
}

int main(int argc, char *argv[]) {
    uki_slot_sel_t sel = UKI_SLOT_SEL_AUTO;
    const char *bootctl_path = NULL;
    const char *output_path = NULL;
    const char *cert_path = NULL;
    bool whole_slot = false;
    int opt;

    static const struct option long_options[] = {
        {"slot",        required_argument, NULL, 's'},
        {"slot-a",      no_argument,       NULL, 'A'},
        {"slot-b",      no_argument,       NULL, 'B'},
        {"bootctl",     required_argument, NULL, OPT_BOOTCTL},
        {"output",      required_argument, NULL, 'o'},
        {"cert",        required_argument, NULL, 'c'},
        {"whole-slot",  no_argument,       NULL, OPT_WHOLE_SLOT},
        {"verbose",     no_argument,       NULL, 'v'},
        {"debug",       required_argument, NULL, 'd'},
        {"help",        no_argument,       NULL, 'h'},
        {NULL,          0,                 NULL, 0},
    };

    while ((opt = getopt_long(argc, argv, "s:ABo:c:vd:h",
                              long_options, NULL)) != -1) {
        switch (opt) {
        case 's':
            if (!uki_slot_parse_sel(optarg, &sel)) {
                fprintf(stderr, "Invalid slot: %s (must be 'a', 'b' or 'auto')\n",
                        optarg);
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
            break;
        case 'o':
            output_path = optarg;
            break;
        case 'c':
            cert_path = optarg;
            break;
        case OPT_WHOLE_SLOT:
            whole_slot = true;
            break;
        case 'v':
            g_log_level = LOG_INFO;
            break;
        case 'd':
            g_log_level = atoi(optarg);
            if (g_log_level < LOG_QUIET || g_log_level > LOG_DEBUG) {
                fprintf(stderr, "Invalid debug level: %s (must be 0-3)\n", optarg);
                return EXIT_FAILURE;
            }
            break;
        case 'h':
            usage(argv[0]);
            return EXIT_SUCCESS;
        default:
            usage(argv[0]);
            return EXIT_FAILURE;
        }
    }

    if (optind + 1 != argc) {
        fprintf(stderr, "Error: expected a single ukiboot image\n");
        usage(argv[0]);
        return EXIT_FAILURE;
    }

    const char *image_path = argv[optind];
    bool to_stdout = (output_path && strcmp(output_path, "-") == 0);

    /* Let ukiboot's helpers log along with our info messages, unless the
     * UKI goes to stdout: ukiboot's verbose() writes there too, and would
     * corrupt the extracted image */
    opt_verbose = (g_log_level >= LOG_INFO) && !to_stdout;

    int image_fd = open(image_path, O_RDONLY);
    if (image_fd < 0) {
        ERROR_LOG("Failed to open '%s': %s", image_path, strerror(errno));
        return EXIT_FAILURE;
    }

    /* Locate the slot, the same way ukifs does */
    uki_slot_info_t slot_info;
    if (!uki_slot_locate(image_fd, sel, bootctl_path, &slot_info)) {
        ERROR_LOG("Failed to locate a ukiboot slot in '%s'", image_path);
        close(image_fd);
        return EXIT_FAILURE;
    }

    /* Size of the UKI itself, the rest of the slot being padding */
    uint64_t length = slot_info.size;

    if (whole_slot) {
        /* An image file may be truncated: do not read past its end */
        off_t media_bytes = uki_media_size(image_fd);

        if (media_bytes > 0 &&
            (off_t)(slot_info.offset + length) > media_bytes) {
            length = (uint64_t)(media_bytes - (off_t)slot_info.offset);
            INFO_LOG("Slot %c extends past the end of '%s', extracting %llu bytes",
                     uki_slot_letter(slot_info.slot), image_path,
                     (unsigned long long)length);
        }
    } else {
        uki_size_info_t size_info;

        if (!uki_compute_size(image_fd, (off_t)slot_info.offset, &size_info)) {
            ERROR_LOG("No valid UKI in slot %c of '%s'",
                      uki_slot_letter(slot_info.slot), image_path);
            close(image_fd);
            return EXIT_FAILURE;
        }

        if ((uint64_t)size_info.expected_size > slot_info.size) {
            ERROR_LOG("Truncated UKI: %u bytes needed, %llu bytes in slot %c",
                      size_info.expected_size,
                      (unsigned long long)slot_info.size,
                      uki_slot_letter(slot_info.slot));
            close(image_fd);
            return EXIT_FAILURE;
        }

        length = size_info.expected_size;

        INFO_LOG("UKI size: %u bytes (%llu bytes of padding ignored)",
                 size_info.expected_size,
                 (unsigned long long)(slot_info.size - size_info.expected_size));

        if (size_info.cert_table_size > 0) {
            INFO_LOG("Certificate table: offset %u, size %u bytes",
                     size_info.cert_table_offset, size_info.cert_table_size);
        }
    }

    /* Verify the signature where the UKI sits in the image: the padding
     * of the slot is not part of the Authenticode digest */
    if (cert_path) {
        INFO_LOG("Verifying the UKI signature with certificate: %s", cert_path);

        if (!pe_verify_signature(image_fd, (off_t)slot_info.offset, cert_path)) {
            ERROR_LOG("UKI signature verification failed, not extracting");
            close(image_fd);
            return EXIT_FAILURE;
        }

        INFO_LOG("UKI signature verified successfully");
    }

    /* Extract */
    const char *default_output = (slot_info.slot == 0)
                                     ? UKI_SLOT_A_PART_NAME ".efi"
                                     : UKI_SLOT_B_PART_NAME ".efi";
    int out_fd;

    if (to_stdout) {
        out_fd = STDOUT_FILENO;
    } else {
        if (!output_path) {
            output_path = default_output;
        }

        out_fd = open(output_path, O_WRONLY | O_CREAT | O_TRUNC, 0644);
        if (out_fd < 0) {
            ERROR_LOG("Failed to create '%s': %s", output_path, strerror(errno));
            close(image_fd);
            return EXIT_FAILURE;
        }
    }

    bool ok = copy_range(image_fd, (off_t)slot_info.offset, length, out_fd);

    if (!to_stdout) {
        close(out_fd);
    }
    close(image_fd);

    if (!ok) {
        return EXIT_FAILURE;
    }

    INFO_LOG("Extracted %llu bytes from slot %c of '%s' to '%s'",
             (unsigned long long)length, uki_slot_letter(slot_info.slot),
             image_path, to_stdout ? "<stdout>" : output_path);

    if (!to_stdout && g_log_level < LOG_INFO) {
        /* Say where the UKI landed, even without --verbose */
        printf("%s\n", output_path);
    }

    return EXIT_SUCCESS;
}
