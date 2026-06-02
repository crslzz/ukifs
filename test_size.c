/*
 * Compute expected UKI size from PE headers
 *
 * Calculates the minimum file size needed for a UKI based on its
 * PE/COFF structure, excluding any extra padding appended after
 * the last section.
 */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <fcntl.h>
#include <unistd.h>
#include <stdint.h>
#include <stdbool.h>
#include <errno.h>
#include <sys/stat.h>
#include <endian.h>

#include "ukifs_size.h"
#include "ukifs_pe.h"

#include "utils.h"

/* Section comparison for qsort (by file offset) */
static int section_compare(const void *a, const void *b) {
    const ImageSectionHeader *sa = (const ImageSectionHeader *)a;
    const ImageSectionHeader *sb = (const ImageSectionHeader *)b;
    uint32_t offset_a = le32toh(sa->PointerToRawData);
    uint32_t offset_b = le32toh(sb->PointerToRawData);

    if (offset_a < offset_b) return -1;
    if (offset_a > offset_b) return 1;
    return 0;
}

/* Sanitize section name for display */
static void print_section_name(const uint8_t *name) {
    char buf[PE_SECTION_NAME_SIZE + 1];
    size_t len = 0;

    for (size_t i = 0; i < PE_SECTION_NAME_SIZE && name[i] != '\0'; i++) {
        buf[len++] = name[i];
    }
    buf[len] = '\0';

    const char *display_name = buf;
    if (len > 0 && buf[0] == '.') {
        display_name = buf + 1;
    }

    printf("%s", display_name);
}

