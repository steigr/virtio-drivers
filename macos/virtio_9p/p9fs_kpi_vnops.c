/*
 * p9fs vnode operations for the VFS KPI of 10.4 on. Each does what p9fs_bsd_vnops.c's of the
 * same name does -- that file says why -- in the KPI's terms:
 *
 *   - no vnode is locked or released here: an operation gets its vnodes with an iocount, and one
 *     it hands back carries one;
 *   - permission is the kernel's (vnode_authorize, from the owner and mode getattr reports), so
 *     there is no access operation and lookup does not check search permission;
 *   - the mount lock is held from the start of an operation to its end, and given up around
 *     uiomove and the UBC (p9fs.h), so data still moves through kernel buffers.
 */

#include "p9fs.h"
#include "p9fs_kpi.h"

#include <sys/fcntl.h>
#include <sys/stat.h>
#include <sys/dirent.h>
#include <sys/uio.h>
#include <sys/unistd.h>
#include <sys/errno.h>
#include <sys/malloc.h>
#include <sys/kauth.h>
#include <sys/vnode_if.h>
#include <mach/memory_object_types.h>

/* A struct dirent's length for a name of n bytes, as p9fs_bsd_vnops.c's. */
#define P9_DIRENTSIZE(n)    ((8 + (n) + 1 + 3) & ~3)

typedef int (*p9_vop_t)(void *);

/* ------------------------------------------------------------------------------------------ */
/* helpers                                                                                     */
/* ------------------------------------------------------------------------------------------ */

static void
p9_owner(struct p9node *np, uid_t *uid, gid_t *gid)
{
    struct p9fs_args *args = &np->n_pm->pm_args;

    *uid = (args->flags & P9FS_MNT_UID) ? args->uid : np->n_attr.uid;
    *gid = (args->flags & P9FS_MNT_GID) ? args->gid : np->n_attr.gid;
}

static u_int64_t
p9_mtimekey(struct p9_attr *a)
{
    return (a->mtime_sec << 30) ^ a->mtime_nsec;
}

static gid_t
p9_ctxgid(vfs_context_t ctx)
{
    return kauth_cred_getgid(vfs_context_ucred(ctx));
}

/* Pages dirtied through a mapping, written back before read() or write() goes to the server
 * around them; with the lock given up, for they come back through pageout. */
static void
p9_pushmapped(struct p9mount *pm, vnode_t vp)
{
    if (!ubc_pages_resident(vp))
        return;
    P9_UNLOCK(pm);
    P9_UBC_SYNC(vp, (off_t) 0, ubc_getsize(vp), UBC_PUSHDIRTY | UBC_SYNC);
    P9_LOCK(pm);
}

/* Drops the pages of [start, end), with the lock given up. */
static void
p9_dropmapped(struct p9mount *pm, vnode_t vp, off_t start, off_t end)
{
    if (!ubc_pages_resident(vp) || end <= start)
        return;
    P9_UNLOCK(pm);
    P9_UBC_SYNC(vp, start, end, UBC_INVALIDATE);
    P9_LOCK(pm);
}

/* The vnode for a name just made in dnp's directory: walked to, so that the node has its own
 * fid. */
static int
p9_newname(struct p9node *dnp, struct componentname *cnp, struct p9_qid *qid, vnode_t *vpp)
{
    struct p9mount *pm = dnp->n_pm;
    struct p9_qid wqid;
    u_int32_t fid;
    int error;

    fid = p9_fid_alloc(pm);
    if (fid == P9_NOFID)
        return ENFILE;
    error = p9_walk(pm, &dnp->n_fid, fid, cnp->cn_nameptr, cnp->cn_namelen, &wqid);
    if (error)
        return error;
    return p9_nget(pm, fid, qid, NULL, vpp);
}

/* ------------------------------------------------------------------------------------------ */
/* names                                                                                       */
/* ------------------------------------------------------------------------------------------ */

static int
p9fs_lookup(struct vnop_lookup_args *ap)
{
    vnode_t dvp = ap->a_dvp;
    struct componentname *cnp = ap->a_cnp;
    struct p9node *dnp = VTOP9(dvp);
    struct p9mount *pm = dnp->n_pm;
    int nameiop = cnp->cn_nameiop;
    int islast = cnp->cn_flags & ISLASTCN;
    int dotdot = cnp->cn_flags & ISDOTDOT;
    struct p9_qid qid;
    u_int32_t newfid;
    int error;

    *ap->a_vpp = NULL;
    if (vnode_vtype(dvp) != VDIR)
        return ENOTDIR;
    if (islast && vfs_isrdonly(vnode_mount(dvp)) && (nameiop == DELETE || nameiop == RENAME))
        return EROFS;
    if (cnp->cn_namelen == 1 && cnp->cn_nameptr[0] == '.') {
        if ((error = vnode_get(dvp)))
            return error;
        *ap->a_vpp = dvp;
        return 0;
    }

    P9_LOCK(pm);
    newfid = p9_fid_alloc(pm);
    if (newfid == P9_NOFID) {
        P9_UNLOCK(pm);
        return ENFILE;
    }
    if (dotdot)
        error = p9_walk(pm, &dnp->n_fid, newfid, "..", 2, &qid);
    else
        error = p9_walk(pm, &dnp->n_fid, newfid, cnp->cn_nameptr, cnp->cn_namelen, &qid);
    if (error) {
        P9_UNLOCK(pm);
        /* Not there: a create or the target of a rename may go ahead. */
        if (error == ENOENT && islast && (nameiop == CREATE || nameiop == RENAME))
            return vfs_isrdonly(vnode_mount(dvp)) ? EROFS : EJUSTRETURN;
        return error;
    }
    if (!dotdot && qid.path == dnp->n_qpath) {
        (void) p9_clunk(pm, newfid);
        P9_UNLOCK(pm);
        if ((error = vnode_get(dvp)))
            return error;
        *ap->a_vpp = dvp;
        return 0;
    }
    error = p9_nget(pm, newfid, &qid, NULL, ap->a_vpp);
    P9_UNLOCK(pm);
    return error;
}

