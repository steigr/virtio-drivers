/*
 * p9fs vnode operations, for the 4.4BSD-Lite VFS of Mac OS X 10.0 to 10.3.
 *
 * What each operation must release and free on the way out -- the directory vnodes, the
 * pathname buffer -- is the contract NFS keeps on these releases (xnu bsd/nfs/nfs_vnops.c,
 * xnu-123.5 to xnu-517), whose lookup also sets SAVENAME for the last component of anything but
 * a LOOKUP, so that the operation that follows has the name and frees it.
 *
 * Reads and writes go to the server, not through a cache, so that what the host changes is seen
 * at once; only mapped files (mmap, exec) have pages, through the UBC and VOP_PAGEIN/PAGEOUT,
 * and read() and write() keep those pages honest around themselves. Data moves through kernel
 * buffers, never straight between the device and user memory: a fault on user memory can be a
 * page-in from this same filesystem, which needs the channel.
 */

#include "p9fs.h"

#include <sys/proc.h>
#include <sys/kernel.h>
#include <sys/malloc.h>
#include <sys/fcntl.h>
#include <sys/stat.h>
#include <sys/dirent.h>
#include <sys/namei.h>
#include <sys/uio.h>
#include <sys/errno.h>
#include <sys/ubc.h>
#include <sys/ucred.h>
#include <sys/resourcevar.h>
#include <sys/signal.h>
#include <mach/memory_object_types.h>

extern int groupmember(gid_t gid, struct ucred *cred);
extern void psignal(struct proc *p, int sig);      /* 10.2's Kernel.framework has no sys/signalvar.h */

/* The kernel's stock operations (bsd/vfs/vfs_support.c), declared here rather than through
 * vfs/vfs_support.h, which needs a sys/unistd.h 10.2's Kernel.framework does not have. */
extern int nop_lease(void *), nop_seek(void *), nop_abortop(void *), nop_rename(void *);
extern int err_mknod(void *), err_strategy(void *), err_copyfile(void *), err_searchfs(void *);
extern int err_cmap(void *);

/* pathconf's names: 10.2's Kernel.framework has no sys/unistd.h. */
#ifndef _PC_LINK_MAX
#define _PC_LINK_MAX            1
#define _PC_NAME_MAX            4
#define _PC_PATH_MAX            5
#define _PC_PIPE_BUF            6
#define _PC_CHOWN_RESTRICTED    7
#define _PC_NO_TRUNC            8
#endif

/*
 * Mapping a page list into the kernel and committing it. The GCC 2.95 build serves 10.0, which
 * has only the kernel_upl_* calls -- 10.1 added the ubc_upl_* ones and kept these -- so it uses
 * them on 10.1 too, as 10.0's NFS does; a NULL page list is what both kernels' commit takes when
 * it has no page states to merge. The GCC 3 build (10.2 on) uses the UBC's own.
 */
#ifdef P9FS_ABI_GCC2
extern vm_map_t kernel_map;
extern kern_return_t kernel_upl_map(vm_map_t map, upl_t upl, vm_offset_t *dst_addr);
extern kern_return_t kernel_upl_unmap(vm_map_t map, upl_t upl);
extern kern_return_t kernel_upl_commit_range(upl_t upl, vm_offset_t offset, vm_size_t size, int flags,
                                             void *pl, unsigned int count);
extern kern_return_t kernel_upl_abort_range(upl_t upl, vm_offset_t offset, vm_size_t size, int flags);
#define P9_UPL_MAP(upl, a)              kernel_upl_map(kernel_map, (upl), (a))
#define P9_UPL_UNMAP(upl)               kernel_upl_unmap(kernel_map, (upl))
#define P9_UPL_COMMIT(upl, o, s, f)     kernel_upl_commit_range((upl), (o), (s), (f), NULL, 0)
#define P9_UPL_ABORT(upl, o, s, f)      kernel_upl_abort_range((upl), (o), (s), (f))
#else
#define P9_UPL_MAP(upl, a)              ubc_upl_map((upl), (a))
#define P9_UPL_UNMAP(upl)               ubc_upl_unmap(upl)
#define P9_UPL_COMMIT(upl, o, s, f)     ubc_upl_commit_range((upl), (o), (s), (f))
#define P9_UPL_ABORT(upl, o, s, f)      ubc_upl_abort_range((upl), (o), (s), (f))
#endif

/* A struct dirent's length for a name of n bytes: the fixed part, the name and its NUL, rounded
 * to 4. */
#define P9_DIRENTSIZE(n)    ((8 + (n) + 1 + 3) & ~3)

typedef int (*p9_vop_t)(void *);

/* ------------------------------------------------------------------------------------------ */
/* helpers                                                                                     */
/* ------------------------------------------------------------------------------------------ */

/* Frees the pathname buffer lookup saved, as nop_abortop does. */
static void
p9_freename(struct componentname *cnp)
{
    if ((cnp->cn_flags & (HASBUF | SAVESTART)) == HASBUF) {
        FREE_ZONE(cnp->cn_pnbuf, cnp->cn_pnlen, M_NAMEI);
        cnp->cn_flags &= ~HASBUF;
    }
}

/* The owner a file is shown with: the server's, or the mount's -u and -g. */
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

/* Pages a process has dirtied through a mapping, written back before read() or write() goes to
 * the server around them. */
static void
p9_pushmapped(struct vnode *vp)
{
    if (UBCINFOEXISTS(vp) && ubc_issetflags(vp, UI_WASMAPPED))
        (void) ubc_pushdirty(vp);
}

/* ------------------------------------------------------------------------------------------ */
/* names                                                                                       */
/* ------------------------------------------------------------------------------------------ */

