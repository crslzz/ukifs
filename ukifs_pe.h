/*
 * uki_pe.h - QNX UKI Filesystem Resource Manager
 *
 * Read-only filesystem driver that exposes UKI (Unified Kernel Image)
 * PE sections as a virtual filesystem using QNX resource manager API.
 */

#ifndef UKI_PE_H
#define UKI_PE_H

#include <stdint.h>

#include "ukifs_utils.h"

/* PE format constants */
#define PE_SECTION_NAME_SIZE 8
#define IMAGE_DOS_SIGNATURE 0x5A4D
#define IMAGE_NT_SIGNATURE 0x00004550

/* PE/COFF Header Structures (all little-endian) */
typedef struct {
    uint16_t e_magic;              /* DOS signature (0x5A4D) */
    uint16_t e_cblp;
    uint16_t e_cp;
    uint16_t e_crlc;
    uint16_t e_cparhdr;
    uint16_t e_minalloc;
    uint16_t e_maxalloc;
    uint16_t e_ss;
    uint16_t e_sp;
    uint16_t e_csum;
    uint16_t e_ip;
    uint16_t e_cs;
    uint16_t e_lfarlc;
    uint16_t e_ovno;
    uint16_t e_res[4];
    uint16_t e_oemid;
    uint16_t e_oeminfo;
    uint16_t e_res2[10];
    int32_t e_lfanew;              /* Offset to NT headers */
} PACKED ImageDosHeader;

typedef struct {
    uint16_t Machine;
    uint16_t NumberOfSections;
    uint32_t TimeDateStamp;
    uint32_t PointerToSymbolTable;
    uint32_t NumberOfSymbols;
    uint16_t SizeOfOptionalHeader;
    uint16_t Characteristics;
} PACKED ImageFileHeader;

typedef struct {
    uint32_t Signature;            /* PE signature (0x00004550) */
    ImageFileHeader FileHeader;
} PACKED ImageNtHeaders;

typedef struct {
    uint8_t Name[8];               /* Section name (NOT null-terminated) */
    union {
        uint32_t PhysicalAddress;
        uint32_t VirtualSize;      /* Use this for actual size */
    } Misc;
    uint32_t VirtualAddress;
    uint32_t SizeOfRawData;
    uint32_t PointerToRawData;     /* File offset to section data */
    uint32_t PointerToRelocations;
    uint32_t PointerToLinenumbers;
    uint16_t NumberOfRelocations;
    uint16_t NumberOfLinenumbers;
    uint32_t Characteristics;
} PACKED ImageSectionHeader;

#endif /* UKI_PE_H */
