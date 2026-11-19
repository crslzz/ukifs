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
#include "ukifs_slot.h"


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
    void *mmap_base;               /* Address of the UKI within the mapping,
                                      NULL when the UKI is read with pread()  */
    void *map_addr;                /* Base address of the mapping (page aligned) */
    size_t map_len;                /* Length of the mapping */
    off_t uki_offset;              /* Offset of the UKI in the opened file */
    size_t uki_size;               /* Bytes available for the UKI */
    size_t uki_extent;             /* Bytes actually covered by its sections */
    int uki_fd;                    /* File descriptor for UKI file/disk image */
    bool no_mmap;                  /* Never mmap, always read with pread() */
    mode_t perm;                   /* File permissions */

    struct stat src_stat;          /* Stat of the source, timestamps fixed up
                                      for sources that have none (devices) */

    uki_slot_sel_t slot_sel;       /* Requested ukiboot slot (or NONE) */
    int slot;                      /* Selected ukiboot slot (-1 if none) */
    char *bootctl_path;            /* Optional standalone ukibootctl partition */

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

/* Ask the main thread to unmount the filesystem and exit */
void uki_request_shutdown(void);

/* True while a client still has a file or the root directory open */
bool uki_fs_busy(const struct uki_context *ctx);

/*
 * Get the UKI context from the mount structure in the OCB
 */
static inline uki_context_t *uki_context(iofunc_mount_t *mount) {
    return container_of(mount, uki_context_t, mount);
}

#endif /* UKIFS_CORE_H */
