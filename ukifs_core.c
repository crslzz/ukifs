/*
 * QNX UKI Filesystem entry point
 */

#include <stdlib.h>
#include <stdio.h>
#include <string.h>
#include <sys/mman.h>
#include <sys/dispatch.h>

#include "ukifs_core.h"
#include "ukifs_size.h"
#include "ukifs_parse.h"
#include "ukifs_io.h"

#include "pe_parser_posix.h"

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
        if (ctx->mmap_base && ctx->mmap_base != MAP_FAILED) {
            DEBUG_LOG("Unmapping UKI file");
            munmap(ctx->mmap_base, ctx->mmap_size);
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

/* Parse command line arguments */
static int parse_arguments(int argc, char *argv[], uki_context_t *ctx) {
    int opt;
    char *endptr;
    mode_t perm = UKI_FILES_PERM; /* default permissions */
    int log_level = g_log_level; /* default log level */

    /* Initialize optional fields */
    ctx->cert_path = NULL;

    /* Parse options */
    while ((opt = getopt(argc, argv, "vp:d:c:h")) != -1) {
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
	case 'h':
	default:
	    fprintf(stderr, "Usage: %s [options] <uki-file> <mount-point>\n", argv[0]);
	    fprintf(stderr, "\n");
	    fprintf(stderr, "Mount a UKI (Unified Kernel Image) as a read-only filesystem.\n");
	    fprintf(stderr, "\n");
	    fprintf(stderr, "Options:\n");
	    fprintf(stderr, "  -v             Verbose mode (info messages)\n");
	    fprintf(stderr, "  -p <perm>      File permission (default: %04o)\n", UKI_FILES_PERM);
	    fprintf(stderr, "  -d <level>     Debug level: 0=quiet, 1=errors, 2=info, 3=debug (default: %d)\n", g_log_level);
	    fprintf(stderr, "  -c <cert.pem>  X.509 PEM certificate for signature verification\n");
	    fprintf(stderr, "  -h             Show this help\n");
	    fprintf(stderr, "\n");
	    fprintf(stderr, "Examples:\n");
	    fprintf(stderr, "  %s /boot/ukiboot.efi /mnt/uki\n", argv[0]);
	    fprintf(stderr, "  %s -v /boot/ukiboot.efi /mnt/uki\n", argv[0]);
	    fprintf(stderr, "  %s -c /etc/uki-cert.pem /boot/ukiboot.efi /mnt/uki\n", argv[0]);
	    fprintf(stderr, "  %s -d 3 /boot/ukiboot.efi /mnt/uki  # Full debug traces\n", argv[0]);
	    return -1;
        }
    }

    /* Check for required positional arguments */
    if (optind + 2 != argc) {
        fprintf(stderr, "Error: Missing required arguments\n");
        fprintf(stderr, "Usage: %s [options] <uki-file> <mount-point>\n", argv[0]);
        fprintf(stderr, "Try '%s -h' for more information.\n", argv[0]);
        return -1;
    }

    g_log_level = log_level;

    ctx->uki_path = argv[optind];
    ctx->mount_point = argv[optind + 1];
    ctx->perm = perm;

    DEBUG_STR("UKI file", ctx->uki_path);
    DEBUG_STR("Mount point", ctx->mount_point);

    return 0;
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

    /* Open UKI file */
    INFO_LOG("Opening UKI file: %s", ctx->uki_path);
    ctx->uki_fd = open(ctx->uki_path, O_RDONLY);
    if (ctx->uki_fd < 0) {
        ERROR_LOG("Failed to open UKI file '%s': %s",
		  ctx->uki_path, strerror(errno));
        cleanup(ctx);
        free(ctx);
        return EXIT_FAILURE;
    }

    /* Get file size */
    if (fstat(ctx->uki_fd, &st) != 0) {
        ERROR_LOG("Failed to stat UKI file: %s", strerror(errno));
        cleanup(ctx);
        free(ctx);
        return EXIT_FAILURE;
    }
    ctx->mmap_size = st.st_size;

    INFO_LOG("UKI file size: %zu bytes", ctx->mmap_size);

    /* Memory-map the UKI file */
    ctx->mmap_base = mmap(NULL, ctx->mmap_size, PROT_READ,
                          MAP_SHARED, ctx->uki_fd, 0);
    if (ctx->mmap_base == MAP_FAILED) {
        ERROR_LOG("Failed to mmap UKI file: %s", strerror(errno));
        cleanup(ctx);
        free(ctx);
        return EXIT_FAILURE;
    }

    INFO_LOG("Mapped UKI file at %p", ctx->mmap_base);

    /* Allocate hash table */
    ctx->hash_table_size = HASH_TABLE_SIZE;
    ctx->hash_table = calloc(ctx->hash_table_size, sizeof(uki_entry_t *));
    if (!ctx->hash_table) {
        ERROR_LOG("Failed to allocate hash table");
        cleanup(ctx);
        free(ctx);
        return EXIT_FAILURE;
    }

    /* Verify signature if certificate provided */
    if (ctx->cert_path) {
        INFO_LOG("Verifying UKI signature with certificate: %s", ctx->cert_path);

        /* Compute actual UKI size (excluding padding) */
        uki_size_info_t size_info;
        if (!uki_compute_size(ctx->uki_fd, &size_info)) {
            ERROR_LOG("Failed to compute UKI size for signature verification");
            cleanup(ctx);
            free(ctx);
            return EXIT_FAILURE;
        }

        INFO_LOG("File size: %zu bytes, UKI size: %u bytes",
                 ctx->mmap_size, size_info.expected_size);

        int verify_fd = ctx->uki_fd;
        int temp_fd = -1;

        /* Check if file has padding beyond the UKI */
        if ((size_t)size_info.expected_size < ctx->mmap_size) {
            size_t padding = ctx->mmap_size - size_info.expected_size;
            INFO_LOG("Detected %zu bytes of padding (%.1f%% of file)",
                     padding, (padding * 100.0) / ctx->mmap_size);
            INFO_LOG("Creating temporary file for signature verification (excluding padding)");

            /* Create temporary file with just UKI data */
            char temp_path[] = "/tmp/ukifs_verify_XXXXXX";
            temp_fd = mkstemp(temp_path);
            if (temp_fd < 0) {
                ERROR_LOG("Failed to create temporary file: %s", strerror(errno));
                cleanup(ctx);
                free(ctx);
                return EXIT_FAILURE;
            }

            /* Unlink immediately so it's cleaned up on exit */
            unlink(temp_path);

            /* Copy UKI data to temporary file */
            off_t remaining = size_info.expected_size;
            off_t offset = 0;
            char buffer[64 * 1024];

            while (remaining > 0) {
                size_t to_read = (remaining > (off_t)sizeof(buffer)) ? sizeof(buffer) : (size_t)remaining;
                ssize_t nread = pread(ctx->uki_fd, buffer, to_read, offset);
                if (nread <= 0) {
                    ERROR_LOG("Failed to read UKI data: %s", strerror(errno));
                    close(temp_fd);
                    cleanup(ctx);
                    free(ctx);
                    return EXIT_FAILURE;
                }

                ssize_t nwritten = write(temp_fd, buffer, nread);
                if (nwritten != nread) {
                    ERROR_LOG("Failed to write to temporary file: %s", strerror(errno));
                    close(temp_fd);
                    cleanup(ctx);
                    free(ctx);
                    return EXIT_FAILURE;
                }

                offset += nread;
                remaining -= nread;
            }

            /* Use temporary file for verification */
            verify_fd = temp_fd;
            lseek(verify_fd, 0, SEEK_SET);
            INFO_LOG("Verifying signature against UKI data only (%u bytes)", size_info.expected_size);
        }

        /* Verify signature */
        bool sig_valid = pe_verify_signature(verify_fd, 0, ctx->cert_path);

        /* Cleanup temporary file if created */
        if (temp_fd >= 0) {
            close(temp_fd);
        }

        if (!sig_valid) {
            ERROR_LOG("UKI signature verification failed!");
            ERROR_LOG("The UKI file is not properly signed or the signature is invalid.");
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
