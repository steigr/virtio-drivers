/*
 * p9fs: mounting, and the filesystem's place in the kernel's list of them.
 *
 * A kext adds a filesystem on these releases as smbfs (smb-115, 10.3) and webdav_fs do: it fills
 * in its vnode operations vector itself, from the kernel's list of operations (vfs_opv_numops,
 * each vop_*_desc's offset), and hands vfsconf_add a struct vfsconf; vfsconf_del takes it out
 * again. Both run under the kernel funnel.
 */

#include "p9fs.h"
#include "p9_transport.h"

#include <sys/proc.h>
#include <sys/kernel.h>
#include <sys/malloc.h>
#include <sys/stat.h>
#include <sys/errno.h>
#include <kern/thread.h>

extern int vfs_opv_numops;
extern struct vnodeopv_desc p9fs_vnodeop_opv_desc;

typedef int (*p9_pfi_t)(void *);

/* ------------------------------------------------------------------------------------------ */
/* the filesystem                                                                              */
/* ------------------------------------------------------------------------------------------ */

static void
p9_unmountdev(struct mount *mp, struct p9mount *pm)
{
    mp->mnt_data = (qaddr_t) 0;
    if (pm->pm_dev)
        p9t_close(pm->pm_dev);
    p9_nodefini(pm);
    if (pm->pm_fidmap)
        FREE(pm->pm_fidmap, M_TEMP);
    FREE(pm, M_TEMP);
}

static int p9fs_statfs(struct mount *mp, struct statfs *sbp, struct proc *p);

/* mount_9p's struct p9fs_args names the device by its mount tag; the device is the session,
 * so a tag is mounted once at a time. */
static int
p9fs_mount(struct mount *mp, char *path, caddr_t data, struct nameidata *ndp, struct proc *p)
{
    struct p9fs_args args;
    struct p9mount *pm;
    struct p9_qid qid;
    struct p9_attr attr;
    struct vnode *rvp;
    unsigned int bufsize;
    u_int32_t fid;
    size_t size;
    int error;

    if (mp->mnt_flag & MNT_UPDATE)
        return EOPNOTSUPP;
    if (data == NULL)
        return EINVAL;
    if ((error = copyin(data, (caddr_t) &args, sizeof(args))))
        return error;
    if (args.version != P9FS_ARGSVERSION) {
        printf("9p: mount_9p speaks version %d of the arguments, the kernel %d\n", args.version,
               P9FS_ARGSVERSION);
        return EINVAL;
    }
    args.tag[sizeof(args.tag) - 1] = 0;
    args.aname[sizeof(args.aname) - 1] = 0;
    args.uname[sizeof(args.uname) - 1] = 0;

    MALLOC(pm, struct p9mount *, sizeof(*pm), M_TEMP, M_WAITOK);
    bzero(pm, sizeof(*pm));
    pm->pm_mp = mp;
    pm->pm_args = args;
    MALLOC(pm->pm_fidmap, u_int8_t *, P9_MAXFIDS / 8, M_TEMP, M_WAITOK);
    bzero(pm->pm_fidmap, P9_MAXFIDS / 8);
    mp->mnt_data = (qaddr_t) pm;

    error = p9t_open(args.tag, &pm->pm_dev, &pm->pm_tbuf, &pm->pm_rbuf, &bufsize);
    if (error) {
        if (error == ENXIO)
            printf("9p: no virtio 9P device has the mount tag \"%s\"\n", args.tag);
        pm->pm_dev = NULL;
        goto bad;
    }
    pm->pm_msize = bufsize;
    if ((error = p9_version(pm)))
        goto bad;
    if ((error = p9_nodeinit(pm)))
        goto bad;

    fid = p9_fid_alloc(pm);
    if ((error = p9_attach(pm, fid, &qid))) {
        p9_fid_free(pm, fid);
        goto bad;
    }
    if (!(error = p9_getattr(pm, &fid, &attr)) && (attr.mode & S_IFMT) != S_IFDIR)
        error = ENOTDIR;
    if (error) {
        (void) p9_clunk(pm, fid);
        goto bad;
    }

    vfs_getnewfsid(mp);
    mp->mnt_stat.f_iosize = pm->pm_iosize & ~PAGE_MASK;
    (void) copyinstr(path, mp->mnt_stat.f_mntonname, MNAMELEN - 1, &size);
    bzero(mp->mnt_stat.f_mntonname + size, MNAMELEN - size);
    bzero(mp->mnt_stat.f_mntfromname, MNAMELEN);
    strncpy(mp->mnt_stat.f_mntfromname, args.tag, MNAMELEN - 1);

    /* The root's reference from p9_nget is the mount's, until unmount. */
    if ((error = p9_nget(pm, fid, &qid, &attr, &rvp)))
        goto bad;
    rvp->v_flag |= VROOT;
    pm->pm_root = VTOP9(rvp);
    (void) p9fs_statfs(mp, &mp->mnt_stat, p);
    return 0;

bad:
    p9_unmountdev(mp, pm);
    return error;
}