static int
p9fs_create(struct vnop_create_args *ap)
{
    vnode_t dvp = ap->a_dvp;
    struct componentname *cnp = ap->a_cnp;
    struct vnode_attr *vap = ap->a_vap;
    struct p9node *dnp = VTOP9(dvp);
    struct p9mount *pm = dnp->n_pm;
    struct p9node *np;
    vnode_t vp;
    struct p9_qid qid;
    u_int32_t ofid, mode;
    int error;

    *ap->a_vpp = NULL;
    if (VATTR_IS_ACTIVE(vap, va_type) && vap->va_type != VREG)
        return ENOTSUP;
    mode = VATTR_IS_ACTIVE(vap, va_mode) ? (vap->va_mode & ALLPERMS) : 0644;

    P9_LOCK(pm);
    /* As p9fs_bsd_vnops.c's: Tlcreate turns a clone of the directory's fid into the new file,
     * opened; a second walk gives the node its walked fid. */
    ofid = p9_fid_alloc(pm);
    if (ofid == P9_NOFID) {
        error = ENFILE;
        goto out;
    }
    if ((error = p9_walk(pm, &dnp->n_fid, ofid, NULL, 0, NULL)))
        goto out;
    error = p9_lcreate(pm, &ofid, cnp->cn_nameptr, cnp->cn_namelen, P9_RDWR | P9_CREAT | P9_EXCL,
                       mode, p9_ctxgid(ap->a_context), &qid);
    if (error) {
        (void) p9_clunk(pm, ofid);
        goto out;
    }
    dnp->n_attrtime = 0;
    if ((error = p9_newname(dnp, cnp, &qid, &vp))) {
        (void) p9_clunk(pm, ofid);
        goto out;
    }
    np = VTOP9(vp);
    if (np->n_wfid == P9_NOFID) {
        np->n_wfid = ofid;
        np->n_flag &= ~NP_WONLY;
    } else
        (void) p9_clunk(pm, ofid);
    *ap->a_vpp = vp;
    /* The mode went with Tlcreate; the owner is the server's to give. */
    VATTR_SET_SUPPORTED(vap, va_type);
    VATTR_SET_SUPPORTED(vap, va_mode);
    VATTR_SET_SUPPORTED(vap, va_uid);
    VATTR_SET_SUPPORTED(vap, va_gid);
out:
    P9_UNLOCK(pm);
    return error;
}

static int
p9fs_mkdir(struct vnop_mkdir_args *ap)
{
    vnode_t dvp = ap->a_dvp;
    struct componentname *cnp = ap->a_cnp;
    struct vnode_attr *vap = ap->a_vap;
    struct p9node *dnp = VTOP9(dvp);
    struct p9mount *pm = dnp->n_pm;
    struct p9_qid qid;
    u_int32_t mode;
    int error;

    *ap->a_vpp = NULL;
    mode = VATTR_IS_ACTIVE(vap, va_mode) ? (vap->va_mode & ALLPERMS) : 0755;
    P9_LOCK(pm);
    error = p9_mkdir(pm, &dnp->n_fid, cnp->cn_nameptr, cnp->cn_namelen, mode, p9_ctxgid(ap->a_context),
                     &qid);
    if (!error) {
        dnp->n_attrtime = 0;
        error = p9_newname(dnp, cnp, &qid, ap->a_vpp);
    }
    P9_UNLOCK(pm);
    if (!error) {
        VATTR_SET_SUPPORTED(vap, va_mode);
        VATTR_SET_SUPPORTED(vap, va_uid);
        VATTR_SET_SUPPORTED(vap, va_gid);
    }
    return error;
}

static int
p9fs_symlink(struct vnop_symlink_args *ap)
{
    vnode_t dvp = ap->a_dvp;
    struct componentname *cnp = ap->a_cnp;
    struct p9node *dnp = VTOP9(dvp);
    struct p9mount *pm = dnp->n_pm;
    struct p9_qid qid;
    int error;

    *ap->a_vpp = NULL;
    P9_LOCK(pm);
    error = p9_symlink(pm, &dnp->n_fid, cnp->cn_nameptr, cnp->cn_namelen, ap->a_target,
                       p9_ctxgid(ap->a_context), &qid);
    if (!error) {
        dnp->n_attrtime = 0;
        error = p9_newname(dnp, cnp, &qid, ap->a_vpp);
    }
    P9_UNLOCK(pm);
    if (!error) {
        VATTR_SET_SUPPORTED(ap->a_vap, va_mode);
        VATTR_SET_SUPPORTED(ap->a_vap, va_uid);
        VATTR_SET_SUPPORTED(ap->a_vap, va_gid);
    }
    return error;
}

