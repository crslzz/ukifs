/*
 * I/O Callbacks
 */
#ifndef UKIFS_IO_H
#define UKIFS_IO_H

#include <sys/iofunc.h>
#include <sys/resmgr.h>

int uki_open(resmgr_context_t *ctp, io_open_t *msg,
             RESMGR_HANDLE_T *handle, void *extra);
int uki_read(resmgr_context_t *ctp, io_read_t *msg,
             RESMGR_OCB_T *ocb);
int uki_lseek(resmgr_context_t *ctp, io_lseek_t *msg,
              RESMGR_OCB_T *ocb);
int uki_close_ocb(resmgr_context_t *ctp, void *reserved,
                  RESMGR_OCB_T *ocb);

#endif /* UKIFS_IO_H */
