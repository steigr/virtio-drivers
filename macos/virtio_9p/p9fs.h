/*
 * p9fs: the "9p" filesystem of Virtio9P.kext, a 9P2000.L client for Mac OS X 10.0 to 10.13, on
 * PowerPC and Intel. See README.md for what it does and does not do.
 *
 * Layers, bottom up; the first two are every release's, the frontend one of two:
 *
 *   p9_transport.h   the virtio device: one request, one reply, one at a time (Virtio9P.cpp)
 *   p9fs_proto.c     9P2000.L messages as C calls, the channel lock and the fid allocator
 *   p9fs_bsd_*.c     the frontend for the 4.4BSD-Lite VFS of 10.0 to 10.3 (P9FS_KPI unset):
 *                    nodes, vnode operations, mount and registration
 *   p9fs_kpi_*.c     the frontend for the VFS KPI of 10.4 on (P9FS_KPI): the same, written
 *                    against vnode_t, vnop_*_args and vfs_fsadd
 *
 * Locking, BSD frontend. All of it runs under the kernel funnel, which a thread gives up whenever
 * it blocks, so state is safe between sleeps and must be looked at again after one. Vnodes are
 * not locked at all, as NFS's are not on these releases (vop_nolock). The device carries one
 * message at a time; whoever is sending holds the mount's channel, and sleeps for it otherwise.
 * The channel is held only inside p9fs_proto.c, never across anything that can come back into
 * this filesystem: a page fault on a mapped 9p file, vnode recycling, the UBC. So data goes
 * through kernel buffers, and a fid a call works on is read from the node only once the channel
 * is held (callers pass the field's address), so that one swapped or closed meanwhile is never
 * sent.
 *
 * Locking, KPI frontend. There is no funnel -- the x86_64 kernels have none -- so each mount has
 * a mutex that plays its part: taken on the way into every operation, held across the messages
 * it sends, and given up around everything that can come back into this filesystem or wait for
 * one of its vnodes (uiomove, the UBC, vnode_create, vnode_get/put, vnode_recycle), as the funnel
 * was given up at a sleep. The reasoning above holds unchanged: state is looked at again after
 * any of those. The channel is then never contended, and p9fs_proto.c needs no lock of its own.
 */

#ifndef _P9FS_H
#define _P9FS_H

#include <sys/param.h>
#include <sys/systm.h>
#include <sys/queue.h>
#include <sys/vnode.h>
#include <sys/mount.h>
#ifdef P9FS_KPI
#include <kern/locks.h>
#endif

#include "p9fs_args.h"

#define P9_NOFID            ((u_int32_t) 0xffffffff)
#define P9_IOHDRSZ          24          /* what a Tread/Twrite/Rread costs besides its data */
#define P9_MAXFIDS          65536

/* qid.type */
#define P9_QTDIR            0x80
#define P9_QTSYMLINK        0x02

/* 9P2000.L open flags: Linux's, which QEMU's server translates. */
#define P9_RDONLY           00000000
#define P9_WRONLY           00000001
#define P9_RDWR             00000002
#define P9_CREAT            00000100
#define P9_EXCL             00000200
#define P9_TRUNC            00001000
#define P9_APPEND           00002000
#define P9_DIRECTORY        00200000

#define P9_AT_REMOVEDIR     0x200

/* Tgetattr request mask */
#define P9_GETATTR_BASIC    0x000007ffULL

/* Tsetattr valid mask */
#define P9_SETATTR_MODE     0x00000001
#define P9_SETATTR_UID      0x00000002
#define P9_SETATTR_GID      0x00000004
#define P9_SETATTR_SIZE     0x00000008
#define P9_SETATTR_ATIME    0x00000010
#define P9_SETATTR_MTIME    0x00000020
#define P9_SETATTR_CTIME    0x00000040
#define P9_SETATTR_ATIME_SET 0x00000080
#define P9_SETATTR_MTIME_SET 0x00000100

struct p9_qid {
    u_int8_t    type;
    u_int32_t   version;
    u_int64_t   path;
};

struct p9_attr {
    struct p9_qid qid;
    u_int32_t   mode;
    u_int32_t   uid;
    u_int32_t   gid;
    u_int64_t   nlink;
    u_int64_t   rdev;
    u_int64_t   size;
    u_int64_t   blksize;
    u_int64_t   blocks;
    u_int64_t   atime_sec;
    u_int64_t   atime_nsec;
    u_int64_t   mtime_sec;
    u_int64_t   mtime_nsec;
    u_int64_t   ctime_sec;
    u_int64_t   ctime_nsec;
};

