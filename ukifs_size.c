/*
 * UKI size computation
 */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

#include "ukifs_pe.h"
#include "ukifs_size.h"

#include "utils.h"


bool uki_compute_size(int fd, uki_size_info_t *info) {
    if (!info) {
        return false;
    }

    /* Initialize output structure */
    memset(info, 0, sizeof(uki_size_info_t));

    /* Read DOS header */
    ImageDosHeader dos_header;
    if (!full_pread(fd, &dos_header, sizeof(dos_header), 0)) {
        return false;
    }

    /* Validate DOS signature */
    if (le16toh(dos_header.e_magic) != IMAGE_DOS_SIGNATURE) {
        return false;
    }

    uint32_t nt_offset = (uint32_t)le32toh(dos_header.e_lfanew);

    /* Read NT header */
    ImageNtHeaders nt_header;
    if (!full_pread(fd, &nt_header, sizeof(nt_header), nt_offset)) {
        return false;
    }

    /* Validate PE signature */
    if (le32toh(nt_header.Signature) != IMAGE_NT_SIGNATURE) {
        return false;
    }

    /* Extract section information */
    uint16_t opt_header_size = le16toh(nt_header.FileHeader.SizeOfOptionalHeader);
    uint16_t num_sections = le16toh(nt_header.FileHeader.NumberOfSections);
    uint32_t opt_header_offset = nt_offset + sizeof(ImageNtHeaders);
    uint32_t sections_offset = opt_header_offset + opt_header_size;

    info->num_sections = num_sections;
    info->headers_end = sections_offset + (num_sections * sizeof(ImageSectionHeader));

    if (num_sections == 0) {
        return false;
    }

    /* Read certificate table info from Optional Header Data Directory */
    if (opt_header_size >= 68) {  /* Large enough to contain data directories */
        /* Read Optional Header magic to determine PE32 vs PE32+ */
        uint16_t opt_magic;
        if (full_pread(fd, &opt_magic, sizeof(opt_magic), opt_header_offset)) {
            opt_magic = le16toh(opt_magic);
            bool is_pe32plus = (opt_magic == 0x20b);  /* PE32+ magic */

            /* Security Data Directory offset: PE32+ = 144, PE32 = 128 */
            uint32_t security_dir_offset = opt_header_offset + (is_pe32plus ? 144 : 128);

            /* Read certificate table offset and size */
            uint32_t sec_dir[2];
            if (full_pread(fd, sec_dir, sizeof(sec_dir), security_dir_offset)) {
                info->cert_table_offset = le32toh(sec_dir[0]);
                info->cert_table_size = le32toh(sec_dir[1]);

                if (info->cert_table_size > 0) {
                    info->cert_table_end = info->cert_table_offset + info->cert_table_size;
                }
            }
        }
    }

    /* Read all section headers */
    ImageSectionHeader *sections = calloc(num_sections, sizeof(ImageSectionHeader));
    if (!sections) {
        return false;
    }

    if (!full_pread(fd, sections, num_sections * sizeof(ImageSectionHeader), sections_offset)) {
        free(sections);
        return false;
    }

    /* Find the end of the last section */
    info->last_section_end = 0;
    for (uint16_t i = 0; i < num_sections; i++) {
        uint32_t offset = le32toh(sections[i].PointerToRawData);
        uint32_t size = le32toh(sections[i].SizeOfRawData);
        uint32_t end = offset + size;

        if (end > info->last_section_end) {
            info->last_section_end = end;
        }
    }

    free(sections);

    /* Expected UKI size is the maximum of headers_end, last_section_end, and cert_table_end */
    info->expected_size = info->headers_end;
    if (info->last_section_end > info->expected_size) {
        info->expected_size = info->last_section_end;
    }
    if (info->cert_table_end > info->expected_size) {
        info->expected_size = info->cert_table_end;
    }

    return true;
}
