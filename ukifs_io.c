/*
 * uki_io.c - I/O Callbacks for UKI Filesystem
 */
#include <stdlib.h>
#include <stddef.h>
#include <string.h>
#include <dirent.h>

#include "ukifs_io.h"
#include "ukifs_core.h"

/*
 * Open callback - handles path resolution and file opening
 */
int uki_open(resmgr_context_t *ctp, io_open_t *msg,
             RESMGR_HANDLE_T *handle, void *extra) {
    DEBUG_ENTER();

    /* Get attr from handle, then get context from mount pointer */
    IOFUNC_ATTR_T *attr = (IOFUNC_ATTR_T *)handle;
    uki_context_t *ctx = uki_context(attr->mount);

    /* Extract path from message buffer */
    char *path = (char *)msg + sizeof(io_open_t) - 4;
    DEBUG_STR("path", path);

    /* Handle root directory */
    if (strcmp(path, "/") == 0 || strcmp(path, "") == 0) {
        DEBUG_LOG("Opening root directory");
        /* iofunc_open_default will call our OCB allocator which will set dir_node */
        int result = iofunc_open_default(ctp, msg, &ctx->root->entry->attr, extra);
        DEBUG_VAL("iofunc_open_default result", result);
        DEBUG_EXIT(result);
        return result;
    }

    /* Look up path in hash table */
    DEBUG_LOG("Looking up path in hash table");
    uki_entry_t *entry = lookup_path(ctx, path);
    DEBUG_PTR("entry", entry);
    if (!entry) {
        DEBUG_LOG("Path not found in hash table");
        DEBUG_EXIT(ENOENT);
        return ENOENT;
    }

    /* Log file open */
    INFO_LOG("[OPEN] file='%s' size=%u offset=%u",
             entry->name, entry->size, entry->file_offset);

    /* Check access permissions */
    DEBUG_LOG("Checking access permissions");
    struct _client_info info;
    iofunc_client_info(ctp, msg->connect.ioflag, &info);
    int status = iofunc_check_access(ctp, &entry->attr, 0, &info);
    DEBUG_VAL("iofunc_check_access status", status);
    if (status != EOK) {
        DEBUG_LOG("Access denied");
        DEBUG_EXIT(status);
        return status;
    }

    /* Reject write attempts on read-only filesystem */
    if ((msg->connect.ioflag & (_IO_FLAG_WR | O_CREAT | O_TRUNC)) != 0) {
        DEBUG_LOG("Write attempt on read-only filesystem");
        DEBUG_EXIT(EROFS);
        return EROFS;
    }

    /* Use default open handling which will allocate the OCB */
    DEBUG_LOG("Calling iofunc_open_default");
    int result = iofunc_open_default(ctp, msg, &entry->attr, extra);
    DEBUG_VAL("iofunc_open_default result", result);
    DEBUG_EXIT(result);
    return result;
}

/*
 * Read directory entries
 */