static int
p9fs_lookup(void *v)
{
    struct vop_lookup_args *ap = (struct vop_lookup_args *) v;
    struct vnode *dvp = ap->a_dvp;
    struct vnode **vpp = ap->a_vpp;
    struct componentname *cnp = ap->a_cnp;
    struct proc *p = cnp->cn_proc;
    struct p9node *dnp = VTOP9(dvp);
    struct p9mount *pm = dnp->n_pm;
    int flags = cnp->cn_flags;
    int nameiop = cnp->cn_nameiop;
    int islast = flags & ISLASTCN;
    struct p9_qid qid;
    u_int32_t newfid;
    int error;

    *vpp = NULLVP;
    if (dvp->v_type != VDIR)
        return ENOTDIR;
    if (islast && (dvp->v_mount->mnt_flag & MNT_RDONLY) && (nameiop == DELETE || nameiop == RENAME))
        return EROFS;
    if ((error = VOP_ACCESS(dvp, VEXEC, cnp->cn_cred, p)))
        return error;

    if (cnp->cn_namelen == 1 && cnp->cn_nameptr[0] == '.') {
        if (nameiop == RENAME && islast)
            return EISDIR;
        VREF(dvp);
        *vpp = dvp;
        if (nameiop != LOOKUP && islast)
            cnp->cn_flags |= SAVENAME;
        return 0;
    }

    newfid = p9_fid_alloc(pm);
    if (newfid == P9_NOFID)
        return ENFILE;
    if (flags & ISDOTDOT)
        error = p9_walk(pm, &dnp->n_fid, newfid, "..", 2, &qid);
    else
        error = p9_walk(pm, &dnp->n_fid, newfid, cnp->cn_nameptr, cnp->cn_namelen, &qid);
    if (error) {
        /* Not there: a create or the target of a rename may go ahead. */
        if (error == ENOENT && islast && (nameiop == CREATE || nameiop == RENAME)) {
            if (dvp->v_mount->mnt_flag & MNT_RDONLY)
                return EROFS;
            if ((error = VOP_ACCESS(dvp, VWRITE, cnp->cn_cred, p)))
                return error;
            cnp->cn_flags |= SAVENAME;
            return EJUSTRETURN;
        }
        return error;
    }

    if (islast && (nameiop == DELETE || nameiop == RENAME)) {
        if (nameiop == RENAME && qid.path == dnp->n_qpath)
            error = EISDIR;
        else
            error = VOP_ACCESS(dvp, VWRITE, cnp->cn_cred, p);
        if (error) {
            (void) p9_clunk(pm, newfid);
            return error;
        }
    }
    if (!(flags & ISDOTDOT) && qid.path == dnp->n_qpath) {
        (void) p9_clunk(pm, newfid);
        VREF(dvp);
        *vpp = dvp;
    } else if ((error = p9_nget(pm, newfid, &qid, NULL, vpp)))
        return error;
    if (nameiop != LOOKUP && islast)
        cnp->cn_flags |= SAVENAME;
    return 0;
}

