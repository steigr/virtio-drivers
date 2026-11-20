/*
 * p9fs: mounting and registration for the VFS KPI of 10.4 on. What p9fs_bsd_vfsops.c does with
 * vfsconf_add and a vnode operations vector of its own making, the KPI does with vfs_fsadd, which
 * builds the vector from p9fs_vnodeop_opv_desc; vfs_fsremove refuses while anything is mounted.
 *
 * The filesystem says it is thread-safe and locks its own nodes (VFS_TBLTHREADSAFE,
 * VFS_TBLFSNODELOCK): 10.4 and 10.5 would otherwise run it under the funnel or lock a vnode
 * around each operation, and the x86_64 kernels have no funnel to run it under.
 */

#include "p9fs.h"
#include "p9fs_kpi.h"
#include "p9_transport.h"

#include <sys/stat.h>
#include <sys/errno.h>
#include <sys/malloc.h>

static lck_grp_t *p9_lckgrp;
static vfstable_t p9fs_vfstable;

/* ------------------------------------------------------------------------------------------ */
/* the filesystem                                                                              */
/* ------------------------------------------------------------------------------------------ */

static void
p9_unmountdev(mount_t mp, struct p9mount *pm)
{
    vfs_setfsprivate(mp, NULL);
    if (pm->pm_dev)
        p9t_close(pm->pm_dev);
    p9_nodefini(pm);
    if (pm->pm_fidmap)
        FREE(pm->pm_fidmap, M_TEMP);
    if (pm->pm_lock)
        lck_mtx_free(pm->pm_lock, p9_lckgrp);
    FREE(pm, M_TEMP);
}

/* As p9fs_bsd_vfsops.c's: mount_9p's struct p9fs_args names the device by its mount tag. Its
 * fields are fixed-size, so a 32-bit mount_9p's arguments read the same into a 64-bit kernel. */
static int
p9fs_mount(mount_t mp, vnode_t devvp, user_addr_t data, vfs_context_t ctx)
{
    struct p9fs_args args;
    struct p9mount *pm;
    struct p9_qid qid;
    struct p9_attr attr;
    struct vfsstatfs *sf;
    vnode_t rvp;
    unsigned int bufsize;
    u_int32_t fid;
    int error;

    if (vfs_isupdate(mp))
        return ENOTSUP;
    if (data == USER_ADDR_NULL)
        return EINVAL;
    if ((error = copyin(data, &args, sizeof(args))))
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
    pm->pm_lock = lck_mtx_alloc_init(p9_lckgrp, LCK_ATTR_NULL);
    MALLOC(pm->pm_fidmap, u_int8_t *, P9_MAXFIDS / 8, M_TEMP, M_WAITOK);
    if (pm->pm_lock == NULL || pm->pm_fidmap == NULL) {
        p9_unmountdev(mp, pm);
        return ENOMEM;
    }
    bzero(pm->pm_fidmap, P9_MAXFIDS / 8);
    vfs_setfsprivate(mp, pm);

    error = p9t_open(args.tag, &pm->pm_dev, &pm->pm_tbuf, &pm->pm_rbuf, &bufsize);
    if (error) {
        if (error == ENXIO)
            printf("9p: no virtio 9P device has the mount tag \"%s\"\n", args.tag);
        pm->pm_dev = NULL;
        p9_unmountdev(mp, pm);
        return error;
    }
    pm->pm_msize = bufsize;

    P9_LOCK(pm);
    if ((error = p9_version(pm)) || (error = p9_nodeinit(pm)))
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
    sf = vfs_statfs(mp);
    sf->f_iosize = pm->pm_iosize & ~PAGE_MASK;
    bzero(sf->f_mntfromname, sizeof(sf->f_mntfromname));
    strncpy(sf->f_mntfromname, args.tag, sizeof(sf->f_mntfromname) - 1);

    /* The root's usecount is the mount's, until unmount; p9_nget's iocount goes now. */
    pm->pm_rootqpath = qid.path;
    if ((error = p9_nget(pm, fid, &qid, &attr, &rvp)))
        goto bad;
    pm->pm_root = VTOP9(rvp);
    P9_UNLOCK(pm);
    (void) vnode_ref(rvp);
    (void) vnode_put(rvp);
    return 0;

bad:
    P9_UNLOCK(pm);
    p9_unmountdev(mp, pm);
    return error;
}

static int
p9fs_unmount(mount_t mp, int mntflags, vfs_context_t ctx)
{
    struct p9mount *pm = VFSTOP9(mp);
    vnode_t rvp;
    int flags = 0, error;

    if (mntflags & MNT_FORCE)
        flags |= FORCECLOSE;
    rvp = pm->pm_root ? P9TOV(pm->pm_root) : NULL;
    if ((error = vflush(mp, rvp, flags)))
        return error;
    if (rvp) {
        if (!(flags & FORCECLOSE) && vnode_isinuse(rvp, 1))
            return EBUSY;
        vnode_rele(rvp);
    }
    (void) vflush(mp, NULL, flags | FORCECLOSE);
    p9_unmountdev(mp, pm);
    return 0;
}