static int
p9fs_unmount(struct mount *mp, int mntflags, struct proc *p)
{
    struct p9mount *pm = VFSTOP9(mp);
    struct vnode *rvp;
    int flags = 0, error;

    if (mntflags & MNT_FORCE)
        flags |= FORCECLOSE;
    rvp = pm->pm_root ? P9TOV(pm->pm_root) : NULLVP;
    if ((error = vflush(mp, rvp, flags)))
        return error;
    if (rvp) {
        if (rvp->v_usecount > 1 && !(flags & FORCECLOSE))
            return EBUSY;
        vrele(rvp);
        vgone(rvp);
    }
    p9_unmountdev(mp, pm);
    return 0;
}

static int
p9fs_root(struct mount *mp, struct vnode **vpp)
{
    struct p9mount *pm = VFSTOP9(mp);
    struct vnode *vp;
    int error;

    if (pm == NULL || pm->pm_root == NULL)
        return EIO;
    vp = P9TOV(pm->pm_root);
    if ((error = vget(vp, LK_EXCLUSIVE, current_proc())))
        return error;
    *vpp = vp;
    return 0;
}

/* statfs's counts are longs, 32 bits: a big host filesystem is told in bigger blocks. */
static int
p9fs_statfs(struct mount *mp, struct statfs *sbp, struct proc *p)
{
    struct p9mount *pm = VFSTOP9(mp);
    struct p9_statfs sf;
    u_int64_t blocks, bfree, bavail;
    u_int32_t bsize;
    int error;

    if (pm == NULL || pm->pm_root == NULL)
        return EIO;
    if ((error = p9_statfs(pm, &pm->pm_root->n_fid, &sf)))
        return error;
    bsize = sf.bsize ? sf.bsize : DEV_BSIZE;
    blocks = sf.blocks;
    bfree = sf.bfree;
    bavail = sf.bavail;
    while (blocks > 0x7fffffffULL && bsize < 0x40000000) {
        bsize <<= 1;
        blocks >>= 1;
        bfree >>= 1;
        bavail >>= 1;
    }
    sbp->f_bsize = bsize;
    sbp->f_iosize = mp->mnt_stat.f_iosize;
    sbp->f_blocks = (long) blocks;
    sbp->f_bfree = (long) bfree;
    sbp->f_bavail = (long) bavail;
    sbp->f_files = (sf.files > 0x7fffffffULL) ? 0x7fffffff : (long) sf.files;
    sbp->f_ffree = (sf.ffree > 0x7fffffffULL) ? 0x7fffffff : (long) sf.ffree;
    if (sbp != &mp->mnt_stat) {
        sbp->f_type = mp->mnt_vfc->vfc_typenum;
        sbp->f_fsid = mp->mnt_stat.f_fsid;
        sbp->f_owner = mp->mnt_stat.f_owner;
        bcopy(mp->mnt_stat.f_mntonname, sbp->f_mntonname, MNAMELEN);
        bcopy(mp->mnt_stat.f_mntfromname, sbp->f_mntfromname, MNAMELEN);
    }
    strncpy(sbp->f_fstypename, mp->mnt_vfc->vfc_name, MFSNAMELEN);
    return 0;
}

/* Nothing is held back to be written: writes go to the server as they are made, and pages
 * dirtied through mappings go when the pager or fsync sends them. */
static int
p9fs_sync(struct mount *mp, int waitfor, struct ucred *cred, struct proc *p)
{
    return 0;
}

static int
p9fs_start(struct mount *mp, int flags, struct proc *p)
{
    return 0;
}

static int
p9fs_quotactl(struct mount *mp, int cmds, uid_t uid, caddr_t arg, struct proc *p)
{
    return EOPNOTSUPP;
}