static int uki_readdir(resmgr_context_t *ctp, io_read_t *msg, uki_ocb_t *ocb) {
    DEBUG_ENTER();

    if (!S_ISDIR(ocb->ocb.attr->mode)) {
        DEBUG_LOG("not a directory");
        DEBUG_EXIT(ENOTDIR);
        return ENOTDIR;
    }

    dir_node_t *dir = ocb->dir_node;

    if (!dir) {
        DEBUG_LOG("dir_node is NULL");
        DEBUG_EXIT(ENOTDIR);
        return ENOTDIR;
    }

    size_t index = ocb->dir_index;
    DEBUG_LOG("dir_index %ld, num_children %ld", index, dir->num_children);

    /* Check if we've read all entries */
    if (index >= dir->num_children + 2) {
        DEBUG_LOG("All entries read");
        _IO_SET_READ_NBYTES(ctp, 0);
        DEBUG_EXIT(0);
        return _RESMGR_NPARTS(0);
    }

    /* Build dirent structures in a buffer */
    char *buffer = malloc(4096);
    if (!buffer) {
        DEBUG_LOG("Failed to allocate buffer");
        DEBUG_EXIT(ENOMEM);
        return ENOMEM;
    }

    size_t buf_used = 0;
    size_t nbytes = _IO_READ_GET_NBYTES(msg);

    /* Add "." and ".." entries first */
    if (index < 2) {
        const char *name = (index == 0) ? "." : "..";
        size_t name_len = strlen(name);
        size_t dirent_size = offsetof(struct dirent, d_name) + name_len + 1;
        dirent_size = (dirent_size + 7) & ~7;  /* 8-byte align */

        if (buf_used + dirent_size <= 4096) {
            struct dirent *dent = (struct dirent *)(buffer + buf_used);
            memset(dent, 0, dirent_size);
            dent->d_ino = (index == 0) ?
		(dir->entry ? dir->entry->attr.inode : (ino_t)dir) :
		(dir->entry ? dir->entry->attr.inode : (ino_t)dir);
            dent->d_offset = index;
            dent->d_reclen = dirent_size;
            dent->d_namelen = name_len;
            strcpy(dent->d_name, name);
            buf_used += dirent_size;
            index++;
        }
    }

    /* Add directory entries (section files) */
    size_t child_index = (index >= 2) ? index - 2 : 0;
    while (child_index < dir->num_children && buf_used < nbytes) {
        dir_node_t *child = dir->children[child_index];
        const char *child_name = child->name;
        size_t name_len = strlen(child_name);
        size_t dirent_size = offsetof(struct dirent, d_name) + name_len + 1;
        dirent_size = (dirent_size + 7) & ~7;  /* 8-byte align */

        if (buf_used + dirent_size > 4096) {
            break;
        }

        struct dirent *dent = (struct dirent *)(buffer + buf_used);
        memset(dent, 0, dirent_size);
        dent->d_ino = child->entry ? child->entry->attr.inode : (ino_t)child;
        dent->d_offset = child_index + 2;
        dent->d_reclen = dirent_size;
        dent->d_namelen = name_len;
        strcpy(dent->d_name, child_name);

        buf_used += dirent_size;
        child_index++;
        index = child_index + 2;
    }

    /* Update position */
    ocb->dir_index = index;

    /* Mark as read */
    if (ocb->ocb.attr) {
        ocb->ocb.attr->flags |= IOFUNC_ATTR_ATIME;
    }

    /* Return directory entries */
    DEBUG_VAL("buf_used", buf_used);
    _IO_SET_READ_NBYTES(ctp, buf_used);
    SETIOV(ctp->iov, buffer, buf_used);

    DEBUG_EXIT(1);
    return _RESMGR_NPARTS(1);
}

/*
 * Read callback - handles reading file data or directory entries
 */
int uki_read(resmgr_context_t *ctp, io_read_t *msg, RESMGR_OCB_T *ocb_base) {
    DEBUG_ENTER();

    uki_ocb_t *ocb = (uki_ocb_t *)ocb_base;
    uki_context_t *ctx = uki_context(ocb->ocb.attr->mount);

    int status;
    int nonblock = 0;
    off_t offset = 0;

    /* Verify read request */
    status = iofunc_read_verify(ctp, msg, &ocb->ocb, &nonblock);
    DEBUG_VAL("iofunc_read_verify status", status);
    if (status != EOK) {
        DEBUG_LOG("Read verify failed");
        DEBUG_EXIT(status);
        return status;
    }

    /* Check for XTYPE override */
    int xtype = msg->i.xtype & _IO_XTYPE_MASK;
    DEBUG_VALX("xtype", xtype);

    if (xtype == _IO_XTYPE_READDIR || S_ISDIR(ocb->ocb.attr->mode)) {
        /* Handle directory reads */
        int result = uki_readdir(ctp, msg, ocb);
        DEBUG_EXIT(result);
        return result;
    }

    /* Handle regular file reads */
    if (!S_ISREG(ocb->ocb.attr->mode)) {
        DEBUG_LOG("Not a regular file");
        DEBUG_EXIT(EISDIR);
        return EISDIR;
    }

    if (xtype == _IO_XTYPE_OFFSET) {
        struct _xtype_offset *xoffset = (struct _xtype_offset *) (&msg -> i + 1);
        offset = xoffset->offset;
        DEBUG_VAL("_IO_XTYPE_OFFSET: xoffset", offset);
    } else if (xtype == _IO_XTYPE_NONE) {
        offset = ocb_base->offset;
        DEBUG_VAL("_IO_XTYPE_NONE: offset", offset);
    } else {
        return ENOSYS;
    }

    /* Get read parameters */
    size_t nbytes = _IO_READ_GET_NBYTES(msg);

    /* Log file access details */
    INFO_LOG("[READ] file='%s' offset=%ld size=%u requested=%zu, flags=%x [%s]",
             ocb->entry->name, (long)offset, ocb->entry->size, nbytes,
             ocb_base->flags,
             (ocb_base->flags & IOFUNC_OCB_MMAP) ? "MMAP" : "");

    /* Clamp to file size */
    if (offset >= (off_t)ocb->entry->size) {
        INFO_LOG("[READ] file='%s' EOF reached at offset=%ld, returning 0 bytes",
                 ocb->entry->name, (long)offset);
        _IO_SET_READ_NBYTES(ctp, 0);
        DEBUG_EXIT(0);
        return _RESMGR_NPARTS(0);
    }

    if ((off_t)(offset + nbytes) > (off_t)ocb->entry->size) {
        size_t original_nbytes = nbytes;
        nbytes = ocb->entry->size - offset;
        INFO_LOG("[READ] file='%s' clamped request from %zu to %zu bytes",
                 ocb->entry->name, original_nbytes, nbytes);
    }

    /* Calculate the actual file position being read */
    uint32_t file_position = ocb->entry->file_offset + offset;
    const char *data = (const char *)ctx->mmap_base + file_position;
    DEBUG_LOG("[READ] file='%s' mmap_base=%p + file_position=%u = data %p",
              ocb->entry->name, ctx->mmap_base, file_position, data);

    /* Log data chunk being returned */
    DEBUG_LOG("[READ] file='%s' returning chunk: file_offset=%u section_offset=%ld nbytes=%zu (file_pos %u-%u)",
              ocb->entry->name, ocb->entry->file_offset, (long)offset, nbytes,
              file_position, file_position + (uint32_t)nbytes - 1);

    /* Update offset for next read */
    ocb->ocb.offset += nbytes;

    /* Mark as read */
    if (ocb->ocb.attr) {
        ocb->ocb.attr->flags |= IOFUNC_ATTR_ATIME;
    }

    /* Return data using zero-copy (SETIOV points into mmap region) */
    _IO_SET_READ_NBYTES(ctp, nbytes);
    SETIOV(ctp->iov, (void *)data, nbytes);

    DEBUG_VAL("returning nbytes", nbytes);
    DEBUG_EXIT(1);
    return _RESMGR_NPARTS(1);
}

