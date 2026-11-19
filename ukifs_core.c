/*
 * QNX UKI Filesystem entry point
 */

#include <getopt.h>
#include <stdlib.h>
#include <stdio.h>
#include <string.h>
#include <time.h>
#include <sys/mman.h>
#include <sys/dispatch.h>

#include "ukifs_core.h"
#include "ukifs_size.h"
#include "ukifs_parse.h"
#include "ukifs_io.h"
#include "ukifs_slot.h"

#include "pe_parser_posix.h"
#include "utils.h"

/* Global context for signal handler */
static uki_context_t *g_ctx = NULL;

int g_log_level = LOG_ERROR;  /* Default log level */


/* Cleanup resources */
static void cleanup(uki_context_t *ctx) {
    DEBUG_ENTER();

    if (ctx) {
        DEBUG_LOG("Shutting down UKI filesystem...");

        /* Step 1: Detach resource manager (if not already done by signal handler) */
        if (ctx->resmgr_id != -1 && ctx->dpp) {
            DEBUG_LOG("Detaching resource manager");
            resmgr_detach(ctx->dpp, ctx->resmgr_id, _RESMGR_DETACH_ALL);
            ctx->resmgr_id = -1;
        }

        /* Give threads a moment to finish current requests */
        usleep(100000);  /* 100ms */

        /* Step 2: Destroy thread pool (may block briefly) */
        if (ctx->tpool) {
            DEBUG_LOG("Destroying thread pool");
            /* Set a reasonable timeout - thread_pool_destroy will wait for threads */
            thread_pool_destroy(ctx->tpool);
            ctx->tpool = NULL;
        }

        /* Step 3: Destroy dispatch */
        if (ctx->dpp) {
            DEBUG_LOG("Destroying dispatch");
            dispatch_destroy(ctx->dpp);
            ctx->dpp = NULL;
        }

        /* Step 5: Free directory tree */
        if (ctx->root) {
            DEBUG_LOG("Freeing directory tree");
            free_dir_node(ctx->root);
            ctx->root = NULL;
        }

        /* Step 5: Free hash table */
        if (ctx->hash_table) {
            DEBUG_LOG("Freeing hash table");
            free(ctx->hash_table);
            ctx->hash_table = NULL;
        }

        /* Step 6: Unmap file */
        if (ctx->map_addr && ctx->map_addr != MAP_FAILED) {
            DEBUG_LOG("Unmapping UKI file");
            munmap(ctx->map_addr, ctx->map_len);
            ctx->map_addr = NULL;
            ctx->mmap_base = NULL;
        }

        /* Step 7: Close file */
        if (ctx->uki_fd >= 0) {
            DEBUG_LOG("Closing UKI file");
            close(ctx->uki_fd);
            ctx->uki_fd = -1;
        }

        DEBUG_LOG("Cleanup complete");
    }

    DEBUG_EXIT(0);
}

/* Alarm handler for forced exit */
static void alarm_handler(int signo UNUSED) {
    ERROR_LOG("Forced shutdown (thread pool not responding)");
    _exit(1);
}

/* Signal handler for cleanup */
static void signal_handler(int signo UNUSED) {
    DEBUG_LOG("Received signal %d, shutting down...", signo);

    /* Set alarm for forced exit in case cleanup hangs */
    alarm(3);  /* Force exit after 3 seconds */

    /* Do cleanup directly */
    if (g_ctx) {
        cleanup(g_ctx);
        free(g_ctx);
        g_ctx = NULL;
    }

    /* Cancel alarm and exit cleanly */
    alarm(0);
    exit(0);
}

/* Long-only option values */
#define OPT_BOOTCTL 256
#define OPT_NO_MMAP 257