static int
p9fs_link(struct vnop_link_args *ap)
{
    vnode_t vp = ap->a_vp;
    vnode_t tdvp = ap->a_tdvp;
    struct componentname *cnp = ap->a_cnp;
    struct p9node *tdnp = VTOP9(tdvp);
    struct p9mount *pm = tdnp->n_pm;
    int error;

    if (vnode_mount(vp) != vnode_mount(tdvp))
        return EXDEV;
    P9_LOCK(pm);
    error = p9_link(pm, &tdnp->n_fid, &VTOP9(vp)->n_fid, cnp->cn_nameptr, cnp->cn_namelen);
    if (!error) {
        tdnp->n_attrtime = 0;
        VTOP9(vp)->n_attrtime = 0;
    }
    P9_UNLOCK(pm);
    return error;
}

static int
p9fs_remove(struct vnop_remove_args *ap)
{
    vnode_t dvp = ap->a_dvp;
    vnode_t vp = ap->a_vp;
    struct componentname *cnp = ap->a_cnp;
    struct p9node *dnp = VTOP9(dvp);
    struct p9node *np = VTOP9(vp);
    struct p9mount *pm = dnp->n_pm;
    int error;

    if (vnode_vtype(vp) == VDIR)
        return EPERM;
    P9_LOCK(pm);
    error = p9_unlinkat(pm, &dnp->n_fid, cnp->cn_nameptr, cnp->cn_namelen, 0);
    if (!error) {
        /* The vnode lives on for whoever has it open, but no lookup will find it again, and it
         * is recycled when it goes inactive. */
        p9_unhash(np);
        dnp->n_attrtime = 0;
        np->n_attrtime = 0;
    }
    P9_UNLOCK(pm);
    return error;
}

static int
p9fs_rmdir(struct vnop_rmdir_args *ap)
{
    vnode_t dvp = ap->a_dvp;
    vnode_t vp = ap->a_vp;
    struct componentname *cnp = ap->a_cnp;
    struct p9node *dnp = VTOP9(dvp);
    struct p9mount *pm = dnp->n_pm;
    int error;

    if (dvp == vp)
        return EINVAL;
    P9_LOCK(pm);
    error = p9_unlinkat(pm, &dnp->n_fid, cnp->cn_nameptr, cnp->cn_namelen, P9_AT_REMOVEDIR);
    if (!error) {
        p9_unhash(VTOP9(vp));
        dnp->n_attrtime = 0;
    }
    P9_UNLOCK(pm);
    return error;
}

/* As p9fs_bsd_vnops.c's, without releasing anything: the kernel does. */
static int
p9fs_rename(struct vnop_rename_args *ap)
{
    vnode_t fdvp = ap->a_fdvp;
    vnode_t fvp = ap->a_fvp;
    vnode_t tdvp = ap->a_tdvp;
    vnode_t tvp = ap->a_tvp;
    struct componentname *fcnp = ap->a_fcnp;
    struct componentname *tcnp = ap->a_tcnp;
    struct p9node *fdnp = VTOP9(fdvp);
    struct p9node *tdnp = VTOP9(tdvp);
    struct p9mount *pm = fdnp->n_pm;
    int error;

    if (vnode_mount(fvp) != vnode_mount(tdvp) || (tvp && vnode_mount(fvp) != vnode_mount(tvp)))
        return EXDEV;
    if (tvp == fvp)
        return 0;
    P9_LOCK(pm);
    error = p9_renameat(pm, &fdnp->n_fid, fcnp->cn_nameptr, fcnp->cn_namelen,
                        &tdnp->n_fid, tcnp->cn_nameptr, tcnp->cn_namelen);
    if (!error) {
        if (tvp)
            p9_unhash(VTOP9(tvp));
        fdnp->n_attrtime = 0;
        tdnp->n_attrtime = 0;
        VTOP9(fvp)->n_attrtime = 0;
    }
    P9_UNLOCK(pm);
    return error;
}

/* ------------------------------------------------------------------------------------------ */
/* attributes                                                                                  */
/* ------------------------------------------------------------------------------------------ */

static int
p9fs_getattr(struct vnop_getattr_args *ap)
{
    vnode_t vp = ap->a_vp;
    struct vnode_attr *vap = ap->a_vap;
    struct p9node *np = VTOP9(vp);
    struct p9mount *pm = np->n_pm;
    struct p9_attr a;
    struct timespec ts;
    u_int64_t size;
    uid_t uid;
    gid_t gid;
    int error;

    P9_LOCK(pm);
    if ((error = p9_attr_refresh(np, 0))) {
        P9_UNLOCK(pm);
        return error;
    }
    a = np->n_attr;
    size = (vnode_vtype(vp) == VREG) ? np->n_size : a.size;
    p9_owner(np, &uid, &gid);
    P9_UNLOCK(pm);

    VATTR_RETURN(vap, va_type, vnode_vtype(vp));
    VATTR_RETURN(vap, va_mode, a.mode & ALLPERMS);
    VATTR_RETURN(vap, va_nlink, a.nlink);
    VATTR_RETURN(vap, va_uid, uid);
    VATTR_RETURN(vap, va_gid, gid);
    VATTR_RETURN(vap, va_fsid, vfs_statfs(vnode_mount(vp))->f_fsid.val[0]);
    VATTR_RETURN(vap, va_fileid, p9_fileid(np->n_qpath));
    VATTR_RETURN(vap, va_data_size, size);
    VATTR_RETURN(vap, va_total_size, size);
    VATTR_RETURN(vap, va_data_alloc, a.blocks * 512);
    VATTR_RETURN(vap, va_total_alloc, a.blocks * 512);
    VATTR_RETURN(vap, va_iosize, pm->pm_iosize & ~PAGE_MASK);
    VATTR_RETURN(vap, va_rdev, (dev_t) a.rdev);
    VATTR_RETURN(vap, va_flags, 0);
    VATTR_RETURN(vap, va_gen, 0);
    VATTR_RETURN(vap, va_filerev, 0);
    ts.tv_sec = (time_t) a.atime_sec;
    ts.tv_nsec = (long) a.atime_nsec;
    VATTR_RETURN(vap, va_access_time, ts);
    ts.tv_sec = (time_t) a.mtime_sec;
    ts.tv_nsec = (long) a.mtime_nsec;
    VATTR_RETURN(vap, va_modify_time, ts);
    ts.tv_sec = (time_t) a.ctime_sec;
    ts.tv_nsec = (long) a.ctime_nsec;
    VATTR_RETURN(vap, va_change_time, ts);
    return 0;
}