static int
p9fs_create(void *v)
{
    struct vop_create_args *ap = (struct vop_create_args *) v;
    struct vnode *dvp = ap->a_dvp;
    struct componentname *cnp = ap->a_cnp;
    struct vattr *vap = ap->a_vap;
    struct p9node *dnp = VTOP9(dvp);
    struct p9mount *pm = dnp->n_pm;
    struct p9node *np;
    struct vnode *vp;
    struct p9_qid qid, wqid;
    u_int32_t ofid, pfid;
    int error;

    *ap->a_vpp = NULL;
    if (vap->va_type != VREG) {
        error = EOPNOTSUPP;
        goto out;
    }
    /* Tlcreate turns a clone of the directory's fid into the new file, opened; a second walk
     * gives the node its walked fid. */
    ofid = p9_fid_alloc(pm);
    if (ofid == P9_NOFID) {
        error = ENFILE;
        goto out;
    }
    if ((error = p9_walk(pm, &dnp->n_fid, ofid, NULL, 0, NULL)))
        goto out;
    error = p9_lcreate(pm, &ofid, cnp->cn_nameptr, cnp->cn_namelen, P9_RDWR | P9_CREAT | P9_EXCL,
                       vap->va_mode & ALLPERMS, cnp->cn_cred->cr_gid, &qid);
    if (error) {
        (void) p9_clunk(pm, ofid);
        goto out;
    }
    dnp->n_attrtime = 0;
    pfid = p9_fid_alloc(pm);
    if (pfid == P9_NOFID)
        error = ENFILE;
    else
        error = p9_walk(pm, &dnp->n_fid, pfid, cnp->cn_nameptr, cnp->cn_namelen, &wqid);
    if (!error)
        error = p9_nget(pm, pfid, &qid, NULL, &vp);
    if (error) {
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
out:
    p9_freename(cnp);
    vput(dvp);
    return error;
}

static int
p9fs_mkdir(void *v)
{
    struct vop_mkdir_args *ap = (struct vop_mkdir_args *) v;
    struct vnode *dvp = ap->a_dvp;
    struct componentname *cnp = ap->a_cnp;
    struct p9node *dnp = VTOP9(dvp);
    struct p9mount *pm = dnp->n_pm;
    struct p9_qid qid, wqid;
    u_int32_t fid;
    int error;

    *ap->a_vpp = NULL;
    error = p9_mkdir(pm, &dnp->n_fid, cnp->cn_nameptr, cnp->cn_namelen, ap->a_vap->va_mode & ALLPERMS,
                     cnp->cn_cred->cr_gid, &qid);
    if (error)
        goto out;
    dnp->n_attrtime = 0;
    fid = p9_fid_alloc(pm);
    if (fid == P9_NOFID)
        error = ENFILE;
    else
        error = p9_walk(pm, &dnp->n_fid, fid, cnp->cn_nameptr, cnp->cn_namelen, &wqid);
    if (!error)
        error = p9_nget(pm, fid, &qid, NULL, ap->a_vpp);
out:
    p9_freename(cnp);
    vput(dvp);
    return error;
}

/* The new link's vnode is not wanted: the caller of VOP_SYMLINK has no use for it. */
static int
p9fs_symlink(void *v)
{
    struct vop_symlink_args *ap = (struct vop_symlink_args *) v;
    struct vnode *dvp = ap->a_dvp;
    struct componentname *cnp = ap->a_cnp;
    struct p9node *dnp = VTOP9(dvp);
    struct p9_qid qid;
    int error;

    *ap->a_vpp = NULL;
    error = p9_symlink(dnp->n_pm, &dnp->n_fid, cnp->cn_nameptr, cnp->cn_namelen, ap->a_target,
                       cnp->cn_cred->cr_gid, &qid);
    if (!error)
        dnp->n_attrtime = 0;
    p9_freename(cnp);
    vput(dvp);
    return error;
}

static int
p9fs_link(void *v)
{
    struct vop_link_args *ap = (struct vop_link_args *) v;
    struct vnode *vp = ap->a_vp;
    struct vnode *tdvp = ap->a_tdvp;
    struct componentname *cnp = ap->a_cnp;
    struct p9node *tdnp;
    int error;

    if (vp->v_mount != tdvp->v_mount) {
        VOP_ABORTOP(vp, cnp);
        if (tdvp == vp)
            vrele(tdvp);
        else
            vput(tdvp);
        return EXDEV;
    }
    tdnp = VTOP9(tdvp);
    error = p9_link(tdnp->n_pm, &tdnp->n_fid, &VTOP9(vp)->n_fid, cnp->cn_nameptr, cnp->cn_namelen);
    if (!error) {
        tdnp->n_attrtime = 0;
        VTOP9(vp)->n_attrtime = 0;
    }
    p9_freename(cnp);
    vput(tdvp);
    return error;
}

static int
p9fs_remove(void *v)
{
    struct vop_remove_args *ap = (struct vop_remove_args *) v;
    struct vnode *dvp = ap->a_dvp;
    struct vnode *vp = ap->a_vp;
    struct componentname *cnp = ap->a_cnp;
    struct p9node *dnp = VTOP9(dvp);
    struct p9node *np = VTOP9(vp);
    int error;

    if (vp->v_type == VDIR)
        error = EPERM;
    else
        error = p9_unlinkat(dnp->n_pm, &dnp->n_fid, cnp->cn_nameptr, cnp->cn_namelen, 0);
    if (!error) {
        /* The vnode lives on for whoever has it open, but no lookup will find it again. */
        p9_unhash(np);
        cache_purge(vp);
        dnp->n_attrtime = 0;
        np->n_attrtime = 0;
        if (UBCINFOEXISTS(vp))
            (void) ubc_uncache(vp);
    }
    p9_freename(cnp);
    if (dvp == vp)
        vrele(vp);
    else
        vput(vp);
    vput(dvp);
    return error;
}

static int
p9fs_rmdir(void *v)
{
    struct vop_rmdir_args *ap = (struct vop_rmdir_args *) v;
    struct vnode *dvp = ap->a_dvp;
    struct vnode *vp = ap->a_vp;
    struct componentname *cnp = ap->a_cnp;
    struct p9node *dnp = VTOP9(dvp);
    int error;

    if (dvp == vp)
        error = EINVAL;
    else
        error = p9_unlinkat(dnp->n_pm, &dnp->n_fid, cnp->cn_nameptr, cnp->cn_namelen, P9_AT_REMOVEDIR);
    if (!error) {
        p9_unhash(VTOP9(vp));
        cache_purge(dvp);
        cache_purge(vp);
        dnp->n_attrtime = 0;
    }
    p9_freename(cnp);
    if (dvp == vp)
        vrele(dvp);
    else
        vput(dvp);
    vput(vp);
    return error;
}

/* The source vnodes come referenced, the target ones locked and referenced; all are released
 * here. Two names of one file are left alone, as POSIX has it. QEMU's server moves the paths of
 * the fids under a renamed name along with it. */
static int
p9fs_rename(void *v)
{
    struct vop_rename_args *ap = (struct vop_rename_args *) v;
    struct vnode *fdvp = ap->a_fdvp;
    struct vnode *fvp = ap->a_fvp;
    struct vnode *tdvp = ap->a_tdvp;
    struct vnode *tvp = ap->a_tvp;
    struct componentname *fcnp = ap->a_fcnp;
    struct componentname *tcnp = ap->a_tcnp;
    struct p9node *fdnp, *tdnp;
    int error = 0;

    if (fvp->v_mount != tdvp->v_mount || (tvp && fvp->v_mount != tvp->v_mount)) {
        (void) nop_rename(v);
        return EXDEV;
    }
    fdnp = VTOP9(fdvp);
    tdnp = VTOP9(tdvp);
    if (tvp != fvp) {
        error = p9_renameat(fdnp->n_pm, &fdnp->n_fid, fcnp->cn_nameptr, fcnp->cn_namelen,
                            &tdnp->n_fid, tcnp->cn_nameptr, tcnp->cn_namelen);
        if (!error) {
            if (tvp) {
                p9_unhash(VTOP9(tvp));
                cache_purge(tvp);
                if (UBCINFOEXISTS(tvp))
                    (void) ubc_uncache(tvp);
            }
            cache_purge(fvp);
            if (fvp->v_type == VDIR) {
                cache_purge(fdvp);
                cache_purge(tdvp);
            }
            fdnp->n_attrtime = 0;
            tdnp->n_attrtime = 0;
            VTOP9(fvp)->n_attrtime = 0;
        }
    }
    if (tdvp == tvp)
        vrele(tdvp);
    else
        vput(tdvp);
    if (tvp)
        vput(tvp);
    vrele(fdvp);
    vrele(fvp);
    return error;
}

/* ------------------------------------------------------------------------------------------ */
/* attributes                                                                                  */
/* ------------------------------------------------------------------------------------------ */

static int
p9fs_access(void *v)
{
    struct vop_access_args *ap = (struct vop_access_args *) v;
    struct vnode *vp = ap->a_vp;
    struct ucred *cred = ap->a_cred;
    struct p9node *np = VTOP9(vp);
    mode_t mode = ap->a_mode, mask;
    uid_t uid;
    gid_t gid;
    int error;

    if ((mode & VWRITE) && (vp->v_mount->mnt_flag & MNT_RDONLY)) {
        switch (vp->v_type) {
        case VREG:
        case VDIR:
        case VLNK:
            return EROFS;
        default:
            break;
        }
    }
    if (cred->cr_uid == 0)
        return 0;
    if ((error = p9_attr_refresh(np, 0)))
        return error;
    p9_owner(np, &uid, &gid);
    /* VREAD, VWRITE and VEXEC are the owner's permission bits; shifted, the group's and others'. */
    if (cred->cr_uid == uid)
        mask = mode;
    else if (groupmember(gid, cred))
        mask = mode >> 3;
    else
        mask = mode >> 6;
    return ((np->n_attr.mode & mask) == mask) ? 0 : EACCES;
}

static int
p9fs_getattr(void *v)
{
    struct vop_getattr_args *ap = (struct vop_getattr_args *) v;
    struct vnode *vp = ap->a_vp;
    struct vattr *vap = ap->a_vap;
    struct p9node *np = VTOP9(vp);
    struct p9_attr *a = &np->n_attr;
    uid_t uid;
    gid_t gid;
    int error;

    if ((error = p9_attr_refresh(np, 0)))
        return error;
    p9_owner(np, &uid, &gid);
    vap->va_type = vp->v_type;
    vap->va_mode = a->mode & ALLPERMS;
    vap->va_nlink = (a->nlink > 0xffff) ? 0xffff : (short) a->nlink;
    vap->va_uid = uid;
    vap->va_gid = gid;
    vap->va_fsid = vp->v_mount->mnt_stat.f_fsid.val[0];
    vap->va_fileid = p9_fileid(np->n_qpath);
    vap->va_size = (vp->v_type == VREG) ? np->n_size : a->size;
    vap->va_blocksize = vp->v_mount->mnt_stat.f_iosize;
    vap->va_atime.tv_sec = (time_t) a->atime_sec;
    vap->va_atime.tv_nsec = (long) a->atime_nsec;
    vap->va_mtime.tv_sec = (time_t) a->mtime_sec;
    vap->va_mtime.tv_nsec = (long) a->mtime_nsec;
    vap->va_ctime.tv_sec = (time_t) a->ctime_sec;
    vap->va_ctime.tv_nsec = (long) a->ctime_nsec;
    vap->va_gen = 0;
    vap->va_flags = 0;
    vap->va_rdev = (dev_t) a->rdev;
    vap->va_bytes = a->blocks * 512;
    vap->va_filerev = 0;
    vap->va_vaflags = 0;
    return 0;
}

static int
p9fs_setattr(void *v)
{
    struct vop_setattr_args *ap = (struct vop_setattr_args *) v;
    struct vnode *vp = ap->a_vp;
    struct vattr *vap = ap->a_vap;
    struct ucred *cred = ap->a_cred;
    struct p9node *np = VTOP9(vp);
    struct p9_setattr sa;
    uid_t uid;
    gid_t gid;
    int error, owner;

    if (vap->va_type != VNON || vap->va_nlink != VNOVAL || vap->va_fsid != VNOVAL ||
        vap->va_fileid != VNOVAL || vap->va_blocksize != VNOVAL || vap->va_rdev != VNOVAL ||
        vap->va_bytes != VNOVAL || vap->va_gen != VNOVAL)
        return EINVAL;
    if ((error = p9_attr_refresh(np, 0)))
        return error;
    p9_owner(np, &uid, &gid);
    owner = (cred->cr_uid == 0 || cred->cr_uid == uid);
    bzero(&sa, sizeof(sa));

    /* No file flags here, but asking for none is no failure (cp -p asks). */
    if (vap->va_flags != VNOVAL && vap->va_flags != 0)
        return EOPNOTSUPP;
    if (vap->va_uid != (uid_t) VNOVAL || vap->va_gid != (gid_t) VNOVAL) {
        if (!owner)
            return EPERM;
        if (vap->va_uid != (uid_t) VNOVAL) {
            sa.valid |= P9_SETATTR_UID;
            sa.uid = vap->va_uid;
        }
        if (vap->va_gid != (gid_t) VNOVAL) {
            sa.valid |= P9_SETATTR_GID;
            sa.gid = vap->va_gid;
        }
    }
    if (vap->va_size != VNOVAL) {
        if (vp->v_type == VDIR)
            return EISDIR;
        if (vp->v_type != VREG)
            return EINVAL;
        sa.valid |= P9_SETATTR_SIZE;
        sa.size = vap->va_size;
    }
    if (vap->va_mode != (mode_t) VNOVAL) {
        if (!owner)
            return EPERM;
        sa.valid |= P9_SETATTR_MODE;
        sa.mode = vap->va_mode & ALLPERMS;
    }
    if (vap->va_atime.tv_sec != VNOVAL || vap->va_mtime.tv_sec != VNOVAL) {
        if (!owner && (!(vap->va_vaflags & VA_UTIMES_NULL) ||
                       (error = VOP_ACCESS(vp, VWRITE, cred, ap->a_p))))
            return error ? error : EPERM;
        if (vap->va_vaflags & VA_UTIMES_NULL)
            sa.valid |= P9_SETATTR_ATIME | P9_SETATTR_MTIME;    /* the server's now */
        else {
            if (vap->va_atime.tv_sec != VNOVAL) {
                sa.valid |= P9_SETATTR_ATIME | P9_SETATTR_ATIME_SET;
                sa.atime_sec = vap->va_atime.tv_sec;
                sa.atime_nsec = vap->va_atime.tv_nsec;
            }
            if (vap->va_mtime.tv_sec != VNOVAL) {
                sa.valid |= P9_SETATTR_MTIME | P9_SETATTR_MTIME_SET;
                sa.mtime_sec = vap->va_mtime.tv_sec;
                sa.mtime_nsec = vap->va_mtime.tv_nsec;
            }
        }
    }
    if (sa.valid == 0)
        return 0;
    if (vp->v_mount->mnt_flag & MNT_RDONLY)
        return EROFS;

    if (sa.valid & P9_SETATTR_SIZE)
        p9_pushmapped(vp);
    error = p9_setattr(np->n_pm, &np->n_fid, &sa);
    if (error == ENOENT && np->n_wfid != P9_NOFID)
        error = p9_setattr(np->n_pm, &np->n_wfid, &sa);
    if (error)
        return error;
    np->n_attrtime = 0;
    if (sa.valid & P9_SETATTR_SIZE) {
        np->n_flag |= NP_MODIFIED;
        np->n_size = sa.size;
        if (UBCINFOEXISTS(vp))
            (void) ubc_setsize(vp, (off_t) sa.size);
    }
    return 0;
}

/* ------------------------------------------------------------------------------------------ */
/* open, close, read, write                                                                    */
/* ------------------------------------------------------------------------------------------ */

/* Close-to-open consistency, as NFS gives it: the attributes are fetched afresh, and if
 * somebody else has changed the file since it was last opened here, its mapped pages are
 * dropped. The fid for the access asked for is opened now, so that a refusal is open()'s. */
static int
p9fs_open(void *v)
{
    struct vop_open_args *ap = (struct vop_open_args *) v;
    struct vnode *vp = ap->a_vp;
    struct p9node *np = VTOP9(vp);
    u_int32_t *fidp;
    u_int64_t key;
    int error;

    switch (vp->v_type) {
    case VDIR:
        np->n_diroff = 0;
        np->n_dircookie = 0;
        return 0;
    case VREG:
        break;
    default:
        return EOPNOTSUPP;      /* devices, FIFOs and sockets on the host are not ours to open */
    }
    if ((error = p9_attr_refresh(np, 1)))
        return error;
    key = p9_mtimekey(&np->n_attr);
    if (np->n_flag & NP_MODIFIED)
        np->n_flag &= ~NP_MODIFIED;
    else if (key != np->n_openmtime && UBCINFOEXISTS(vp) && !(vp->v_flag & VTEXT) && np->n_size)
        (void) ubc_invalidate(vp, (off_t) 0, (size_t) np->n_size);
    np->n_openmtime = key;
    return p9_openfid(np, (ap->a_mode & FWRITE) ? FWRITE : FREAD, &fidp);
}

static int
p9fs_close(void *v)
{
    return 0;
}

static int
p9fs_read(void *v)
{
    struct vop_read_args *ap = (struct vop_read_args *) v;
    struct vnode *vp = ap->a_vp;
    struct uio *uio = ap->a_uio;
    struct p9node *np = VTOP9(vp);
    struct p9mount *pm = np->n_pm;
    u_int32_t *fidp, n, got;
    caddr_t buf;
    int error = 0, retried = 0;

    if (vp->v_type == VDIR)
        return EISDIR;
    if (vp->v_type != VREG)
        return EOPNOTSUPP;
    if (uio->uio_offset < 0)
        return EINVAL;
    if (uio->uio_resid == 0)
        return 0;
    p9_pushmapped(vp);

    n = (uio->uio_resid < pm->pm_iosize) ? uio->uio_resid : pm->pm_iosize;
    MALLOC(buf, caddr_t, n, M_TEMP, M_WAITOK);
    while (uio->uio_resid > 0) {
        if ((error = p9_openfid(np, FREAD, &fidp)))
            break;
        n = (uio->uio_resid < pm->pm_iosize) ? uio->uio_resid : pm->pm_iosize;
        error = p9_read(pm, fidp, (u_int64_t) uio->uio_offset, n, buf, &got);
        if (error == EBADF && !retried++)
            continue;           /* closed under us: open again, once */
        if (error || got == 0)
            break;
        if ((error = uiomove(buf, got, uio)))
            break;
    }
    FREE(buf, M_TEMP);
    return error;
}

static int
p9fs_write(void *v)
{
    struct vop_write_args *ap = (struct vop_write_args *) v;
    struct vnode *vp = ap->a_vp;
    struct uio *uio = ap->a_uio;
    struct proc *p = uio->uio_procp;
    struct p9node *np = VTOP9(vp);
    struct p9mount *pm = np->n_pm;
    u_int32_t *fidp, n, done, sent;
    off_t start;
    caddr_t buf;
    int error = 0, retried = 0;

    if (vp->v_type != VREG)
        return EISDIR;
    if (uio->uio_offset < 0)
        return EINVAL;
    if (uio->uio_resid == 0)
        return 0;
    if (ap->a_ioflag & IO_APPEND) {
        if ((error = p9_attr_refresh(np, 1)))
            return error;
        uio->uio_offset = (off_t) np->n_size;
    }
    if (p && uio->uio_offset + uio->uio_resid > p->p_rlimit[RLIMIT_FSIZE].rlim_cur) {
        psignal(p, SIGXFSZ);
        return EFBIG;
    }
    p9_pushmapped(vp);
    start = uio->uio_offset;

    n = (uio->uio_resid < pm->pm_iosize) ? uio->uio_resid : pm->pm_iosize;
    MALLOC(buf, caddr_t, n, M_TEMP, M_WAITOK);
    while (uio->uio_resid > 0) {
        off_t off = uio->uio_offset;

        n = (uio->uio_resid < pm->pm_iosize) ? uio->uio_resid : pm->pm_iosize;
        if ((error = uiomove(buf, n, uio)))
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
    FREE(buf, M_TEMP);

    np->n_flag |= NP_MODIFIED;
    np->n_attrtime = 0;
    if (UBCINFOEXISTS(vp)) {
        (void) ubc_setsize(vp, (off_t) np->n_size);
        if (ubc_issetflags(vp, UI_WASMAPPED) && uio->uio_offset > start)
            (void) ubc_invalidate(vp, start, (size_t) (uio->uio_offset - start));
    }
    if (!error && (ap->a_ioflag & IO_SYNC))
        error = p9_fsync(pm, &np->n_wfid);
    return error;
}

static int
p9fs_fsync(void *v)
{
    struct vop_fsync_args *ap = (struct vop_fsync_args *) v;
    struct vnode *vp = ap->a_vp;
    struct p9node *np = VTOP9(vp);

    if (UBCINFOEXISTS(vp))
        (void) ubc_pushdirty(vp);
    if (np->n_wfid == P9_NOFID)
        return 0;
    return p9_fsync(np->n_pm, &np->n_wfid);
}

/* ------------------------------------------------------------------------------------------ */
/* directories and links                                                                       */
/* ------------------------------------------------------------------------------------------ */

/* The offset a directory is read at is the index of the next entry, not the server's cookie:
 * telldir() and seekdir() keep it in a long, and QEMU's cookies are the host's 64-bit ones. The
 * node remembers where the last read stopped, so reading straight through costs nothing; any
 * other offset is reached by reading from the start. */
static int
p9fs_readdir(void *v)
{
    struct vop_readdir_args *ap = (struct vop_readdir_args *) v;
    struct vnode *vp = ap->a_vp;
    struct uio *uio = ap->a_uio;
    struct p9node *np = VTOP9(vp);
    struct p9mount *pm = np->n_pm;
    struct dirent d;
    u_int32_t *fidp, got, pos;
    u_int64_t cookie, ecookie;
    off_t idx, target;
    struct p9_qid qid;
    u_int16_t nlen;
    u_int8_t *buf, *e, type;
    int error, eof = 0, full = 0;

    if (vp->v_type != VDIR)
        return ENOTDIR;
    if (ap->a_ncookies) {
        *ap->a_ncookies = 0;
        *ap->a_cookies = NULL;
    }
    target = uio->uio_offset;
    if (target < 0)
        return EINVAL;
    if (target != 0 && target == np->n_diroff) {
        idx = target;
        cookie = np->n_dircookie;
    } else {
        idx = 0;
        cookie = 0;
    }
    if ((error = p9_openfid(np, FREAD, &fidp)))
        return error;

    MALLOC(buf, u_int8_t *, pm->pm_iosize, M_TEMP, M_WAITOK);
    while (!eof && !full && uio->uio_resid > 0) {
        error = p9_readdir(pm, fidp, cookie, pm->pm_iosize, buf, &got);
        if (error)
            break;
        if (got == 0) {
            eof = 1;
            break;
        }
        /* qid[13] offset[8] type[1] name[s], little-endian */
        for (pos = 0; pos + 24 <= got; pos += 24 + nlen) {
            e = buf + pos;
            qid.path = (u_int64_t) e[5] | ((u_int64_t) e[6] << 8) | ((u_int64_t) e[7] << 16) |
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
                d.d_fileno = p9_fileid(qid.path);
                d.d_type = type;    /* Linux's DT_* are BSD's */
                d.d_namlen = (u_int8_t) nlen;
                bcopy(e + 24, d.d_name, nlen);
                d.d_reclen = P9_DIRENTSIZE(nlen);
                if (d.d_reclen > uio->uio_resid) {
                    full = 1;
                    break;
                }
                if ((error = uiomove((caddr_t) &d, d.d_reclen, uio)))
                    break;
            }
            idx++;
            cookie = ecookie;
        }
        if (error)
            break;
    }
    FREE(buf, M_TEMP);

    uio->uio_offset = idx;      /* uiomove counted bytes */
    np->n_diroff = idx;
    np->n_dircookie = cookie;
    if (ap->a_eofflag)
        *ap->a_eofflag = eof;
    return error;
}

static int
p9fs_readlink(void *v)
{
    struct vop_readlink_args *ap = (struct vop_readlink_args *) v;
    struct p9node *np = VTOP9(ap->a_vp);
    u_int32_t len;
    char *buf;
    int error;

    if (ap->a_vp->v_type != VLNK)
        return EINVAL;
    MALLOC(buf, char *, MAXPATHLEN, M_TEMP, M_WAITOK);
    error = p9_readlink(np->n_pm, &np->n_fid, buf, MAXPATHLEN, &len);
    if (!error)
        error = uiomove(buf, len, ap->a_uio);
    FREE(buf, M_TEMP);
    return error;
}

/* ------------------------------------------------------------------------------------------ */
/* paging                                                                                      */
/* ------------------------------------------------------------------------------------------ */

/* Reads a page list's range of the file into it: what mmap and exec are made of. The part past
 * the end of the file is zeroes. */
static int
p9fs_pagein(void *v)
{
    struct vop_pagein_args *ap = (struct vop_pagein_args *) v;
    struct vnode *vp = ap->a_vp;
    struct p9node *np = VTOP9(vp);
    struct p9mount *pm;
    upl_t pl = ap->a_pl;
    vm_offset_t ploff = ap->a_pl_offset, ioaddr;
    off_t off = ap->a_f_offset;
    size_t size = ap->a_size, xsize, done = 0;
    u_int32_t *fidp, n, got;
    int error = 0, retried = 0;

    if (np == NULL || size == 0 || off < 0 || (u_int64_t) off >= np->n_size ||
        (off & PAGE_MASK_64) || (size & PAGE_MASK)) {
        error = EINVAL;
        goto out;
    }
    pm = np->n_pm;
    if (P9_UPL_MAP(pl, &ioaddr) != KERN_SUCCESS) {
        error = EIO;
        goto out;
    }
    ioaddr += ploff;
    xsize = size;
    if ((u_int64_t) off + size > np->n_size)
        xsize = (size_t) (np->n_size - off);
    while (done < xsize) {
        if ((error = p9_openfid(np, FREAD, &fidp)))
            break;
        n = xsize - done;
        if (n > pm->pm_iosize)
            n = pm->pm_iosize;
        error = p9_read(pm, fidp, (u_int64_t) off + done, n, (void *) (ioaddr + done), &got);
        if (error == EBADF && !retried++)
            continue;
        if (error || got == 0)
            break;
        done += got;
    }
    if (!error && done < size)
        bzero((caddr_t) (ioaddr + done), size - done);
    (void) P9_UPL_UNMAP(pl);
out:
    if (!(ap->a_flags & UPL_NOCOMMIT)) {
        if (error)
            (void) P9_UPL_ABORT(pl, ploff, size, UPL_ABORT_ERROR | UPL_ABORT_FREE_ON_EMPTY);
        else
            (void) P9_UPL_COMMIT(pl, ploff, size, UPL_COMMIT_CLEAR_DIRTY | UPL_COMMIT_FREE_ON_EMPTY);
    }
    return error;
}

/* Writes a page list's range back, up to the end of the file. */
static int
p9fs_pageout(void *v)
{
    struct vop_pageout_args *ap = (struct vop_pageout_args *) v;
    struct vnode *vp = ap->a_vp;
    struct p9node *np = VTOP9(vp);
    struct p9mount *pm;
    upl_t pl = ap->a_pl;
    vm_offset_t ploff = ap->a_pl_offset, ioaddr;
    off_t off = ap->a_f_offset;
    size_t size = ap->a_size, xsize, sent = 0;
    u_int32_t *fidp, n, done;
    int error = 0, retried = 0;

    if (np == NULL || size == 0 || off < 0 || (u_int64_t) off >= np->n_size ||
        (off & PAGE_MASK_64) || (size & PAGE_MASK)) {
        error = EINVAL;
        goto out;
    }
    if (vp->v_mount->mnt_flag & MNT_RDONLY) {
        error = EROFS;
        goto out;
    }
    pm = np->n_pm;
    if (P9_UPL_MAP(pl, &ioaddr) != KERN_SUCCESS) {
        error = EIO;
        goto out;
    }
    ioaddr += ploff;
    xsize = size;
    if ((u_int64_t) off + size > np->n_size)
        xsize = (size_t) (np->n_size - off);
    while (sent < xsize) {
        if ((error = p9_openfid(np, FWRITE, &fidp)))
            break;
        n = xsize - sent;
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
    (void) P9_UPL_UNMAP(pl);
    if (!error) {
        np->n_flag |= NP_MODIFIED;
        np->n_attrtime = 0;
    }
out:
    if (!(ap->a_flags & UPL_NOCOMMIT)) {
        if (error)
            (void) P9_UPL_ABORT(pl, ploff, size, UPL_ABORT_FREE_ON_EMPTY);
        else
            (void) P9_UPL_COMMIT(pl, ploff, size, UPL_COMMIT_CLEAR_DIRTY | UPL_COMMIT_FREE_ON_EMPTY);
    }
    if (error)
        printf("9p: pageout of %d bytes at %d failed: %d\n", (int) size, (int) off, error);
    return error;
}

/* The cluster layer's block size for a file without blocks: a page. */
static int
p9fs_blktooff(void *v)
{
    struct vop_blktooff_args *ap = (struct vop_blktooff_args *) v;

    *ap->a_offset = (off_t) ap->a_lblkno * PAGE_SIZE_64;
    return 0;
}

static int
p9fs_offtoblk(void *v)
{
    struct vop_offtoblk_args *ap = (struct vop_offtoblk_args *) v;

    *ap->a_lblkno = (daddr_t) (ap->a_offset / PAGE_SIZE_64);
    return 0;
}

static int
p9fs_bmap(void *v)
{
    struct vop_bmap_args *ap = (struct vop_bmap_args *) v;

    if (ap->a_vpp != NULL)
        *ap->a_vpp = ap->a_vp;
    if (ap->a_bnp != NULL)
        *ap->a_bnp = ap->a_bn * btodb(ap->a_vp->v_mount->mnt_stat.f_iosize, DEV_BSIZE);
    if (ap->a_runp != NULL)
        *ap->a_runp = 0;
    return 0;
}

/* ------------------------------------------------------------------------------------------ */
/* the rest                                                                                    */
/* ------------------------------------------------------------------------------------------ */

static int
p9fs_pathconf(void *v)
{
    struct vop_pathconf_args *ap = (struct vop_pathconf_args *) v;

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

/* Advisory locks are granted and hold nothing back, as on an NFS mount with nolock. These
 * kernels have no lock manager a filesystem outside them can use -- sys/lockf.h declares
 * lf_advlock, but UFS and NFS each carry their own and 10.0's kernel exports none -- and QEMU's
 * server answers every Tlock with success without locking anything. */
static int
p9fs_advlock(void *v)
{
    struct vop_advlock_args *ap = (struct vop_advlock_args *) v;

    if (ap->a_op == F_GETLK)
        ap->a_fl->l_type = F_UNLCK;
    return 0;
}

static int
p9fs_ioctl(void *v)
{
    return ENOTTY;
}

static int
p9fs_select(void *v)
{
    return 1;
}

static int
p9fs_mmap(void *v)
{
    return 0;
}

static int
p9fs_print(void *v)
{
    struct vop_print_args *ap = (struct vop_print_args *) v;
    struct p9node *np = VTOP9(ap->a_vp);

    if (np)
        printf("tag VT_OTHER (9p), qid.path 0x%x%08x, fid %u, open %d/%d/%d\n",
               (u_int32_t) (np->n_qpath >> 32), (u_int32_t) np->n_qpath, np->n_fid,
               np->n_rfid != P9_NOFID, np->n_wfid != P9_NOFID, np->n_dfid != P9_NOFID);
    return 0;
}

/* ------------------------------------------------------------------------------------------ */
/* the vector                                                                                  */
/* ------------------------------------------------------------------------------------------ */

int (**p9fs_vnodeop_p)(void *);

static struct vnodeopv_entry_desc p9fs_vnodeop_entries[] = {
    { &vop_default_desc,    (p9_vop_t) vn_default_error },
    { &vop_lookup_desc,     (p9_vop_t) p9fs_lookup },
    { &vop_create_desc,     (p9_vop_t) p9fs_create },
    { &vop_mknod_desc,      (p9_vop_t) err_mknod },
    { &vop_open_desc,       (p9_vop_t) p9fs_open },
    { &vop_close_desc,      (p9_vop_t) p9fs_close },
    { &vop_access_desc,     (p9_vop_t) p9fs_access },
    { &vop_getattr_desc,    (p9_vop_t) p9fs_getattr },
    { &vop_setattr_desc,    (p9_vop_t) p9fs_setattr },
    { &vop_read_desc,       (p9_vop_t) p9fs_read },
    { &vop_write_desc,      (p9_vop_t) p9fs_write },
    { &vop_lease_desc,      (p9_vop_t) nop_lease },
    { &vop_ioctl_desc,      (p9_vop_t) p9fs_ioctl },
    { &vop_select_desc,     (p9_vop_t) p9fs_select },
    { &vop_revoke_desc,     (p9_vop_t) vop_revoke },
    { &vop_mmap_desc,       (p9_vop_t) p9fs_mmap },
    { &vop_fsync_desc,      (p9_vop_t) p9fs_fsync },
    { &vop_seek_desc,       (p9_vop_t) nop_seek },
    { &vop_remove_desc,     (p9_vop_t) p9fs_remove },
    { &vop_link_desc,       (p9_vop_t) p9fs_link },
    { &vop_rename_desc,     (p9_vop_t) p9fs_rename },
    { &vop_mkdir_desc,      (p9_vop_t) p9fs_mkdir },
    { &vop_rmdir_desc,      (p9_vop_t) p9fs_rmdir },
    { &vop_symlink_desc,    (p9_vop_t) p9fs_symlink },
    { &vop_readdir_desc,    (p9_vop_t) p9fs_readdir },
    { &vop_readlink_desc,   (p9_vop_t) p9fs_readlink },
    { &vop_abortop_desc,    (p9_vop_t) nop_abortop },
    { &vop_inactive_desc,   (p9_vop_t) p9fs_inactive },
    { &vop_reclaim_desc,    (p9_vop_t) p9fs_reclaim },
    { &vop_lock_desc,       (p9_vop_t) vop_nolock },
    { &vop_unlock_desc,     (p9_vop_t) vop_nounlock },
    { &vop_islocked_desc,   (p9_vop_t) vop_noislocked },
    { &vop_bmap_desc,       (p9_vop_t) p9fs_bmap },
    { &vop_strategy_desc,   (p9_vop_t) err_strategy },
    { &vop_print_desc,      (p9_vop_t) p9fs_print },
    { &vop_pathconf_desc,   (p9_vop_t) p9fs_pathconf },
    { &vop_advlock_desc,    (p9_vop_t) p9fs_advlock },
    { &vop_pagein_desc,     (p9_vop_t) p9fs_pagein },
    { &vop_pageout_desc,    (p9_vop_t) p9fs_pageout },
    { &vop_copyfile_desc,   (p9_vop_t) err_copyfile },
    { &vop_searchfs_desc,   (p9_vop_t) err_searchfs },
    { &vop_blktooff_desc,   (p9_vop_t) p9fs_blktooff },
    { &vop_offtoblk_desc,   (p9_vop_t) p9fs_offtoblk },
    { &vop_cmap_desc,       (p9_vop_t) err_cmap },
    { NULL, NULL }
};

struct vnodeopv_desc p9fs_vnodeop_opv_desc = { &p9fs_vnodeop_p, p9fs_vnodeop_entries };