static void usage(const char *argv0) {
    fprintf(stderr, "Usage: %s [options] <uki-file|ukiboot-image> <mount-point>\n", argv0);
    fprintf(stderr, "\n");
    fprintf(stderr, "Mount a UKI (Unified Kernel Image) as a read-only filesystem.\n");
    fprintf(stderr, "The UKI is either a plain UKI file, or the content of one of the\n");
    fprintf(stderr, "ukiboot_a / ukiboot_b slots of a ukiboot disk image.\n");
    fprintf(stderr, "\n");
    fprintf(stderr, "Options:\n");
    fprintf(stderr, "  -v, --verbose       Verbose mode (info messages)\n");
    fprintf(stderr, "  -p, --perm <perm>   File permission (default: %04o)\n", UKI_FILES_PERM);
    fprintf(stderr, "  -d, --debug <level> Debug level: 0=quiet, 1=errors, 2=info, 3=debug (default: %d)\n", g_log_level);
    fprintf(stderr, "  -c, --cert <cert.pem>\n");
    fprintf(stderr, "                      X.509 PEM certificate for signature verification\n");
    fprintf(stderr, "  -s, --slot <a|b|auto>\n");
    fprintf(stderr, "                      Read the UKI from a slot of a ukiboot disk image:\n");
    fprintf(stderr, "                      'a' (%s), 'b' (%s), or 'auto' to\n",
	    UKI_SLOT_A_PART_NAME, UKI_SLOT_B_PART_NAME);
    fprintf(stderr, "                      select the active slot from the %s partition\n",
	    UKI_BOOTCTL_PART_NAME);
    fprintf(stderr, "  -A, --slot-a        Same as --slot a\n");
    fprintf(stderr, "  -B, --slot-b        Same as --slot b\n");
    fprintf(stderr, "      --bootctl <path>\n");
    fprintf(stderr, "                      Path to a standalone %s partition, used\n",
	    UKI_BOOTCTL_PART_NAME);
    fprintf(stderr, "                      instead of the one of the disk image (implies --slot auto)\n");
    fprintf(stderr, "      --no-mmap       Never map the UKI, always read it with pread()\n");
    fprintf(stderr, "                      (automatic when the source cannot be mapped)\n");
    fprintf(stderr, "  -h, --help          Show this help\n");
    fprintf(stderr, "\n");
    fprintf(stderr, "Examples:\n");
    fprintf(stderr, "  %s /boot/ukiboot.efi /mnt/uki\n", argv0);
    fprintf(stderr, "  %s -v /boot/ukiboot.efi /mnt/uki\n", argv0);
    fprintf(stderr, "  %s -c /etc/uki-cert.pem /boot/ukiboot.efi /mnt/uki\n", argv0);
    fprintf(stderr, "  %s -d 3 /boot/ukiboot.efi /mnt/uki  # Full debug traces\n", argv0);
    fprintf(stderr, "  %s --slot auto /dev/hd0 /mnt/uki    # Active slot of a ukiboot disk\n", argv0);
    fprintf(stderr, "  %s --slot-b /tmp/ukiboot.img /mnt/uki\n", argv0);
    fprintf(stderr, "  %s --slot auto /dev/hd1t179 /mnt/uki # ukiboot image in a partition\n", argv0);
}