/* The kernel has decided whether the caller may; this only says how the server is asked. */
static int
p9fs_setattr(struct vnop_setattr_args *ap)
{
    vnode_t vp = ap->a_vp;
    struct vnode_attr *vap = ap->a_vap;
    struct p9node *np = VTOP9(vp);
    struct p9mount *pm = np->n_pm;
    struct p9_setattr sa;
    int error;

    bzero(&sa, sizeof(sa));
    /* No file flags here, but asking for none is no failure (cp -p asks). */
    if (VATTR_IS_ACTIVE(vap, va_flags)) {
        if (vap->va_flags != 0)
            return ENOTSUP;
        VATTR_SET_SUPPORTED(vap, va_flags);
    }
    if (VATTR_IS_ACTIVE(vap, va_uid)) {
        sa.valid |= P9_SETATTR_UID;
        sa.uid = vap->va_uid;
    }
    if (VATTR_IS_ACTIVE(vap, va_gid)) {
        sa.valid |= P9_SETATTR_GID;
        sa.gid = vap->va_gid;
    }
    if (VATTR_IS_ACTIVE(vap, va_data_size)) {
        if (vnode_vtype(vp) == VDIR)
            return EISDIR;
        if (vnode_vtype(vp) != VREG)
            return EINVAL;
        sa.valid |= P9_SETATTR_SIZE;
        sa.size = vap->va_data_size;
    }
    if (VATTR_IS_ACTIVE(vap, va_mode)) {
        sa.valid |= P9_SETATTR_MODE;
        sa.mode = vap->va_mode & ALLPERMS;
    }
    if (VATTR_IS_ACTIVE(vap, va_access_time) || VATTR_IS_ACTIVE(vap, va_modify_time)) {
        if (vap->va_vaflags & VA_UTIMES_NULL)
            sa.valid |= P9_SETATTR_ATIME | P9_SETATTR_MTIME;    /* the server's now */
        else {
            if (VATTR_IS_ACTIVE(vap, va_access_time)) {
                sa.valid |= P9_SETATTR_ATIME | P9_SETATTR_ATIME_SET;
                sa.atime_sec = vap->va_access_time.tv_sec;
                sa.atime_nsec = vap->va_access_time.tv_nsec;
            }
            if (VATTR_IS_ACTIVE(vap, va_modify_time)) {
                sa.valid |= P9_SETATTR_MTIME | P9_SETATTR_MTIME_SET;
                sa.mtime_sec = vap->va_modify_time.tv_sec;
                sa.mtime_nsec = vap->va_modify_time.tv_nsec;
            }
        }
    }
    if (sa.valid == 0)
        return 0;
    if (vfs_isrdonly(vnode_mount(vp)))
        return EROFS;

    P9_LOCK(pm);
    if (sa.valid & P9_SETATTR_SIZE)
        p9_pushmapped(pm, vp);
    error = p9_setattr(pm, &np->n_fid, &sa);
    if (error == ENOENT && np->n_wfid != P9_NOFID)
        error = p9_setattr(pm, &np->n_wfid, &sa);
    if (!error) {
        np->n_attrtime = 0;
        if (sa.valid & P9_SETATTR_SIZE) {
            np->n_flag |= NP_MODIFIED;
            np->n_size = sa.size;
        }
    }
    P9_UNLOCK(pm);
    if (error)
        return error;
    if (sa.valid & P9_SETATTR_SIZE)
        (void) ubc_setsize(vp, (off_t) sa.size);
    VATTR_SET_SUPPORTED(vap, va_uid);
    VATTR_SET_SUPPORTED(vap, va_gid);
    VATTR_SET_SUPPORTED(vap, va_data_size);
    VATTR_SET_SUPPORTED(vap, va_mode);
    VATTR_SET_SUPPORTED(vap, va_access_time);
    VATTR_SET_SUPPORTED(vap, va_modify_time);
    return 0;
}

/* ------------------------------------------------------------------------------------------ */
/* open, close, read, write                                                                    */
/* ------------------------------------------------------------------------------------------ */

