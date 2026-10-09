/*
 * p9fs nodes: one vnode per file, the file being its qid.path as NFS's is its file handle.
 *
 * QEMU's local backend keeps a fid as a path, not as an open inode, so a fid goes stale when the
 * host renames or removes what it walked to, and each lookup that arrives at a node therefore
 * hands it the fid it walked with, in place of the old one. Fids opened for I/O (n_rfid, n_wfid,
 * n_dfid) are opened on the host and stay good; they are opened when first needed and kept
 * until the vnode goes inactive, as NFS keeps its state until then, because the kernel's open
 * and close do not pair up with what uses them (exec, mmap, the pager).
 *
 * The structure of p9_nget -- look in the hash, else make the node under a sleep flag and look
 * again -- is nfs_nget's (xnu bsd/nfs/nfs_node.c); inactive and reclaim follow NFS's and
 * smbfs's of the same releases.
 */

#include "p9fs.h"

#include <sys/proc.h>
#include <sys/kernel.h>
#include <sys/malloc.h>
#include <sys/fcntl.h>
#include <sys/stat.h>
#include <sys/errno.h>
#include <sys/namei.h>
#include <sys/ubc.h>

/* The hash's size: a quarter of the kernel's vnodes, as the bucket for a qid.path is found by its
 * low bits. */
int
p9_nodeinit(struct p9mount *pm)
{
    pm->pm_hash = (struct p9nodehash *) hashinit(desiredvnodes / 4, M_TEMP, &pm->pm_hashmask);
    return pm->pm_hash ? 0 : ENOMEM;
}

void
p9_nodefini(struct p9mount *pm)
{
    if (pm->pm_hash)
        FREE(pm->pm_hash, M_TEMP);
    pm->pm_hash = NULL;
}

/* What stat() calls the inode number and readdir() the file number: the two must agree, for
 * getcwd() matches one against the other. A 64-bit qid.path folded to 32 bits, never 0, which
 * readdir() would skip. */
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

void
p9_unhash(struct p9node *np)
{
    if (!(np->n_flag & NP_UNHASHED)) {
        LIST_REMOVE(np, n_hash);
        np->n_flag |= NP_UNHASHED;
    }
}

/* Takes in an Rgetattr. A regular file's new size goes to the UBC, which drops the pages
 * beyond it. Never with the channel held: the UBC can page out to us. */
void
p9_attr_load(struct p9node *np, struct p9_attr *attr)
{
    struct vnode *vp = P9TOV(np);
    struct timeval tv;

    np->n_attr = *attr;
    microtime(&tv);
    np->n_attrtime = tv.tv_sec ? tv.tv_sec : 1;
    if (vp->v_type != VNON && p9_vtype(attr->mode) != vp->v_type) {
        /* Not the file this vnode was made for: its qid.path has been reused. */
        p9_unhash(np);
        return;
    }
    if (vp->v_type == VREG && attr->size != np->n_size) {
        np->n_size = attr->size;
        if (UBCINFOEXISTS(vp))
            (void) ubc_setsize(vp, (off_t) attr->size);
    }
}

/* Fetches the attributes unless they are fresh. A file removed while open answers no Tgetattr
 * on its walked fid; an opened one may still, and otherwise its last attributes stand. */