int main(int argc, char *argv[]) {
    if (argc != 2) {
        fprintf(stderr, "Usage: %s <uki-file>\n", argv[0]);
        fprintf(stderr, "\n");
        fprintf(stderr, "Compute expected UKI size from PE headers.\n");
        fprintf(stderr, "\n");
        fprintf(stderr, "This program analyzes the PE/COFF structure and calculates\n");
        fprintf(stderr, "the minimum file size needed based on section layout,\n");
        fprintf(stderr, "excluding any extra padding appended after the UKI.\n");
        fprintf(stderr, "\n");
        fprintf(stderr, "Examples:\n");
        fprintf(stderr, "  %s /boot/archlinux.efi\n", argv[0]);
        fprintf(stderr, "  %s ukiboot_a.img\n", argv[0]);
        return EXIT_FAILURE;
    }

    const char *uki_path = argv[1];

    /* Open UKI file */
    int fd = open(uki_path, O_RDONLY);
    if (fd < 0) {
        fprintf(stderr, "ERROR: Failed to open '%s': %s\n",
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
    off_t actual_size = st.st_size;

    printf("========================================\n");
    printf("UKI Size Analysis\n");
    printf("========================================\n");
    printf("File: %s\n", uki_path);
    printf("Actual file size: %ld bytes (%.2f MB)\n",
           (long)actual_size, actual_size / 1024.0 / 1024.0);
    printf("========================================\n\n");

    /* Compute UKI size using common code */
    uki_size_info_t size_info;
    if (!uki_compute_size(fd, &size_info)) {
        fprintf(stderr, "ERROR: Failed to compute UKI size (invalid PE file)\n");
        close(fd);
        return EXIT_FAILURE;
    }

    /* Read section headers for display */
    ImageDosHeader dos_header;
    if (!full_pread(fd, &dos_header, sizeof(dos_header), 0)) {
        fprintf(stderr, "ERROR: Failed to read DOS header\n");
        close(fd);
        return EXIT_FAILURE;
    }

    uint32_t nt_offset = (uint32_t)le32toh(dos_header.e_lfanew);

    ImageNtHeaders nt_header;
    if (!full_pread(fd, &nt_header, sizeof(nt_header), nt_offset)) {
        fprintf(stderr, "ERROR: Failed to read NT header\n");
        close(fd);
        return EXIT_FAILURE;
    }

    uint16_t opt_header_size = le16toh(nt_header.FileHeader.SizeOfOptionalHeader);
    uint16_t num_sections = le16toh(nt_header.FileHeader.NumberOfSections);
    uint32_t sections_offset = nt_offset + sizeof(ImageNtHeaders) + opt_header_size;

    printf("PE Structure:\n");
    printf("  DOS header:      0 - %zu bytes\n", sizeof(ImageDosHeader));
    printf("  NT headers at:   %u bytes\n", nt_offset);
    printf("  Optional header: %u bytes\n", opt_header_size);
    printf("  Sections table:  %u bytes (offset %u)\n",
           num_sections * (uint32_t)sizeof(ImageSectionHeader), sections_offset);
    printf("  Number of sections: %u\n", num_sections);

    if (size_info.cert_table_size > 0) {
        printf("  Certificate table: offset %u, size %u bytes\n",
               size_info.cert_table_offset, size_info.cert_table_size);
    }

    /* Read and display section layout */
    ImageSectionHeader *sections = calloc(num_sections, sizeof(ImageSectionHeader));
    if (!sections) {
        fprintf(stderr, "ERROR: Failed to allocate section headers\n");
        close(fd);
        return EXIT_FAILURE;
    }

    if (!full_pread(fd, sections, num_sections * sizeof(ImageSectionHeader), sections_offset)) {
        fprintf(stderr, "ERROR: Failed to read section headers\n");
        free(sections);
        close(fd);
        return EXIT_FAILURE;
    }

    qsort(sections, num_sections, sizeof(ImageSectionHeader), section_compare);

    printf("\nSection Layout:\n");
    printf("%-15s %10s %10s %12s\n", "Name", "Offset", "Size", "End");
    printf("%-15s %10s %10s %12s\n", "----", "------", "----", "---");

    int last_section_idx = -1;
    for (uint16_t i = 0; i < num_sections; i++) {
        uint32_t offset = le32toh(sections[i].PointerToRawData);
        uint32_t size = le32toh(sections[i].SizeOfRawData);
        uint32_t end = offset + size;

        printf("  ");
        print_section_name(sections[i].Name);

        char name_buf[PE_SECTION_NAME_SIZE + 1];
        size_t name_len = 0;
        for (size_t j = 0; j < PE_SECTION_NAME_SIZE && sections[i].Name[j] != '\0'; j++) {
            name_buf[name_len++] = sections[i].Name[j];
        }
        if (name_len > 0 && name_buf[0] == '.') name_len--;
        for (size_t j = name_len; j < 13; j++) printf(" ");

        printf("%10u %10u %12u", offset, size, end);

        if (end == size_info.last_section_end) {
            last_section_idx = i;
        }

        printf("\n");
    }

    printf("\n");
    printf("Size Computation:\n");
    printf("  Headers end at:        %u bytes\n", size_info.headers_end);
    printf("  Last section ends at:  %u bytes", size_info.last_section_end);
    if (last_section_idx >= 0) {
        printf(" (");
        print_section_name(sections[last_section_idx].Name);
        printf(")");
    }
    printf("\n");

    if (size_info.cert_table_end > 0) {
        printf("  Cert table ends at:    %u bytes\n", size_info.cert_table_end);
    }

    free(sections);

    printf("\n");
    printf("========================================\n");
    printf("Results:\n");
    printf("========================================\n");
    printf("Expected UKI size: %u bytes (%.2f MB)\n",
           size_info.expected_size, size_info.expected_size / 1024.0 / 1024.0);
    printf("Actual file size:  %ld bytes (%.2f MB)\n",
           (long)actual_size, actual_size / 1024.0 / 1024.0);

    if ((off_t)size_info.expected_size < actual_size) {
        off_t extra = actual_size - size_info.expected_size;
        printf("\n");
        printf("Extra padding:     %ld bytes (%.2f MB)\n",
               (long)extra, extra / 1024.0 / 1024.0);
        printf("Padding ratio:     %.1f%% of file size\n",
               (extra * 100.0) / actual_size);
        printf("\n");
        printf("✓ The file contains %ld bytes of extra data after the UKI\n", (long)extra);
    } else if ((off_t)size_info.expected_size == actual_size) {
        printf("\n");
        printf("✓ File size matches expected UKI size (no extra padding)\n");
    } else {
        printf("\n");
        printf("✗ WARNING: File is smaller than expected!\n");
        printf("  Missing: %ld bytes\n", (long)(size_info.expected_size - actual_size));
        printf("  The file may be truncated or corrupted.\n");
    }
    printf("========================================\n");

    close(fd);
    return EXIT_SUCCESS;
}