/* Close-to-open consistency, as p9fs_bsd_vnops.c's. */
static int
p9fs_open(struct vnop_open_args *ap)
{
    vnode_t vp = ap->a_vp;
    struct p9node *np = VTOP9(vp);
    struct p9mount *pm = np->n_pm;
    u_int32_t *fidp;
    u_int64_t key;
    int error;

    switch (vnode_vtype(vp)) {
    case VDIR:
        P9_LOCK(pm);
        np->n_diroff = 0;
        np->n_dircookie = 0;
        P9_UNLOCK(pm);
        return 0;
    case VREG:
        break;
    default:
        return ENOTSUP;         /* devices, FIFOs and sockets on the host are not ours to open */
    }
    P9_LOCK(pm);
    if ((error = p9_attr_refresh(np, 1)))
        goto out;
    key = p9_mtimekey(&np->n_attr);
    if (np->n_flag & NP_MODIFIED)
        np->n_flag &= ~NP_MODIFIED;
    else if (key != np->n_openmtime && np->n_size)
        p9_dropmapped(pm, vp, (off_t) 0, (off_t) np->n_size);
    np->n_openmtime = key;
    error = p9_openfid(np, (ap->a_mode & FWRITE) ? FWRITE : FREAD, &fidp);
out:
    P9_UNLOCK(pm);
    return error;
}

static int
p9fs_close(struct vnop_close_args *ap)
{
    return 0;
}

static int
p9fs_read(struct vnop_read_args *ap)
{
    vnode_t vp = ap->a_vp;
    uio_t uio = ap->a_uio;
    struct p9node *np = VTOP9(vp);
    struct p9mount *pm = np->n_pm;
    u_int32_t *fidp, n, got;
    caddr_t buf;
    int error = 0, retried = 0;

    if (vnode_vtype(vp) == VDIR)
        return EISDIR;
    if (vnode_vtype(vp) != VREG)
        return ENOTSUP;
    if (uio_offset(uio) < 0)
        return EINVAL;
    if (uio_resid(uio) == 0)
        return 0;

    MALLOC(buf, caddr_t, pm->pm_iosize, M_TEMP, M_WAITOK);
    P9_LOCK(pm);
    p9_pushmapped(pm, vp);
    while (uio_resid(uio) > 0) {
        if ((error = p9_openfid(np, FREAD, &fidp)))
            break;
        n = (uio_resid(uio) < (user_ssize_t) pm->pm_iosize) ? (u_int32_t) uio_resid(uio) : pm->pm_iosize;
        error = p9_read(pm, fidp, (u_int64_t) uio_offset(uio), n, buf, &got);
        if (error == EBADF && !retried++)
            continue;           /* closed under us: open again, once */
        if (error || got == 0)
            break;
        P9_UNLOCK(pm);
        error = uiomove(buf, (int) got, uio);
        P9_LOCK(pm);
        if (error)
            break;
    }
    P9_UNLOCK(pm);
    FREE(buf, M_TEMP);
    return error;
}

static int
p9fs_write(struct vnop_write_args *ap)
{
    vnode_t vp = ap->a_vp;
    uio_t uio = ap->a_uio;
    struct p9node *np = VTOP9(vp);
    struct p9mount *pm = np->n_pm;
    u_int32_t *fidp, n, done, sent;
    off_t start, off;
    u_int64_t size;
    caddr_t buf;
    int error = 0, retried = 0;

    if (vnode_vtype(vp) != VREG)
        return EISDIR;
    if (uio_offset(uio) < 0)
        return EINVAL;
    if (uio_resid(uio) == 0)
        return 0;

    MALLOC(buf, caddr_t, pm->pm_iosize, M_TEMP, M_WAITOK);
    P9_LOCK(pm);
    start = uio_offset(uio);
    size = np->n_size;
    if (ap->a_ioflag & IO_APPEND) {
        if ((error = p9_attr_refresh(np, 1)))
            goto out;
        uio_setoffset(uio, (off_t) np->n_size);
    }
    p9_pushmapped(pm, vp);
    start = uio_offset(uio);
    while (uio_resid(uio) > 0) {
        off = uio_offset(uio);
        n = (uio_resid(uio) < (user_ssize_t) pm->pm_iosize) ? (u_int32_t) uio_resid(uio) : pm->pm_iosize;
        P9_UNLOCK(pm);
        error = uiomove(buf, (int) n, uio);
        P9_LOCK(pm);
        if (error)
            break;
        for (sent = 0; sent < n; sent += done) {
            if ((error = p9_openfid(np, FWRITE, &fidp)))
                break;
            error = p9_write(pm, fidp, (u_int64_t) (off + sent), n - sent, buf + sent, &done);
            if (error == EBADF && !retried++) {
                done = 0;
                continue;
            }
            if (!error && done == 0)
                error = EIO;
            if (error)
                break;
        }
        if (error)
            break;
        if ((u_int64_t) (off + n) > np->n_size)
            np->n_size = off + n;
    }
    np->n_flag |= NP_MODIFIED;
    np->n_attrtime = 0;
    size = np->n_size;
    if (!error && (ap->a_ioflag & IO_SYNC))
        error = p9_fsync(pm, &np->n_wfid);
out:
    P9_UNLOCK(pm);
    FREE(buf, M_TEMP);
    (void) ubc_setsize(vp, (off_t) size);
    if (ubc_pages_resident(vp) && uio_offset(uio) > start)
        P9_UBC_SYNC(vp, start, uio_offset(uio), UBC_INVALIDATE);
    return error;
}