/* Parse command line arguments */
static int parse_arguments(int argc, char *argv[], uki_context_t *ctx) {
    int opt;
    char *endptr;
    mode_t perm = UKI_FILES_PERM; /* default permissions */
    int log_level = g_log_level; /* default log level */

    static const struct option long_options[] = {
	{"verbose", no_argument,       NULL, 'v'},
	{"perm",    required_argument, NULL, 'p'},
	{"debug",   required_argument, NULL, 'd'},
	{"cert",    required_argument, NULL, 'c'},
	{"slot",    required_argument, NULL, 's'},
	{"slot-a",  no_argument,       NULL, 'A'},
	{"slot-b",  no_argument,       NULL, 'B'},
	{"bootctl", required_argument, NULL, OPT_BOOTCTL},
	{"no-mmap", no_argument,       NULL, OPT_NO_MMAP},
	{"help",    no_argument,       NULL, 'h'},
	{NULL,      0,                 NULL, 0},
    };

    /* Initialize optional fields */
    ctx->cert_path = NULL;
    ctx->bootctl_path = NULL;
    ctx->no_mmap = false;
    ctx->slot_sel = UKI_SLOT_SEL_NONE;
    ctx->slot = -1;

    /* Parse options */
    while ((opt = getopt_long(argc, argv, "vp:d:c:s:ABh",
			      long_options, NULL)) != -1) {
        switch (opt) {
	case 'v':
	    log_level = LOG_INFO;
	    break;
	case 'p':
	    perm = (int) strtol(optarg, &endptr, 8);
	    if (optarg == endptr) {
		fprintf(stderr, "Invalid file permissions: %s (must be 3-digit octal)\n", optarg);
		return -1;
	    }
	    perm &= 0555; /* r-xr-x-r-x permissions only */
	    break;
	case 'd':
	    log_level = atoi(optarg);
	    if (log_level < LOG_QUIET || log_level > LOG_DEBUG) {
		fprintf(stderr, "Invalid debug level: %s (must be 0-3)\n", optarg);
		return -1;
	    }
	    break;
	case 'c':
	    ctx->cert_path = optarg;
	    break;
	case 's':
	    if (!uki_slot_parse_sel(optarg, &ctx->slot_sel)) {
		fprintf(stderr, "Invalid slot: %s (must be 'a', 'b' or 'auto')\n", optarg);
		return -1;
	    }
	    break;
	case 'A':
	    ctx->slot_sel = UKI_SLOT_SEL_A;
	    break;
	case 'B':
	    ctx->slot_sel = UKI_SLOT_SEL_B;
	    break;
	case OPT_BOOTCTL:
	    ctx->bootctl_path = optarg;
	    break;
	case OPT_NO_MMAP:
	    ctx->no_mmap = true;
	    break;
	case 'h':
	    usage(argv[0]);
	    return -1;
	default:
	    usage(argv[0]);
	    return -1;
        }
    }

    /* A standalone ukibootctl partition only makes sense for an
     * automatic slot selection */
    if (ctx->bootctl_path && ctx->slot_sel == UKI_SLOT_SEL_NONE) {
	ctx->slot_sel = UKI_SLOT_SEL_AUTO;
    }

    /* Check for required positional arguments */
    if (optind + 2 != argc) {
        fprintf(stderr, "Error: Missing required arguments\n");
        fprintf(stderr, "Usage: %s [options] <uki-file|ukiboot-image> <mount-point>\n", argv[0]);
        fprintf(stderr, "Try '%s -h' for more information.\n", argv[0]);
        return -1;
    }

    g_log_level = log_level;

    /* Let ukiboot's helpers log along with our info messages */
    opt_verbose = (log_level >= LOG_INFO);

    ctx->uki_path = argv[optind];
    ctx->mount_point = argv[optind + 1];
    ctx->perm = perm;

    DEBUG_STR("UKI file", ctx->uki_path);
    DEBUG_STR("Mount point", ctx->mount_point);

    return 0;
}

/*
 * Locate the UKI within the opened file or device.
 *
 * Without a slot selection the whole media is the UKI. Otherwise it is a
 * ukiboot disk image, and the UKI is the content of the selected
 * ukiboot_a / ukiboot_b partition.
 *
 * Returns 0 on success, -1 on error.
 */
