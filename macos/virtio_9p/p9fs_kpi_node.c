/*
 * p9fs nodes for the VFS KPI of 10.4 on: p9fs_bsd_node.c's, with vnode_t in place of struct vnode
 * and the mount lock in place of the funnel (p9fs.h). The policy is that file's and is not
 * repeated here: one vnode per qid.path, a fresh walked fid handed over by each lookup, fids
 * opened for I/O kept until the vnode goes inactive.
 *
 * Everything here runs with the mount lock held, and gives it up around vnode_create,
 * vnode_getwithvid and ubc_setsize, which can wait for a vnode of this filesystem that another
 * operation is reclaiming, or page out to it. What was looked at before is looked at again.
 */

#include "p9fs.h"
#include "p9fs_kpi.h"

#include <sys/fcntl.h>
#include <sys/stat.h>
#include <sys/errno.h>
#include <sys/malloc.h>
#include <sys/time.h>

/* A fixed number of buckets: a mount has as many nodes as the kernel lets it keep vnodes, and
 * qid.paths are inode numbers, which spread over the low bits. */
#define P9_HASHSIZE         256

int
p9_nodeinit(struct p9mount *pm)
{
    int i;

    MALLOC(pm->pm_hash, struct p9nodehash *, P9_HASHSIZE * sizeof(struct p9nodehash), M_TEMP, M_WAITOK);
    if (pm->pm_hash == NULL)
        return ENOMEM;
    for (i = 0; i < P9_HASHSIZE; i++)
        LIST_INIT(&pm->pm_hash[i]);
    pm->pm_hashmask = P9_HASHSIZE - 1;
    return 0;
}

void
p9_nodefini(struct p9mount *pm)
{
    if (pm->pm_hash)
        FREE(pm->pm_hash, M_TEMP);
    pm->pm_hash = NULL;
}

/* As p9fs_bsd_node.c's: stat's inode number and readdir's file number must agree. */
u_int32_t
p9_fileid(u_int64_t qpath)
{
    u_int32_t id = (u_int32_t) qpath ^ (u_int32_t) (qpath >> 32);

    return id ? id : 0xffffffff;
}

enum vtype
p9_vtype(u_int32_t mode)
{
    switch (mode & S_IFMT) {
    case S_IFDIR:   return VDIR;
    case S_IFREG:   return VREG;
    case S_IFLNK:   return VLNK;
    case S_IFCHR:   return VCHR;
    case S_IFBLK:   return VBLK;
    case S_IFIFO:   return VFIFO;
    case S_IFSOCK:  return VSOCK;
    default:        return VNON;
    }
}

static struct p9nodehash *
p9_hashhead(struct p9mount *pm, u_int64_t qpath)
{
    return &pm->pm_hash[((u_long) qpath ^ (u_long) (qpath >> 32)) & pm->pm_hashmask];
}

/* Seconds of uptime, never 0, which n_attrtime keeps for "fetch them". */
static time_t
p9_now(void)
{
    struct timeval tv;

    microuptime(&tv);
    return tv.tv_sec ? tv.tv_sec : 1;
}

void
p9_unhash(struct p9node *np)
{
    if (!(np->n_flag & NP_UNHASHED)) {
        LIST_REMOVE(np, n_hash);
        np->n_flag |= NP_UNHASHED;
    }
}

/* Takes in an Rgetattr. A regular file's new size goes to the UBC, with the lock given up. */
void
p9_attr_load(struct p9node *np, struct p9_attr *attr)
{
    struct p9mount *pm = np->n_pm;
    vnode_t vp = P9TOV(np);

    np->n_attr = *attr;
    np->n_attrtime = p9_now();
    if (vp == NULL)
        return;                 /* being made: p9_nget sets the rest */
    if (vnode_vtype(vp) != VNON && p9_vtype(attr->mode) != vnode_vtype(vp)) {
        /* Not the file this vnode was made for: its qid.path has been reused. */
        p9_unhash(np);
        return;
    }
    if (vnode_vtype(vp) == VREG && attr->size != np->n_size) {
        np->n_size = attr->size;
        P9_UNLOCK(pm);
        (void) ubc_setsize(vp, (off_t) attr->size);
        P9_LOCK(pm);
    }
}