static int
p9fs_fsync(struct vnop_fsync_args *ap)
{
    vnode_t vp = ap->a_vp;
    struct p9node *np = VTOP9(vp);
    struct p9mount *pm = np->n_pm;
    int error = 0;

    if (ubc_pages_resident(vp))
        P9_UBC_SYNC(vp, (off_t) 0, ubc_getsize(vp), UBC_PUSHDIRTY | UBC_SYNC);
    P9_LOCK(pm);
    if (np->n_wfid != P9_NOFID)
        error = p9_fsync(pm, &np->n_wfid);
    P9_UNLOCK(pm);
    return error;
}

/* ------------------------------------------------------------------------------------------ */
/* directories and links                                                                       */
/* ------------------------------------------------------------------------------------------ */

/* As p9fs_bsd_vnops.c's: the offset is an entry index. Each Rreaddir is taken apart with the
 * lock given up, for the entries go out through uiomove. */
static int
p9fs_readdir(struct vnop_readdir_args *ap)
{
    vnode_t vp = ap->a_vp;
    uio_t uio = ap->a_uio;
    struct p9node *np = VTOP9(vp);
    struct p9mount *pm = np->n_pm;
    struct dirent d;
    u_int32_t *fidp, got, pos;
    u_int64_t cookie, ecookie;
    off_t idx, target;
    u_int64_t qpath;
    u_int16_t nlen;
    u_int8_t *buf, *e, type;
    int error, eof = 0, full = 0, count = 0;

    if (vnode_vtype(vp) != VDIR)
        return ENOTDIR;
    target = uio_offset(uio);
    if (target < 0)
        return EINVAL;

    MALLOC(buf, u_int8_t *, pm->pm_iosize, M_TEMP, M_WAITOK);
    P9_LOCK(pm);
    if (target != 0 && target == np->n_diroff) {
        idx = target;
        cookie = np->n_dircookie;
    } else {
        idx = 0;
        cookie = 0;
    }
    if ((error = p9_openfid(np, FREAD, &fidp)))
        goto out;
    while (!eof && !full && uio_resid(uio) > 0) {
        error = p9_readdir(pm, fidp, cookie, pm->pm_iosize, buf, &got);
        if (error)
            break;
        if (got == 0) {
            eof = 1;
            break;
        }
        P9_UNLOCK(pm);
        /* qid[13] offset[8] type[1] name[s], little-endian */
        for (pos = 0; pos + 24 <= got; pos += 24 + nlen) {
            e = buf + pos;
            qpath = (u_int64_t) e[5] | ((u_int64_t) e[6] << 8) | ((u_int64_t) e[7] << 16) |
                    ((u_int64_t) e[8] << 24) | ((u_int64_t) e[9] << 32) | ((u_int64_t) e[10] << 40) |
                    ((u_int64_t) e[11] << 48) | ((u_int64_t) e[12] << 56);
            ecookie = (u_int64_t) e[13] | ((u_int64_t) e[14] << 8) | ((u_int64_t) e[15] << 16) |
                      ((u_int64_t) e[16] << 24) | ((u_int64_t) e[17] << 32) | ((u_int64_t) e[18] << 40) |
                      ((u_int64_t) e[19] << 48) | ((u_int64_t) e[20] << 56);
            type = e[21];
            nlen = (u_int16_t) (e[22] | (e[23] << 8));
            if (pos + 24 + nlen > got)
                break;
            if (idx >= target && nlen <= MAXNAMLEN) {
                bzero(&d, sizeof(d));
                d.d_fileno = p9_fileid(qpath);
                d.d_type = type;    /* Linux's DT_* are BSD's */
                d.d_namlen = (u_int8_t) nlen;
                bcopy(e + 24, d.d_name, nlen);
                d.d_reclen = P9_DIRENTSIZE(nlen);
                if (d.d_reclen > uio_resid(uio)) {
                    full = 1;
                    break;
                }
                if ((error = uiomove((caddr_t) &d, d.d_reclen, uio)))
                    break;
                count++;
            }
            idx++;
            cookie = ecookie;
        }
        P9_LOCK(pm);
        if (error)
            break;
    }
    np->n_diroff = idx;
    np->n_dircookie = cookie;
out:
    P9_UNLOCK(pm);
    FREE(buf, M_TEMP);
    uio_setoffset(uio, idx);    /* uiomove counted bytes */
    if (ap->a_eofflag)
        *ap->a_eofflag = eof;
    if (ap->a_numdirent)
        *ap->a_numdirent = count;
    return error;
}

static int
p9fs_readlink(struct vnop_readlink_args *ap)
{
    struct p9node *np = VTOP9(ap->a_vp);
    struct p9mount *pm = np->n_pm;
    u_int32_t len;
    char *buf;
    int error;

    if (vnode_vtype(ap->a_vp) != VLNK)
        return EINVAL;
    MALLOC(buf, char *, MAXPATHLEN, M_TEMP, M_WAITOK);
    P9_LOCK(pm);
    error = p9_readlink(pm, &np->n_fid, buf, MAXPATHLEN, &len);
    P9_UNLOCK(pm);
    if (!error)
        error = uiomove(buf, (int) len, ap->a_uio);
    FREE(buf, M_TEMP);
    return error;
}

/* ------------------------------------------------------------------------------------------ */
/* paging                                                                                      */
/* ------------------------------------------------------------------------------------------ */

/* As p9fs_bsd_vnops.c's: the page list is mapped into the kernel and read into directly, which
 * faults on nothing of this filesystem's, so the lock is held throughout. */