static int locate_uki(uki_context_t *ctx, off_t media_bytes,
		      bool size_is_exact) {
    if (ctx->slot_sel == UKI_SLOT_SEL_NONE) {
	if (media_bytes <= 0) {
	    ERROR_LOG("Cannot determine the size of '%s'", ctx->uki_path);
	    return -1;
	}
	ctx->uki_offset = 0;
	ctx->uki_size = (size_t)media_bytes;
	ctx->slot = -1;
	return 0;
    }

    uki_slot_info_t slot_info;

    if (!uki_slot_locate(ctx->uki_fd, ctx->slot_sel,
			 ctx->bootctl_path, &slot_info)) {
	ERROR_LOG("Failed to locate a ukiboot slot in '%s'", ctx->uki_path);
	return -1;
    }

    ctx->slot = slot_info.slot;
    ctx->uki_offset = (off_t)slot_info.offset;
    ctx->uki_size = (size_t)slot_info.size;

    /* The slot may lie past the end of the media, for instance when an
     * image file is truncated. Only an exact size is trusted here: the
     * size reported by a device driver is not worth failing on, a read
     * that really is out of range fails on its own. */
    if (media_bytes > 0 &&
	(off_t)(slot_info.offset + slot_info.size) > media_bytes) {
	if (!size_is_exact) {
	    INFO_LOG("Slot %c ends past the reported size of '%s' (%lld bytes), "
		     "trusting the partition table",
		     uki_slot_letter(ctx->slot), ctx->uki_path,
		     (long long)media_bytes);
	} else if ((off_t)slot_info.offset >= media_bytes) {
	    ERROR_LOG("Slot %c starts at offset %llu, past the end of '%s'",
		      uki_slot_letter(slot_info.slot),
		      (unsigned long long)slot_info.offset, ctx->uki_path);
	    return -1;
	} else {
	    ctx->uki_size = (size_t)(media_bytes - (off_t)slot_info.offset);
	    INFO_LOG("Slot %c extends past the end of '%s', clamping to %zu bytes",
		     uki_slot_letter(ctx->slot), ctx->uki_path, ctx->uki_size);
	}
    }

    INFO_LOG("Using ukiboot slot %c at offset %lld (%zu bytes)",
	     uki_slot_letter(ctx->slot), (long long)ctx->uki_offset,
	     ctx->uki_size);

    return 0;
}

/*
 * Map the UKI region of the opened file.
 *
 * mmap() offsets must be page aligned, so the mapping starts at the page
 * holding the beginning of the UKI, and mmap_base points to the UKI itself.
 *
 * Mapping is an optimization: it lets reads be served straight out of the
 * mapping, without copying. Block devices usually cannot be mapped (QNX
 * block drivers do not implement mmap), in which case the UKI is read
 * with pread() instead, and mmap_base stays NULL.
 */
static void map_uki(uki_context_t *ctx) {
    if (ctx->no_mmap) {
	INFO_LOG("Mapping disabled, reading the UKI with pread()");
	return;
    }

    long page_size = sysconf(_SC_PAGESIZE);

    if (page_size <= 0) {
	page_size = 4096;
    }

    off_t map_offset = ctx->uki_offset & ~((off_t)page_size - 1);
    size_t delta = (size_t)(ctx->uki_offset - map_offset);

    ctx->map_len = ctx->uki_size + delta;
    ctx->map_addr = mmap(NULL, ctx->map_len, PROT_READ, MAP_SHARED,
			 ctx->uki_fd, map_offset);
    if (ctx->map_addr == MAP_FAILED) {
	INFO_LOG("Cannot map '%s' (%s), reading the UKI with pread() instead",
		 ctx->uki_path, strerror(errno));
	ctx->map_addr = NULL;
	ctx->map_len = 0;
	return;
    }

    ctx->mmap_base = (char *)ctx->map_addr + delta;

    INFO_LOG("Mapped UKI at %p (%zu bytes)", ctx->mmap_base, ctx->uki_size);
}


/* Custom OCB allocator */
static IOFUNC_OCB_T *uki_ocb_calloc(resmgr_context_t *ctp UNUSED, IOFUNC_ATTR_T *attr) {
    DEBUG_LOG("Allocating uki_ocb_t (size=%zu)", sizeof(uki_ocb_t));

    uki_ocb_t *ocb = calloc(1, sizeof(uki_ocb_t));
    if (!ocb) {
        return NULL;
    }

    /* Get context from attr->mount */
    uki_context_t *ctx = uki_context(attr->mount);

    /* Get entry from attr */
    uki_entry_t *entry = container_of(attr, uki_entry_t, attr);

    /* Set entry pointer */
    ocb->entry = entry;

    /* If this is a directory (root), set dir_node */
    if (S_ISDIR(attr->mode)) {
        /* For root directory, find the corresponding dir_node */
        if (ctx->root && ctx->root->entry == entry) {
            ocb->dir_node = ctx->root;
            DEBUG_PTR("OCB dir_node set to root", ctx->root);
        }
    }

    DEBUG_PTR("OCB entry set to", entry);
    return (IOFUNC_OCB_T *)ocb;
}