/* No file handles: neither NFS export nor volfs lookups by number. */
static int
p9fs_vget(struct mount *mp, void *ino, struct vnode **vpp)
{
    return EOPNOTSUPP;
}

static int
p9fs_fhtovp(struct mount *mp, struct fid *fhp, struct mbuf *nam, struct vnode **vpp, int *exflagsp,
            struct ucred **credanonp)
{
    return EINVAL;
}

static int
p9fs_vptofh(struct vnode *vp, struct fid *fhp)
{
    return EINVAL;
}

static int
p9fs_init(struct vfsconf *vfsp)
{
    return 0;
}

static int
p9fs_sysctl(int *name, u_int namelen, void *oldp, size_t *oldlenp, void *newp, size_t newlen,
            struct proc *p)
{
    return EOPNOTSUPP;
}

static struct vfsops p9fs_vfsops = {
    p9fs_mount,
    p9fs_start,
    p9fs_unmount,
    p9fs_root,
    p9fs_quotactl,
    p9fs_statfs,
    p9fs_sync,
    p9fs_vget,
    p9fs_fhtovp,
    p9fs_vptofh,
    p9fs_init,
    p9fs_sysctl
};

/* ------------------------------------------------------------------------------------------ */
/* registration                                                                                */
/* ------------------------------------------------------------------------------------------ */

int
p9fs_register(void)
{
    struct vfsconf vfc;
    p9_pfi_t *vector;
    struct vnodeopv_entry_desc *e;
    boolean_t funnel_state;
    int j, error = 0;

    funnel_state = thread_funnel_set(kernel_flock, TRUE);

    MALLOC(vector, p9_pfi_t *, vfs_opv_numops * sizeof(p9_pfi_t), M_TEMP, M_WAITOK);
    bzero(vector, vfs_opv_numops * sizeof(p9_pfi_t));
    for (j = 0; p9fs_vnodeop_opv_desc.opv_desc_ops[j].opve_op; j++) {
        e = &p9fs_vnodeop_opv_desc.opv_desc_ops[j];
        /* Only vop_default has offset 0: anything else there is an operation this kernel does
         * not list. */
        if (e->opve_op->vdesc_offset == 0 && e->opve_op != &vop_default_desc) {
            printf("9p: this kernel has no vnode operation %s\n", e->opve_op->vdesc_name);
            error = EINVAL;
            break;
        }
        vector[e->opve_op->vdesc_offset] = e->opve_impl;
    }
    if (!error) {
        for (j = 0; j < vfs_opv_numops; j++)
            if (vector[j] == NULL)
                vector[j] = vector[VOFFSET(vop_default)];
        *p9fs_vnodeop_opv_desc.opv_desc_vector_p = vector;

        bzero(&vfc, sizeof(vfc));
        vfc.vfc_vfsops = &p9fs_vfsops;
        strncpy(vfc.vfc_name, P9FS_NAME, MFSNAMELEN);
        vfc.vfc_typenum = maxvfsconf++;
        if (vfsconf_add(&vfc) != 0) {
            printf("9p: the kernel has no room for another filesystem\n");
            error = ENOMEM;
        }
    }
    if (error) {
        *p9fs_vnodeop_opv_desc.opv_desc_vector_p = NULL;
        FREE(vector, M_TEMP);
    }

    thread_funnel_set(kernel_flock, funnel_state);
    return error;
}

/* EBUSY while anything is mounted. */
int
p9fs_unregister(void)
{
    struct vfsconf *vfsp;
    boolean_t funnel_state;
    int error = 0;

    funnel_state = thread_funnel_set(kernel_flock, TRUE);
    for (vfsp = vfsconf; vfsp; vfsp = vfsp->vfc_next)
        if (bcmp(vfsp->vfc_name, P9FS_NAME, sizeof(P9FS_NAME)) == 0)
            break;
    if (vfsp && vfsp->vfc_refcount > 0)
        error = EBUSY;
    else {
        if (vfsp)
            (void) vfsconf_del(P9FS_NAME);
        if (*p9fs_vnodeop_opv_desc.opv_desc_vector_p) {
            FREE(*p9fs_vnodeop_opv_desc.opv_desc_vector_p, M_TEMP);
            *p9fs_vnodeop_opv_desc.opv_desc_vector_p = NULL;
        }
    }
    thread_funnel_set(kernel_flock, funnel_state);
    return error;
}
