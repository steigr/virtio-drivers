/*
 * What the KPI frontend's three files share: the mount lock, taken and given up (p9fs.h says
 * when), and the UBC calls whose names changed between the releases it runs on.
 */

#ifndef _P9FS_KPI_H
#define _P9FS_KPI_H

#include <sys/ubc.h>

#define P9_LOCK(pm)         lck_mtx_lock((pm)->pm_lock)
#define P9_UNLOCK(pm)       lck_mtx_unlock((pm)->pm_lock)

/* Pushing a file's dirty pages to us and dropping its pages: 10.4's ubc_sync_range, which 10.5
 * renamed ubc_msync. The i386 slice is built against 10.4's headers and runs on 10.4 to 10.7;
 * the x86_64 one against 10.6's. */
#ifdef P9FS_UBC_MSYNC
#define P9_UBC_SYNC(vp, start, end, flags)  ((void) ubc_msync((vp), (start), (end), NULL, (flags)))
#else
#define P9_UBC_SYNC(vp, start, end, flags)  ((void) ubc_sync_range((vp), (start), (end), (flags)))
#endif

extern struct vnodeopv_desc p9fs_vnodeop_opv_desc;

#endif /* !_P9FS_KPI_H */