/* Custom OCB free */
static void uki_ocb_free(IOFUNC_OCB_T *ocb) {
    DEBUG_LOG("Freeing uki_ocb_t");
    free(ocb);
}

int main(int argc, char *argv[]) {
    uki_context_t *ctx;
    struct stat st;
    resmgr_connect_funcs_t connect_funcs;
    resmgr_io_funcs_t io_funcs;
    iofunc_funcs_t iofuncs;

    DEBUG_LOG("=== UKIFS STARTING ===");

    /* Allocate context */
    ctx = calloc(1, sizeof(uki_context_t));
    if (!ctx) {
        ERROR_LOG("Failed to allocate context");
        return EXIT_FAILURE;
    }

    ctx->uki_fd = -1;
    ctx->resmgr_id = -1;
    g_ctx = ctx;

    /* Parse arguments */
    if (parse_arguments(argc, argv, ctx) != 0) {
        free(ctx);
        return EXIT_FAILURE;
    }

    /* Setup signal handlers */
    signal(SIGINT, signal_handler);
    signal(SIGTERM, signal_handler);
    signal(SIGALRM, alarm_handler);

    /* Open UKI file or ukiboot disk image */
    INFO_LOG("Opening %s: %s",
	     (ctx->slot_sel == UKI_SLOT_SEL_NONE) ? "UKI file" : "ukiboot image",
	     ctx->uki_path);
    ctx->uki_fd = open(ctx->uki_path, O_RDONLY);
    if (ctx->uki_fd < 0) {
        ERROR_LOG("Failed to open UKI file '%s': %s",
		  ctx->uki_path, strerror(errno));
        cleanup(ctx);
        free(ctx);
        return EXIT_FAILURE;
    }

    /* Get the type and the timestamps of the source */
    if (fstat(ctx->uki_fd, &st) != 0) {
        ERROR_LOG("Failed to stat UKI file: %s", strerror(errno));
        cleanup(ctx);
        free(ctx);
        return EXIT_FAILURE;
    }
    /* A size of 0 is not fatal when a slot is selected: the partition
     * table then tells where the UKI is and how large its slot is */
    off_t media_bytes = uki_media_size(ctx->uki_fd);

    /* Block devices carry no meaningful timestamps: date the files of the
     * filesystem from the moment they were published instead of 1970 */
    if (!S_ISREG(st.st_mode) || st.st_mtime == 0) {
        time_t now = time(NULL);

        st.st_mtime = now;
        st.st_ctime = now;
        st.st_atime = now;
    }
    ctx->src_stat = st;

    bool size_is_exact = S_ISREG(st.st_mode);

    INFO_LOG("Source: %s, size: %lld bytes%s",
             size_is_exact ? "regular file" :
             S_ISBLK(st.st_mode) ? "block device" :
             S_ISCHR(st.st_mode) ? "character device" : "special file",
             (long long)media_bytes,
             size_is_exact ? "" : " (as reported by the driver)");

    /* Find the UKI: either the whole media, or a ukiboot slot partition */
    if (locate_uki(ctx, media_bytes, size_is_exact) != 0) {
        cleanup(ctx);
        free(ctx);
        return EXIT_FAILURE;
    }

    /* Map the UKI, or fall back to reading it with pread() */
    map_uki(ctx);

    /* Allocate hash table */
    ctx->hash_table_size = HASH_TABLE_SIZE;
    ctx->hash_table = calloc(ctx->hash_table_size, sizeof(uki_entry_t *));
    if (!ctx->hash_table) {
        ERROR_LOG("Failed to allocate hash table");
        cleanup(ctx);
        free(ctx);
        return EXIT_FAILURE;
    }

    /* Check that the UKI fits in the space available for it */
    uki_size_info_t size_info;
    if (uki_compute_size(ctx->uki_fd, ctx->uki_offset, &size_info)) {
        INFO_LOG("UKI size: %u bytes (%zu bytes available)",
                 size_info.expected_size, ctx->uki_size);

        if ((size_t)size_info.expected_size > ctx->uki_size) {
            ERROR_LOG("Truncated UKI: %u bytes needed, %zu bytes available",
                      size_info.expected_size, ctx->uki_size);
            cleanup(ctx);
            free(ctx);
            return EXIT_FAILURE;
        }

        if ((size_t)size_info.expected_size < ctx->uki_size) {
            size_t padding = ctx->uki_size - size_info.expected_size;
            INFO_LOG("Ignoring %zu bytes of padding after the UKI (%.1f%%)",
                     padding, (padding * 100.0) / ctx->uki_size);
        }
    }

    /* Verify signature if certificate provided */
    if (ctx->cert_path) {
        INFO_LOG("Verifying UKI signature with certificate: %s", ctx->cert_path);

        /* The Authenticode digest covers the PE headers, the sections and
         * the certificate table only. Any padding after the UKI (such as
         * the remainder of a ukiboot slot partition) is not part of it. */
        if (!pe_verify_signature(ctx->uki_fd, ctx->uki_offset, ctx->cert_path)) {
            ERROR_LOG("UKI signature verification failed!");
            ERROR_LOG("The UKI is not properly signed or the signature is invalid.");
            cleanup(ctx);
            free(ctx);
            return EXIT_FAILURE;
        }

        INFO_LOG("UKI signature verified successfully");
    } else {
        INFO_LOG("No certificate provided, signature verification skipped");
    }

    /* Parse UKI file and build directory tree */
    INFO_LOG("Parsing UKI sections...");
    if (uki_parse_sections(ctx) != 0) {
        ERROR_LOG("Failed to parse UKI file");
        cleanup(ctx);
        free(ctx);
        return EXIT_FAILURE;
    }

    /* Initialize QNX iofunc structures */
    iofunc_func_init(_RESMGR_CONNECT_NFUNCS, &connect_funcs,
                     _RESMGR_IO_NFUNCS, &io_funcs);

    iofunc_mount_init(&ctx->mount, sizeof(ctx->mount));
    ctx->mount.conf = IOFUNC_PC_CHOWN_RESTRICTED | IOFUNC_PC_NO_TRUNC;
    ctx->mount.flags = ST_RDONLY | ST_NOSUID;
    ctx->mount.dev = 0;

    /* Setup iofunc funcs for OCB management */
    memset(&iofuncs, 0, sizeof(iofuncs));
    iofuncs.nfuncs = _IOFUNC_NFUNCS;
    iofuncs.ocb_calloc = uki_ocb_calloc;
    iofuncs.ocb_free = uki_ocb_free;
    ctx->mount.funcs = &iofuncs;

    /* Override connect functions */
    connect_funcs.open = uki_open;

    /* Override I/O functions */
    io_funcs.read = uki_read;
    io_funcs.lseek = uki_lseek;
    io_funcs.close_ocb = uki_close_ocb;

    /* Create dispatch handle */
    ctx->dpp = dispatch_create();
    if (!ctx->dpp) {
        ERROR_LOG("Failed to create dispatch: %s", strerror(errno));
        cleanup(ctx);
        free(ctx);
        return EXIT_FAILURE;
    }

    /* Setup resource manager attributes */
    memset(&ctx->resmgr_attr, 0, sizeof(ctx->resmgr_attr));
    ctx->resmgr_attr.nparts_max = 10;
    /* Use large message size for efficient I/O on large UKI sections (64KB) */
    ctx->resmgr_attr.msg_max_size = 65536;

    /* Initialize root directory attributes */
    if (!ctx->root || !ctx->root->entry) {
        /* Create root entry if it doesn't exist */
        uki_entry_t *root_entry = calloc(1, sizeof(uki_entry_t));
        if (!root_entry) {
            ERROR_LOG("Failed to allocate root entry");
            cleanup(ctx);
            free(ctx);
            return EXIT_FAILURE;
        }
        root_entry->name = strdup("/");
        root_entry->mode = S_IFDIR | 0555;
        root_entry->mtime = st.st_mtime;
        iofunc_attr_init(&root_entry->attr, root_entry->mode, 0, 0);
        root_entry->attr.mtime = root_entry->mtime;
        root_entry->attr.ctime = st.st_ctime;
        root_entry->attr.atime = st.st_atime;
        root_entry->attr.mount = &ctx->mount;

        if (ctx->root) {
            ctx->root->entry = root_entry;
        }
    } else {
        ctx->root->entry->attr.mount = &ctx->mount;
    }

    /* Attach resource manager */
    INFO_LOG("Attaching resource manager at %s", ctx->mount_point);
    ctx->resmgr_id = resmgr_attach(ctx->dpp,
                                   &ctx->resmgr_attr,
                                   ctx->mount_point,
                                   _FTYPE_ANY,
                                   _RESMGR_FLAG_DIR,
                                   &connect_funcs,
                                   &io_funcs,
                                   &ctx->root->entry->attr);

    if (ctx->resmgr_id == -1) {
        ERROR_LOG("Failed to attach resource manager: %s", strerror(errno));
        cleanup(ctx);
        free(ctx);
        return EXIT_FAILURE;
    }

    INFO_LOG("Resource manager attached (ID: %d)", ctx->resmgr_id);

    /* Setup thread pool attributes */
    memset(&ctx->pool_attr, 0, sizeof(ctx->pool_attr));
    ctx->pool_attr.handle = ctx->dpp;
    ctx->pool_attr.context_alloc = (THREAD_POOL_PARAM_T *(*)(THREAD_POOL_HANDLE_T *))dispatch_context_alloc;
    ctx->pool_attr.block_func = (THREAD_POOL_PARAM_T *(*)(THREAD_POOL_PARAM_T *))dispatch_block;
    ctx->pool_attr.unblock_func = (void (*)(THREAD_POOL_PARAM_T *))dispatch_unblock;
    ctx->pool_attr.handler_func = (int (*)(THREAD_POOL_PARAM_T *))dispatch_handler;
    ctx->pool_attr.context_free = (void (*)(THREAD_POOL_PARAM_T *))dispatch_context_free;
    ctx->pool_attr.lo_water = 2;
    ctx->pool_attr.hi_water = 4;
    ctx->pool_attr.increment = 1;
    ctx->pool_attr.maximum = 10;

    /* Create and start thread pool */
    ctx->tpool = thread_pool_create(&ctx->pool_attr, POOL_FLAG_EXIT_SELF);
    if (!ctx->tpool) {
        ERROR_LOG("Failed to create thread pool: %s", strerror(errno));
        cleanup(ctx);
        free(ctx);
        return EXIT_FAILURE;
    }

    INFO_LOG("Starting thread pool...");
    if (thread_pool_start(ctx->tpool) != 0) {
        ERROR_LOG("Failed to start thread pool: %s", strerror(errno));
        cleanup(ctx);
        free(ctx);
        return EXIT_FAILURE;
    }

    INFO_LOG("UKI filesystem ready at %s", ctx->mount_point);
    DEBUG_LOG("=== UKIFS INITIALIZED SUCCESSFULLY ===");

    /* Main loop - thread pool handles all requests, just wait for signal */
    pause();

    /* Should never reach here (signal handler exits) */
    cleanup(ctx);
    free(ctx);

    DEBUG_LOG("=== UKIFS EXITING ===");
    return EXIT_SUCCESS;
}