/*
 * Lseek callback - handles file position changes
 */
int uki_lseek(resmgr_context_t *ctp, io_lseek_t *msg, RESMGR_OCB_T *ocb_base) {
    DEBUG_ENTER();

    uki_ocb_t *ocb = (uki_ocb_t *)ocb_base;

    /* Directories should use iofunc default lseek */
    if (S_ISDIR(ocb->ocb.attr->mode)) {
        DEBUG_LOG("Handling directory lseek");
        int result = iofunc_lseek_default(ctp, msg, &ocb->ocb);
        DEBUG_EXIT(result);
        return result;
    }

    /* For regular files, handle seek operation */
    if (!S_ISREG(ocb->ocb.attr->mode)) {
        DEBUG_LOG("Not a regular file or directory");
        DEBUG_EXIT(ENOSYS);
        return ENOSYS;
    }

    off_t offset = 0;
    int whence = msg->i.whence;

    DEBUG_VAL("requested offset", msg->i.offset);
    DEBUG_VAL("whence", whence);
    DEBUG_VAL("current offset", ocb->ocb.offset);
    DEBUG_VAL("file size", ocb->entry->size);

    /* Calculate new offset based on whence */
    switch (whence) {
    case SEEK_SET:
	offset = msg->i.offset;
	break;
    case SEEK_CUR:
	offset = ocb->ocb.offset + msg->i.offset;
	break;
    case SEEK_END:
	offset = ocb->entry->size + msg->i.offset;
	break;
    default:
	DEBUG_LOG("Invalid whence value");
	DEBUG_EXIT(EINVAL);
	return EINVAL;
    }

    /* Allow seeking past end of file (POSIX allows this) */
    if (offset < 0) {
        DEBUG_LOG("Negative offset not allowed");
        DEBUG_EXIT(EINVAL);
        return EINVAL;
    }

    /* Log lseek operation */
    INFO_LOG("[LSEEK] file='%s' whence=%s offset=%ld -> new_offset=%ld (size=%u)",
             ocb->entry->name,
             (whence == SEEK_SET) ? "SET" : (whence == SEEK_CUR) ? "CUR" : "END",
             (long)msg->i.offset,
             (long)offset,
             ocb->entry->size);

    /* Update offset */
    ocb->ocb.offset = offset;

    /* Return new offset */
    msg->o = offset;

    DEBUG_EXIT(EOK);
    return EOK;
}

/*
 * Close OCB callback
 */
int uki_close_ocb(resmgr_context_t *ctp, void *reserved, RESMGR_OCB_T *ocb_base) {
    DEBUG_ENTER();

    uki_ocb_t *ocb = (uki_ocb_t *)ocb_base;

    /* Log file close with final offset (total bytes read) */
    if (ocb->entry) {
        INFO_LOG("[CLOSE] file='%s' final_offset=%ld total_read=%ld size=%u",
                 ocb->entry->name,
                 (long)ocb->ocb.offset,
                 (long)ocb->ocb.offset,
                 ocb->entry->size);
    }

    int result = iofunc_close_ocb_default(ctp, reserved, ocb_base);
    DEBUG_EXIT(result);
    return result;
}
