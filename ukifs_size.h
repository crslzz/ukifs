/*
 * UKI Size Computation
 *
 * Compute the actual UKI size from PE/COFF headers,
 * excluding any extra padding appended after the UKI.
 */

#ifndef UKIFS_SIZE_H
#define UKIFS_SIZE_H

#include <stdint.h>
#include <stdbool.h>
#include <sys/types.h>

/*
 * UKI size information
 */
typedef struct {
    uint32_t headers_end;         /* End of PE headers (DOS + NT + sections table) */
    uint32_t last_section_end;    /* End of last section */
    uint32_t cert_table_offset;   /* Certificate table offset (0 if none) */
    uint32_t cert_table_size;     /* Certificate table size (0 if none) */
    uint32_t cert_table_end;      /* End of certificate table (0 if none) */
    uint32_t expected_size;       /* Computed UKI size (max of above) */
    uint16_t num_sections;        /* Number of PE sections */
} uki_size_info_t;

/*
 * Compute UKI size from PE headers
 *
 * Analyzes the PE/COFF structure and calculates the minimum file size
 * needed based on headers, sections, and certificate table (if present).
 *
 * Parameters:
 *   fd - Open file descriptor to UKI file
 *   info - Output structure to receive size information
 *
 * Returns:
 *   true on success, false on error (invalid PE file, read error, etc.)
 */
bool uki_compute_size(int fd, uki_size_info_t *info);

#endif /* UKIFS_SIZE_H */