static int
p9fs_pagein(struct vnop_pagein_args *ap)
{
    vnode_t vp = ap->a_vp;
    struct p9node *np = VTOP9(vp);
    struct p9mount *pm;
    upl_t pl = ap->a_pl;
    vm_offset_t ploff = ap->a_pl_offset, ioaddr;
    off_t off = ap->a_f_offset;
    size_t size = ap->a_size, xsize, done = 0;
    u_int32_t *fidp, n, got;
    int error = 0, retried = 0;

    if (np == NULL || pl == NULL || size == 0 || off < 0 || (off & PAGE_MASK_64) || (size & PAGE_MASK)) {
        error = EINVAL;
        goto out;
    }
    pm = np->n_pm;
    if (ubc_upl_map(pl, &ioaddr) != KERN_SUCCESS) {
        error = EIO;
        goto out;
    }
    ioaddr += ploff;
    P9_LOCK(pm);
    xsize = size;
    if ((u_int64_t) off >= np->n_size)
        xsize = 0;
    else if ((u_int64_t) off + size > np->n_size)
        xsize = (size_t) (np->n_size - off);
    while (done < xsize) {
        if ((error = p9_openfid(np, FREAD, &fidp)))
            break;
        n = (u_int32_t) (xsize - done);
        if (n > pm->pm_iosize)
            n = pm->pm_iosize;
        error = p9_read(pm, fidp, (u_int64_t) off + done, n, (void *) (ioaddr + done), &got);
        if (error == EBADF && !retried++)
            continue;
        if (error || got == 0)
            break;
        done += got;
    }
    P9_UNLOCK(pm);
    if (!error && done < size)
        bzero((caddr_t) (ioaddr + done), size - done);
    (void) ubc_upl_unmap(pl);
out:
    if (pl && !(ap->a_flags & UPL_NOCOMMIT)) {
        if (error)
            (void) ubc_upl_abort_range(pl, ploff, size, UPL_ABORT_ERROR | UPL_ABORT_FREE_ON_EMPTY);
        else
            (void) ubc_upl_commit_range(pl, ploff, size, UPL_COMMIT_CLEAR_DIRTY | UPL_COMMIT_FREE_ON_EMPTY);
    }
    return error;
}

static int
p9fs_pageout(struct vnop_pageout_args *ap)
{
    vnode_t vp = ap->a_vp;
    struct p9node *np = VTOP9(vp);
    struct p9mount *pm;
    upl_t pl = ap->a_pl;
    vm_offset_t ploff = ap->a_pl_offset, ioaddr;
    off_t off = ap->a_f_offset;
    size_t size = ap->a_size, xsize, sent = 0;
    u_int32_t *fidp, n, done;
    int error = 0, retried = 0;

    if (np == NULL || pl == NULL || size == 0 || off < 0 || (off & PAGE_MASK_64) || (size & PAGE_MASK)) {
        error = EINVAL;
        goto out;
    }
    if (vfs_isrdonly(vnode_mount(vp))) {
        error = EROFS;
        goto out;
    }
    pm = np->n_pm;
    if (ubc_upl_map(pl, &ioaddr) != KERN_SUCCESS) {
        error = EIO;
        goto out;
    }
    ioaddr += ploff;
    P9_LOCK(pm);
    xsize = size;
    if ((u_int64_t) off >= np->n_size)
        xsize = 0;
    else if ((u_int64_t) off + size > np->n_size)
        xsize = (size_t) (np->n_size - off);
    while (sent < xsize) {
        if ((error = p9_openfid(np, FWRITE, &fidp)))
            break;
        n = (u_int32_t) (xsize - sent);
        if (n > pm->pm_iosize)
            n = pm->pm_iosize;
        error = p9_write(pm, fidp, (u_int64_t) off + sent, n, (void *) (ioaddr + sent), &done);
        if (error == EBADF && !retried++)
            continue;
        if (!error && done == 0)
            error = EIO;
        if (error)
            break;
        sent += done;
    }
    if (!error) {
        np->n_flag |= NP_MODIFIED;
        np->n_attrtime = 0;
    }
    P9_UNLOCK(pm);
    (void) ubc_upl_unmap(pl);
out:
    if (pl && !(ap->a_flags & UPL_NOCOMMIT)) {
        if (error)
            (void) ubc_upl_abort_range(pl, ploff, size, UPL_ABORT_FREE_ON_EMPTY);
        else
            (void) ubc_upl_commit_range(pl, ploff, size, UPL_COMMIT_CLEAR_DIRTY | UPL_COMMIT_FREE_ON_EMPTY);
    }
    if (error)
        printf("9p: pageout of %d bytes at %d failed: %d\n", (int) size, (int) off, error);
    return error;
}

static int
p9fs_blktooff(struct vnop_blktooff_args *ap)
{
    *ap->a_offset = (off_t) ap->a_lblkno * PAGE_SIZE_64;
    return 0;
}

static int
p9fs_offtoblk(struct vnop_offtoblk_args *ap)
{
    *ap->a_lblkno = (daddr64_t) (ap->a_offset / PAGE_SIZE_64);
    return 0;
}

/* ------------------------------------------------------------------------------------------ */
/* the node's end                                                                              */
/* ------------------------------------------------------------------------------------------ */

/* The last iocount and usecount have gone: close what was opened, and let a removed file's
 * vnode go now rather than when it is next recycled. */