struct p9_setattr {
    u_int32_t   valid;
    u_int32_t   mode;
    u_int32_t   uid;
    u_int32_t   gid;
    u_int64_t   size;
    u_int64_t   atime_sec;
    u_int64_t   atime_nsec;
    u_int64_t   mtime_sec;
    u_int64_t   mtime_nsec;
};

struct p9_statfs {
    u_int32_t   type;
    u_int32_t   bsize;
    u_int64_t   blocks;
    u_int64_t   bfree;
    u_int64_t   bavail;
    u_int64_t   files;
    u_int64_t   ffree;
    u_int64_t   fsid;
    u_int32_t   namelen;
};

struct p9node;
LIST_HEAD(p9nodehash, p9node);

/* One per mount, which is one per device. */
struct p9mount {
    struct mount       *pm_mp;
    struct p9fs_args    pm_args;
    void               *pm_dev;         /* p9_transport.h's handle */
    u_int8_t           *pm_tbuf;        /* the device's request buffer */
    u_int8_t           *pm_rbuf;        /* ... and reply buffer */
    u_int32_t           pm_msize;       /* the negotiated message size */
    u_int32_t           pm_iosize;      /* the most data one Tread or Twrite carries */
    int                 pm_chan;        /* the channel is held */
    int                 pm_chanwant;    /* ... and someone sleeps for it */
    int                 pm_dead;        /* the device has gone: everything fails */
    u_int8_t           *pm_fidmap;      /* a bit per fid in use */
    u_int32_t           pm_fidnext;     /* where the next search for a free fid starts */
    struct p9node      *pm_root;
    struct p9nodehash  *pm_hash;        /* nodes by qid.path */
    u_long              pm_hashmask;
    int                 pm_hashlock;    /* a node is being made: others wait, then look again */
#ifdef P9FS_KPI
    lck_mtx_t          *pm_lock;        /* the frontend's: see above */
    u_int64_t           pm_rootqpath;   /* the root's qid.path, so p9_nget can mark its vnode */
#endif
};

/* p9node.n_flag */
#define NP_UNHASHED         0x0001      /* removed or replaced: lookups must not find it */
#define NP_MODIFIED         0x0002      /* we wrote to it: a changed mtime is our own doing */
#define NP_WONLY            0x0004      /* n_wfid was opened write-only */

/* One per vnode, and one vnode per qid.path. */
struct p9node {
    LIST_ENTRY(p9node)  n_hash;
    struct vnode       *n_vnode;
    struct p9mount     *n_pm;
    int                 n_flag;
    u_int64_t           n_qpath;        /* qid.path: the file's identity */
    u_int8_t            n_qtype;
    u_int32_t           n_fid;          /* walked to the file, never opened: for path operations */
    u_int32_t           n_rfid;         /* opened read-only, or P9_NOFID */
    u_int32_t           n_wfid;         /* opened read-write (or write-only), or P9_NOFID */
    u_int32_t           n_dfid;         /* the directory opened for Treaddir, or P9_NOFID */
    struct p9_attr      n_attr;         /* the last Rgetattr */
    time_t              n_attrtime;     /* when, in seconds of uptime; 0 if it must be fetched */
    u_int64_t           n_size;         /* the size the UBC was last told */
    u_int64_t           n_openmtime;    /* mtime at the last open, to notice others' changes */
    off_t               n_diroff;       /* readdir: the entry index next to be returned, */
    u_int64_t           n_dircookie;    /* ... and the server's offset of that entry */
};

#ifdef P9FS_KPI
#define VTOP9(vp)           ((struct p9node *) vnode_fsnode(vp))
#define VFSTOP9(mp)         ((struct p9mount *) vfs_fsprivate(mp))
#else
#define VTOP9(vp)           ((struct p9node *) (vp)->v_data)
#define VFSTOP9(mp)         ((struct p9mount *) (mp)->mnt_data)
#endif
#define P9TOV(np)           ((np)->n_vnode)

/* How p9fs_proto.c waits for the channel: under the funnel, a sleep that gives it up; under the
 * KPI frontend's mount lock, which its callers hold, never (see above), but the same sleep on
 * that lock if it ever is. */
