/*
 * ukifs.h - QNX UKI Filesystem Resource Manager
 *
 * Read-only filesystem driver that exposes UKI (Unified Kernel Image)
 * PE sections as a virtual filesystem using QNX resource manager API.
 */

#ifndef UKIFS_CORE_H
#define UKIFS_CORE_H

#include <stdint.h>
#include <stdbool.h>

#include <sys/iofunc.h>
#include <sys/dispatch.h>
#include <sys/resmgr.h>

#include "ukifs_utils.h"
#include "ukifs_dir.h"


/* Hash table size for path lookups */
#define HASH_TABLE_SIZE 1024

/* Initial capacity for directory children arrays */
#define DIR_INITIAL_CAPACITY 16

/* Default UKI files permissions */
#define UKI_FILES_PERM 0444

/*
 * UKI entry ctructure
 * Represents a single PE section exposed as a file
 */
typedef struct uki_entry {
    char *name;                    /* Sanitized name (e.g., "linux", "initrd") */
    uint32_t file_offset;          /* PointerToRawData from PE */
    uint32_t size;                 /* VirtualSize from PE */
    mode_t mode;                   /* Always S_IFREG | 0444 (read-only) */
    time_t mtime;                  /* Modification time */
    struct uki_entry *next;        /* For hash table collision chaining */
    iofunc_attr_t attr;            /* QNX iofunc attributes */
} uki_entry_t;

/*
 * Global UKI context
 * Maintains all state for the UKI filesystem instance
 */
typedef struct uki_context {
    void *mmap_base;               /* Base address of mmap'd UKI file */
    size_t mmap_size;              /* Size of mmap'd region */
    int uki_fd;                    /* File descriptor for UKI file */
    mode_t perm;                   /* File permissions */

    dir_node_t *root;              /* Root of directory tree */
    uki_entry_t **hash_table;      /* Hash table for path lookups */
    size_t hash_table_size;        /* Size of hash table */

    iofunc_mount_t mount;          /* QNX mount point structure */
    resmgr_attr_t resmgr_attr;     /* Resource manager attributes */
    dispatch_t *dpp;               /* Dispatch handle */
    thread_pool_attr_t pool_attr;  /* Thread pool attributes */
    thread_pool_t *tpool;          /* Thread pool for handling requests */

    int resmgr_id;                 /* Attached resource manager ID */
    char *mount_point;             /* Mount point path */
    char *uki_path;                /* UKI file path */
    uint32_t num_sections;         /* Number of PE sections */
    char *cert_path;               /* Optional certificate for signature verification */
} uki_context_t;

/*
 * Extended OCB (Open Control Block)
 * Per-file-descriptor state for open files/directories
 */
typedef struct uki_ocb {
    iofunc_ocb_t ocb;              /* Base QNX OCB (MUST be first) */
    uki_entry_t *entry;            /* Associated UKI entry */

    /* For directory reading */
    size_t dir_index;              /* Current position in directory listing */
    dir_node_t *dir_node;          /* Directory being read (if S_ISDIR) */
} uki_ocb_t;

/*
 * Get the UKI context from the mount structure in the OCB
 */
static inline uki_context_t *uki_context(iofunc_mount_t *mount) {
    return container_of(mount, uki_context_t, mount);
}

#endif /* UKIFS_CORE_H */