int
p9_attr_refresh(struct p9node *np, int force)
{
    struct p9mount *pm = np->n_pm;
    struct p9_attr attr;
    int error;

    if (!force && np->n_attrtime && p9_now() - np->n_attrtime < P9_ATTRTIMEO)
        return 0;
    error = p9_getattr(pm, &np->n_fid, &attr);
    if (error && error != ENXIO && np->n_wfid != P9_NOFID)
        error = p9_getattr(pm, &np->n_wfid, &attr);
    if (error && error != ENXIO && np->n_rfid != P9_NOFID)
        error = p9_getattr(pm, &np->n_rfid, &attr);
    if (error) {
        if ((np->n_flag & NP_UNHASHED) && np->n_attrtime && error != ENXIO)
            return 0;
        return error;
    }
    p9_attr_load(np, &attr);
    return 0;
}

/* The vnode for the file fid has been walked to, with an iocount. The fid is the node's
 * afterwards, or clunked. As p9fs_bsd_node.c's p9_nget; the vnode is made with the lock given up,
 * so the node goes into the hash only once it has one, and others wait on pm_hashlock meanwhile. */
int
p9_nget(struct p9mount *pm, u_int32_t fid, struct p9_qid *qid, struct p9_attr *attr, vnode_t *vpp)
{
    struct vnode_fsparam vfsp;
    struct p9nodehash *head;
    struct p9node *np;
    vnode_t vp;
    struct p9_attr a;
    u_int32_t old, vid;
    int error, have = 0;

    *vpp = NULL;
    if (attr) {
        a = *attr;
        have = 1;
    }
    head = p9_hashhead(pm, qid->path);
loop:
    for (np = head->lh_first; np != NULL; np = np->n_hash.le_next) {
        if (np->n_qpath != qid->path)
            continue;
        if ((np->n_qtype & P9_QTDIR) != (qid->type & P9_QTDIR)) {
            p9_unhash(np);
            goto loop;
        }
        vp = P9TOV(np);
        vid = vnode_vid(vp);
        P9_UNLOCK(pm);
        error = vnode_getwithvid(vp, vid);
        P9_LOCK(pm);
        if (error)
            goto loop;          /* reclaimed meanwhile */
        if (np->n_fid != fid) {
            old = np->n_fid;
            np->n_fid = fid;
            (void) p9_clunk(pm, old);
        }
        if (have)
            p9_attr_load(np, &a);
        *vpp = vp;
        return 0;
    }

    /* No new node for an unmount in progress is checked here, as the BSD frontend checks: 10.4's
     * KPI has no vfs_isunmount, and vflush with FORCECLOSE takes whatever is made meanwhile. */
    if (!have) {
        error = p9_getattr(pm, &fid, &a);
        if (error) {
            (void) p9_clunk(pm, fid);
            return error;
        }
        have = 1;
        goto loop;
    }
    if (pm->pm_hashlock) {
        while (pm->pm_hashlock) {
            pm->pm_hashlock = -1;
            (void) msleep(&pm->pm_hashlock, pm->pm_lock, PINOD, "9pnget", NULL);
        }
        goto loop;
    }
    pm->pm_hashlock = 1;

    MALLOC(np, struct p9node *, sizeof(*np), M_TEMP, M_WAITOK);
    bzero(np, sizeof(*np));
    np->n_pm = pm;
    np->n_qpath = qid->path;
    np->n_qtype = qid->type;
    np->n_fid = fid;
    np->n_rfid = P9_NOFID;
    np->n_wfid = P9_NOFID;
    np->n_dfid = P9_NOFID;
    np->n_size = a.size;
    np->n_openmtime = (a.mtime_sec << 30) ^ a.mtime_nsec;
    p9_attr_load(np, &a);

    bzero(&vfsp, sizeof(vfsp));
    vfsp.vnfs_mp = pm->pm_mp;
    vfsp.vnfs_vtype = p9_vtype(a.mode);
    vfsp.vnfs_str = P9FS_NAME;
    vfsp.vnfs_dvp = NULL;
    vfsp.vnfs_fsnode = np;
    vfsp.vnfs_vops = p9fs_vnodeop_p;
    vfsp.vnfs_markroot = (qid->path == pm->pm_rootqpath && pm->pm_root == NULL);
    vfsp.vnfs_marksystem = 0;
    vfsp.vnfs_rdev = 0;
    vfsp.vnfs_filesize = (off_t) a.size;
    vfsp.vnfs_cnp = NULL;
    vfsp.vnfs_flags = VNFS_NOCACHE | VNFS_CANTCACHE;   /* lookups always ask the server */
    P9_UNLOCK(pm);
    error = vnode_create(VNCREATE_FLAVOR, VCREATESIZE, &vfsp, &vp);
    P9_LOCK(pm);
    if (!error) {
        np->n_vnode = vp;
        LIST_INSERT_HEAD(head, np, n_hash);
    }
    if (pm->pm_hashlock < 0)
        wakeup(&pm->pm_hashlock);
    pm->pm_hashlock = 0;
    if (error) {
        FREE(np, M_TEMP);
        (void) p9_clunk(pm, fid);
        return error;
    }
    *vpp = vp;
    return 0;
}