#ifdef P9FS_KPI
#define P9_CHANSLEEP(pm)    ((void) msleep(&(pm)->pm_chan, (pm)->pm_lock, PRIBIO, "9pchan", NULL))
#else
#define P9_CHANSLEEP(pm)    ((void) tsleep((caddr_t) &(pm)->pm_chan, PRIBIO, "9pchan", 0))
#endif

#define P9_ATTRTIMEO        2           /* an Rgetattr is believed for 1 to 2 seconds */

extern int (**p9fs_vnodeop_p)(void *);

/* p9fs_proto.c: each returns 0 or a BSD errno. A u_int32_t * fid is a node field, read only once
 * the channel is held. */
int     p9_errno(u_int32_t linux_errno);
u_int32_t p9_fid_alloc(struct p9mount *pm);
void    p9_fid_free(struct p9mount *pm, u_int32_t fid);
int     p9_version(struct p9mount *pm);
int     p9_attach(struct p9mount *pm, u_int32_t fid, struct p9_qid *qid);
int     p9_walk(struct p9mount *pm, u_int32_t *fidp, u_int32_t newfid, const char *name, int namelen,
                struct p9_qid *qid);
int     p9_clunk(struct p9mount *pm, u_int32_t fid);
int     p9_lopen(struct p9mount *pm, u_int32_t *fidp, u_int32_t flags);
int     p9_lcreate(struct p9mount *pm, u_int32_t *fidp, const char *name, int namelen, u_int32_t flags,
                   u_int32_t mode, u_int32_t gid, struct p9_qid *qid);
int     p9_getattr(struct p9mount *pm, u_int32_t *fidp, struct p9_attr *attr);
int     p9_setattr(struct p9mount *pm, u_int32_t *fidp, struct p9_setattr *sa);
int     p9_read(struct p9mount *pm, u_int32_t *fidp, u_int64_t offset, u_int32_t count, void *buf,
                u_int32_t *got);
int     p9_write(struct p9mount *pm, u_int32_t *fidp, u_int64_t offset, u_int32_t count, const void *buf,
                 u_int32_t *done);
int     p9_readdir(struct p9mount *pm, u_int32_t *fidp, u_int64_t offset, u_int32_t count, void *buf,
                   u_int32_t *got);
int     p9_mkdir(struct p9mount *pm, u_int32_t *dfidp, const char *name, int namelen, u_int32_t mode,
                 u_int32_t gid, struct p9_qid *qid);
int     p9_symlink(struct p9mount *pm, u_int32_t *dfidp, const char *name, int namelen, const char *target,
                   u_int32_t gid, struct p9_qid *qid);
int     p9_link(struct p9mount *pm, u_int32_t *dfidp, u_int32_t *fidp, const char *name, int namelen);
int     p9_readlink(struct p9mount *pm, u_int32_t *fidp, char *buf, u_int32_t bufsize, u_int32_t *len);
int     p9_unlinkat(struct p9mount *pm, u_int32_t *dfidp, const char *name, int namelen, u_int32_t flags);
int     p9_renameat(struct p9mount *pm, u_int32_t *odfidp, const char *oname, int onamelen,
                    u_int32_t *ndfidp, const char *nname, int nnamelen);
int     p9_statfs(struct p9mount *pm, u_int32_t *fidp, struct p9_statfs *sf);
int     p9_fsync(struct p9mount *pm, u_int32_t *fidp);

/* p9fs_bsd_node.c, p9fs_kpi_node.c */
int     p9_nodeinit(struct p9mount *pm);
void    p9_nodefini(struct p9mount *pm);
int     p9_nget(struct p9mount *pm, u_int32_t fid, struct p9_qid *qid, struct p9_attr *attr,
                struct vnode **vpp);
void    p9_unhash(struct p9node *np);
void    p9_attr_load(struct p9node *np, struct p9_attr *attr);
int     p9_attr_refresh(struct p9node *np, int force);
int     p9_openfid(struct p9node *np, int fflag, u_int32_t **fidpp);
void    p9_closefids(struct p9node *np);
u_int32_t p9_fileid(u_int64_t qpath);
enum vtype p9_vtype(u_int32_t mode);
#ifndef P9FS_KPI
int     p9fs_inactive(void *v);
int     p9fs_reclaim(void *v);
#endif

/* p9fs_bsd_vfsops.c, p9fs_kpi_vfsops.c */
int     p9fs_register(void);
int     p9fs_unregister(void);

#endif /* !_P9FS_H */