int
p9_attr_refresh(struct p9node *np, int force)
{
    struct p9mount *pm = np->n_pm;
    struct p9_attr attr;
    struct timeval tv;
    int error;

    microtime(&tv);
    if (!force && np->n_attrtime && tv.tv_sec - np->n_attrtime < P9_ATTRTIMEO)
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

/* The vnode for the file fid has been walked to, referenced. The fid is the node's afterwards,
 * or clunked: the caller has no more to do with it. attr, if there is one, is the file's
 * Rgetattr; without one it is fetched for a new node. */
int
p9_nget(struct p9mount *pm, u_int32_t fid, struct p9_qid *qid, struct p9_attr *attr,
        struct vnode **vpp)
{
    struct proc *p = current_proc();
    struct p9nodehash *head;
    struct p9node *np;
    struct vnode *vp;
    struct p9_attr a;
    u_int32_t old;
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
        if (vget(vp, LK_EXCLUSIVE, p))
            goto loop;
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

    if (pm->pm_mp->mnt_kern_flag & MNTK_UNMOUNT) {
        (void) p9_clunk(pm, fid);
        return EPERM;
    }
    if (!have) {
        error = p9_getattr(pm, &fid, &a);
        if (error) {
            (void) p9_clunk(pm, fid);
            return error;
        }
        have = 1;
        goto loop;      /* that slept */
    }
    if (pm->pm_hashlock) {
        while (pm->pm_hashlock) {
            pm->pm_hashlock = -1;
            (void) tsleep((caddr_t) &pm->pm_hashlock, PINOD, "9pnget", 0);
        }
        goto loop;
    }
    pm->pm_hashlock = 1;

    /* The node before the vnode, so that a vnode never points at memory not yet there. */
    MALLOC(np, struct p9node *, sizeof(*np), M_TEMP, M_WAITOK);
    bzero(np, sizeof(*np));
    error = getnewvnode(VT_OTHER, pm->pm_mp, p9fs_vnodeop_p, &vp);
    if (error) {
        FREE(np, M_TEMP);
        if (pm->pm_hashlock < 0)
            wakeup((caddr_t) &pm->pm_hashlock);
        pm->pm_hashlock = 0;
        (void) p9_clunk(pm, fid);
        return error;
    }
    np->n_vnode = vp;
    np->n_pm = pm;
    np->n_qpath = qid->path;
    np->n_qtype = qid->type;
    np->n_fid = fid;
    np->n_rfid = P9_NOFID;
    np->n_wfid = P9_NOFID;
    np->n_dfid = P9_NOFID;
    vp->v_data = np;
    vp->v_type = p9_vtype(a.mode);
    LIST_INSERT_HEAD(head, np, n_hash);

    if (pm->pm_hashlock < 0)
        wakeup((caddr_t) &pm->pm_hashlock);
    pm->pm_hashlock = 0;

    p9_attr_load(np, &a);
    np->n_openmtime = (a.mtime_sec << 30) ^ a.mtime_nsec;
    /* After the attributes: ubc_info_init asks VOP_GETATTR for the size. */
    if (vp->v_type == VREG)
        (void) ubc_info_init(vp);
    *vpp = vp;
    return 0;
}

/* A fid opened on the file for fflag (FREAD, FWRITE), opened now if there is none. Returns the
 * node field that holds it, for p9fs_proto.c to read once it has the channel. A directory's
 * is opened for Treaddir. Write access is tried read-write, then write-only. */
int
p9_openfid(struct p9node *np, int fflag, u_int32_t **fidpp)
{
    struct p9mount *pm = np->n_pm;
    u_int32_t *slot, newfid, flags;
    int error, wonly = 0;

    if (P9TOV(np)->v_type == VDIR) {
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

    /* That slept: another thread may have opened one meanwhile. */
    if (*slot != P9_NOFID)
        (void) p9_clunk(pm, newfid);
    else {
        *slot = newfid;
        if (slot == &np->n_wfid) {
            if (wonly)
                np->n_flag |= NP_WONLY;
            else
                np->n_flag &= ~NP_WONLY;
        }
    }
    *fidpp = slot;
    return 0;
}

/* Gives back the opened fids. Each field is cleared before its Tclunk, which sleeps. */
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

/* The last reference has gone: close what was opened, and let a removed file's vnode go now
 * rather than when it is next recycled. */
int
p9fs_inactive(void *v)
{
    struct vop_inactive_args *ap = (struct vop_inactive_args *) v;
    struct vnode *vp = ap->a_vp;
    struct p9node *np = VTOP9(vp);
    int unhashed = 0;

    if (np) {
        p9_closefids(np);
        unhashed = np->n_flag & NP_UNHASHED;
    }
    VOP_UNLOCK(vp, 0, ap->a_p);
    if (unhashed)
        (void) vrecycle(vp, (struct slock *) 0, ap->a_p);
    return 0;
}

int
p9fs_reclaim(void *v)
{
    struct vop_reclaim_args *ap = (struct vop_reclaim_args *) v;
    struct vnode *vp = ap->a_vp;
    struct p9node *np = VTOP9(vp);
    struct p9mount *pm;
    u_int32_t fid;

    if (np == NULL)
        return 0;
    pm = np->n_pm;
    p9_unhash(np);
    cache_purge(vp);
    if (pm->pm_root == np)
        pm->pm_root = NULL;
    p9_closefids(np);
    fid = np->n_fid;
    np->n_fid = P9_NOFID;
    (void) p9_clunk(pm, fid);
    vp->v_data = NULL;
    FREE(np, M_TEMP);
    return 0;
}