/* As p9fs_bsd_node.c's p9_openfid. */
int
p9_openfid(struct p9node *np, int fflag, u_int32_t **fidpp)
{
    struct p9mount *pm = np->n_pm;
    u_int32_t *slot, newfid, flags;
    int error, wonly = 0;

    if (vnode_vtype(P9TOV(np)) == VDIR) {
        slot = &np->n_dfid;
        flags = P9_RDONLY | P9_DIRECTORY;
    } else if (fflag & FWRITE) {
        slot = &np->n_wfid;
        flags = P9_RDWR;
    } else {
        if (np->n_wfid != P9_NOFID && !(np->n_flag & NP_WONLY)) {
            *fidpp = &np->n_wfid;
            return 0;
        }
        slot = &np->n_rfid;
        flags = P9_RDONLY;
    }
    if (*slot != P9_NOFID) {
        *fidpp = slot;
        return 0;
    }

    for (;;) {
        newfid = p9_fid_alloc(pm);
        if (newfid == P9_NOFID)
            return ENFILE;
        error = p9_walk(pm, &np->n_fid, newfid, NULL, 0, NULL);
        if (error)
            return error;
        error = p9_lopen(pm, &newfid, flags);
        if (!error)
            break;
        (void) p9_clunk(pm, newfid);
        if (error != EACCES || flags != P9_RDWR)
            return error;
        flags = P9_WRONLY;
        wonly = 1;
    }
    *slot = newfid;
    if (slot == &np->n_wfid) {
        if (wonly)
            np->n_flag |= NP_WONLY;
        else
            np->n_flag &= ~NP_WONLY;
    }
    *fidpp = slot;
    return 0;
}

void
p9_closefids(struct p9node *np)
{
    struct p9mount *pm = np->n_pm;
    u_int32_t fid;

    fid = np->n_rfid;
    np->n_rfid = P9_NOFID;
    (void) p9_clunk(pm, fid);
    fid = np->n_wfid;
    np->n_wfid = P9_NOFID;
    np->n_flag &= ~NP_WONLY;
    (void) p9_clunk(pm, fid);
    fid = np->n_dfid;
    np->n_dfid = P9_NOFID;
    (void) p9_clunk(pm, fid);
}