static int
p9fs_inactive(struct vnop_inactive_args *ap)
{
    vnode_t vp = ap->a_vp;
    struct p9node *np = VTOP9(vp);
    struct p9mount *pm;
    int unhashed;

    if (np == NULL)
        return 0;
    pm = np->n_pm;
    P9_LOCK(pm);
    p9_closefids(np);
    unhashed = np->n_flag & NP_UNHASHED;
    P9_UNLOCK(pm);
    if (unhashed)
        (void) vnode_recycle(vp);
    return 0;
}

static int
p9fs_reclaim(struct vnop_reclaim_args *ap)
{
    vnode_t vp = ap->a_vp;
    struct p9node *np = VTOP9(vp);
    struct p9mount *pm;
    u_int32_t fid;

    if (np == NULL)
        return 0;
    pm = np->n_pm;
    P9_LOCK(pm);
    p9_unhash(np);
    if (pm->pm_root == np)
        pm->pm_root = NULL;
    p9_closefids(np);
    fid = np->n_fid;
    np->n_fid = P9_NOFID;
    (void) p9_clunk(pm, fid);
    vnode_clearfsnode(vp);
    P9_UNLOCK(pm);
    FREE(np, M_TEMP);
    return 0;
}

/* ------------------------------------------------------------------------------------------ */
/* the rest                                                                                    */
/* ------------------------------------------------------------------------------------------ */

static int
p9fs_pathconf(struct vnop_pathconf_args *ap)
{
    switch (ap->a_name) {
    case _PC_LINK_MAX:          *ap->a_retval = 32767;      return 0;
    case _PC_NAME_MAX:          *ap->a_retval = MAXNAMLEN;  return 0;
    case _PC_PATH_MAX:          *ap->a_retval = MAXPATHLEN; return 0;
    case _PC_PIPE_BUF:          *ap->a_retval = PIPE_BUF;   return 0;
    case _PC_CHOWN_RESTRICTED:  *ap->a_retval = 1;          return 0;
    case _PC_NO_TRUNC:          *ap->a_retval = 1;          return 0;
    default:                    return EINVAL;
    }
}

/* As p9fs_bsd_vnops.c's: granted, holding nothing back. */
static int
p9fs_advlock(struct vnop_advlock_args *ap)
{
    if (ap->a_op == F_GETLK)
        ap->a_fl->l_type = F_UNLCK;
    return 0;
}

static int
p9fs_ioctl(struct vnop_ioctl_args *ap)
{
    return ENOTTY;
}

static int
p9fs_select(struct vnop_select_args *ap)
{
    return 1;
}

static int
p9fs_mmap(struct vnop_mmap_args *ap)
{
    return 0;
}

/* ------------------------------------------------------------------------------------------ */
/* the vector                                                                                  */
/* ------------------------------------------------------------------------------------------ */

int (**p9fs_vnodeop_p)(void *);

static struct vnodeopv_entry_desc p9fs_vnodeop_entries[] = {
    { &vnop_default_desc,   (p9_vop_t) vn_default_error },
    { &vnop_lookup_desc,    (p9_vop_t) p9fs_lookup },
    { &vnop_create_desc,    (p9_vop_t) p9fs_create },
    { &vnop_open_desc,      (p9_vop_t) p9fs_open },
    { &vnop_close_desc,     (p9_vop_t) p9fs_close },
    { &vnop_getattr_desc,   (p9_vop_t) p9fs_getattr },
    { &vnop_setattr_desc,   (p9_vop_t) p9fs_setattr },
    { &vnop_read_desc,      (p9_vop_t) p9fs_read },
    { &vnop_write_desc,     (p9_vop_t) p9fs_write },
    { &vnop_ioctl_desc,     (p9_vop_t) p9fs_ioctl },
    { &vnop_select_desc,    (p9_vop_t) p9fs_select },
    { &vnop_mmap_desc,      (p9_vop_t) p9fs_mmap },
    { &vnop_fsync_desc,     (p9_vop_t) p9fs_fsync },
    { &vnop_remove_desc,    (p9_vop_t) p9fs_remove },
    { &vnop_link_desc,      (p9_vop_t) p9fs_link },
    { &vnop_rename_desc,    (p9_vop_t) p9fs_rename },
    { &vnop_mkdir_desc,     (p9_vop_t) p9fs_mkdir },
    { &vnop_rmdir_desc,     (p9_vop_t) p9fs_rmdir },
    { &vnop_symlink_desc,   (p9_vop_t) p9fs_symlink },
    { &vnop_readdir_desc,   (p9_vop_t) p9fs_readdir },
    { &vnop_readlink_desc,  (p9_vop_t) p9fs_readlink },
    { &vnop_inactive_desc,  (p9_vop_t) p9fs_inactive },
    { &vnop_reclaim_desc,   (p9_vop_t) p9fs_reclaim },
    { &vnop_pathconf_desc,  (p9_vop_t) p9fs_pathconf },
    { &vnop_advlock_desc,   (p9_vop_t) p9fs_advlock },
    { &vnop_pagein_desc,    (p9_vop_t) p9fs_pagein },
    { &vnop_pageout_desc,   (p9_vop_t) p9fs_pageout },
    { &vnop_blktooff_desc,  (p9_vop_t) p9fs_blktooff },
    { &vnop_offtoblk_desc,  (p9_vop_t) p9fs_offtoblk },
    { NULL, NULL }
};

struct vnodeopv_desc p9fs_vnodeop_opv_desc = { &p9fs_vnodeop_p, p9fs_vnodeop_entries };