static int
p9fs_root(mount_t mp, vnode_t *vpp, vfs_context_t ctx)
{
    struct p9mount *pm = VFSTOP9(mp);
    vnode_t vp;
    int error;

    if (pm == NULL || pm->pm_root == NULL)
        return EIO;
    vp = P9TOV(pm->pm_root);
    if ((error = vnode_get(vp)))
        return error;
    *vpp = vp;
    return 0;
}

/* The KPI's counts are 64 bits: the server's go through as they are. */
static int
p9fs_getattr(mount_t mp, struct vfs_attr *fsap, vfs_context_t ctx)
{
    struct p9mount *pm = VFSTOP9(mp);
    struct p9_statfs sf;
    int error;

    if (pm == NULL || pm->pm_root == NULL)
        return EIO;
    P9_LOCK(pm);
    error = p9_statfs(pm, &pm->pm_root->n_fid, &sf);
    P9_UNLOCK(pm);
    if (error)
        return error;
    VFSATTR_RETURN(fsap, f_bsize, sf.bsize ? sf.bsize : DEV_BSIZE);
    VFSATTR_RETURN(fsap, f_iosize, pm->pm_iosize & ~PAGE_MASK);
    VFSATTR_RETURN(fsap, f_blocks, sf.blocks);
    VFSATTR_RETURN(fsap, f_bfree, sf.bfree);
    VFSATTR_RETURN(fsap, f_bavail, sf.bavail);
    VFSATTR_RETURN(fsap, f_bused, sf.blocks - sf.bfree);
    VFSATTR_RETURN(fsap, f_files, sf.files);
    VFSATTR_RETURN(fsap, f_ffree, sf.ffree);
    return 0;
}

/* As p9fs_bsd_vfsops.c's: nothing is held back to be written. */
static int
p9fs_sync(mount_t mp, int waitfor, vfs_context_t ctx)
{
    return 0;
}

static int
p9fs_start(mount_t mp, int flags, vfs_context_t ctx)
{
    return 0;
}

static int
p9fs_init(struct vfsconf *vfsp)
{
    return 0;
}

/* No file handles, quotas or sysctls, as p9fs_bsd_vfsops.c has none: the table's other entries
 * are NULL, which the KPI answers ENOTSUP for. */
static struct vfsops p9fs_vfsops = {
    p9fs_mount,
    p9fs_start,
    p9fs_unmount,
    p9fs_root,
    NULL,           /* quotactl */
    p9fs_getattr,
    p9fs_sync,
    NULL,           /* vget */
    NULL,           /* fhtovp */
    NULL,           /* vptofh */
    p9fs_init,
    NULL,           /* sysctl */
    NULL            /* setattr */
};

/* ------------------------------------------------------------------------------------------ */
/* registration                                                                                */
/* ------------------------------------------------------------------------------------------ */

static struct vnodeopv_desc *p9fs_vnodeopv_descs[] = { &p9fs_vnodeop_opv_desc };

int
p9fs_register(void)
{
    struct vfs_fsentry vfe;
    int error;

    P9_TRACE("register: lock group");
    p9_lckgrp = lck_grp_alloc_init("9p", LCK_GRP_ATTR_NULL);
    if (p9_lckgrp == NULL)
        return ENOMEM;
    bzero(&vfe, sizeof(vfe));
    vfe.vfe_vfsops = &p9fs_vfsops;
    vfe.vfe_vopcnt = 1;
    vfe.vfe_opvdescs = p9fs_vnodeopv_descs;
    strncpy(vfe.vfe_fsname, P9FS_NAME, sizeof(vfe.vfe_fsname) - 1);
    vfe.vfe_flags = VFS_TBLTHREADSAFE | VFS_TBLFSNODELOCK | VFS_TBLNOTYPENUM;
#ifdef VFS_TBL64BITREADY
    vfe.vfe_flags |= VFS_TBL64BITREADY;
#endif
    P9_TRACE("register: vfs_fsadd");
    error = vfs_fsadd(&vfe, &p9fs_vfstable);
    P9_TRACE("register: done");
    if (error) {
        printf("9p: vfs_fsadd: %d\n", error);
        lck_grp_free(p9_lckgrp);
        p9_lckgrp = NULL;
    }
    return error;
}

/* EBUSY while anything is mounted. */
int
p9fs_unregister(void)
{
    int error;

    if (p9fs_vfstable == NULL)
        return 0;
    if ((error = vfs_fsremove(p9fs_vfstable)))
        return error;
    p9fs_vfstable = NULL;
    lck_grp_free(p9_lckgrp);
    p9_lckgrp = NULL;
    return 0;
}
