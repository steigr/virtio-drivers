/*
 * A 9P2000.L filesystem over virtio-9p, for IRIX 6.5: the mount and the module.
 *
 * Load with ml ld -j p9fs.o -p p9fs_ -a 9p.  mount(2) takes the device's tag as
 * the special file and, in its data, "aname=dir" to mount a directory under the
 * share rather than the top of it.
 *
 * The share belongs to the host: files can change or go at any time, there is
 * no lease or callback, and every attribute is at most a clock tick old when it
 * decides anything.  Data is cached, close to open: a close pushes what the
 * file wrote, and an open or a read notices a file the host has changed.
 */
#include <sys/types.h>
#include <sys/capability.h>
#include <sys/cmn_err.h>
#include <sys/cred.h>
#include <sys/debug.h>
#include <sys/errno.h>
#include <sys/fs_subr.h>
#include <sys/kmem.h>
#include <sys/ksynch.h>
#include <sys/mload.h>
#include <sys/mount.h>
#include <sys/param.h>
#include <sys/sema.h>
#include <sys/statvfs.h>
#include <sys/systm.h>
#include <sys/uio.h>
#include <sys/ddi.h>
#include <ksys/behavior.h>
#include <sys/vfs.h>
#include <sys/vnode.h>

#include "p9fs_impl.h"

char	*p9fs_mversion = M_VERSION;
int	p9fs_fstype;

static int p9fs_major;
static unsigned int p9fs_minor;

#define OPTS_MAX	256
#define SPEC_MAX	80

/* The value of "name=" in a comma-separated list, into val. */
static int
opt_get(const char *opts, const char *name, char *val, size_t cap)
{
	size_t nlen = strlen(name), i;
	const char *p = opts;

	while (*p != 0) {
		if (strncmp(p, name, nlen) == 0 && p[nlen] == '=') {
			p += nlen + 1;
			for (i = 0; i + 1 < cap && p[i] != 0 && p[i] != ','; i++)
				val[i] = p[i];
			val[i] = 0;
			return 1;
		}
		while (*p != 0 && *p != ',')
			p++;
		if (*p == ',')
			p++;
	}
	return 0;
}

/*ARGSUSED*/
static int
p9fs_mount(struct vfs *vfsp, struct vnode *mvp, struct mounta *uap, char *attrs,
	   struct cred *cr)
{
	struct p9_client *c;
	struct p9mount *pm;
	struct p9_attr a;
	struct p9_qid qid;
	struct p9node *root;
	char spec[SPEC_MAX];
	char *opts, *aname;
	size_t got;
	int err;

	if (!_CAP_CRABLE(cr, CAP_MOUNT_MGT))
		return EPERM;
	if (mvp->v_type != VDIR)
		return ENOTDIR;
	if ((c = p9dev_client()) == 0)
		return ENODEV;
	if ((err = copyinstr(uap->spec, spec, sizeof(spec), &got)) != 0)
		return err;
	if (strcmp(spec, p9dev_tag()) != 0)
		return ENODEV;

	opts = kmem_zalloc(OPTS_MAX, KM_SLEEP);
	aname = kmem_zalloc(OPTS_MAX, KM_SLEEP);
	if ((uap->flags & MS_DATA) && uap->datalen > 0) {
		if (uap->datalen >= OPTS_MAX ||
		    copyin(uap->dataptr, opts, uap->datalen) != 0) {
			err = EINVAL;
			goto out;
		}
		(void)opt_get(opts, "aname", aname, OPTS_MAX);
	}

	pm = kmem_zalloc(sizeof(*pm), KM_SLEEP);
	pm->pm_vfs = vfsp;
	pm->pm_c = c;
	pm->pm_hashlock = MUTEX_ALLOC(MUTEX_DEFAULT, KM_SLEEP, "p9hash");
	pm->pm_rootfid = p9dev_newfid();
	if ((err = p9_attach(c, pm->pm_rootfid, aname, &qid)) != 0)
		goto fail;
	if ((err = p9_getattr(c, pm->pm_rootfid, &a)) != 0)
		goto fail_clunk;

	vfs_insertbhv(vfsp, &pm->pm_bhv, &p9fs_vfsops, pm);
	vfsp->vfs_bsize = P9FS_BSIZE;
	vfsp->vfs_fstype = p9fs_fstype;
	vfsp->vfs_dev = makedevice(p9fs_major, atomicAddUint(&p9fs_minor, 1) & 0x3ffff);
	vfsp->vfs_fsid.val[0] = vfsp->vfs_dev;
	vfsp->vfs_fsid.val[1] = p9fs_fstype;

	/* The root keeps a fid of its own, so the attach fid stays for statfs. */
	{
		p9_u32 fid = p9dev_newfid();

		if ((err = p9_walk(c, pm->pm_rootfid, fid, 0, 0)) != 0)
			goto fail_bhv;
		if ((err = p9node_get(pm, fid, &a, &root)) != 0)
			goto fail_bhv;
	}
	VN_FLAGSET(root->pn_vp, VROOT);
	pm->pm_root = root;
	p9dev_hold();
	err = 0;
	goto out;

fail_bhv:
	VFS_REMOVEBHV(vfsp, &pm->pm_bhv);
fail_clunk:
	(void)p9_clunk(c, pm->pm_rootfid);
fail:
	MUTEX_DEALLOC(pm->pm_hashlock);
	kmem_free(pm, sizeof(*pm));
out:
	kmem_free(opts, OPTS_MAX);
	kmem_free(aname, OPTS_MAX);
	return err;
}

/*ARGSUSED*/
static int
p9fs_unmount(bhv_desc_t *bdp, int flags, struct cred *cr)
{
	struct p9mount *pm = BHV_TO_PM(bdp);
	struct vfs *vfsp = pm->pm_vfs;
	struct p9node *pn;
	vnode_t *rootvp = pm->pm_root->pn_vp;
	vmap_t vmap;
	unsigned int i;

	if (!_CAP_CRABLE(cr, CAP_MOUNT_MGT))
		return EPERM;
	p9fs_sync_all(pm, 1);

	/* Busy if anything but the mount's own hold on the root is in use. */
	MUTEX_LOCK(pm->pm_hashlock, -1);
	if (rootvp->v_count != 1) {
		MUTEX_UNLOCK(pm->pm_hashlock);
		return EBUSY;
	}
	for (i = 0; i < P9FS_HASHSZ; i++) {
		for (pn = pm->pm_hash[i]; pn != 0; pn = pn->pn_hnext) {
			if (pn != pm->pm_root && pn->pn_vp->v_count != 0) {
				MUTEX_UNLOCK(pm->pm_hashlock);
				return EBUSY;
			}
		}
	}
	/* VOP_RECLAIM takes the node off the chain, so start over each time. */
	for (i = 0; i < P9FS_HASHSZ; i++) {
		while ((pn = pm->pm_hash[i]) != 0) {
			if (pn == pm->pm_root) {
				pn = pn->pn_hnext;
				if (pn == 0)
					break;
			}
			VMAP(pn->pn_vp, vmap);
			MUTEX_UNLOCK(pm->pm_hashlock);
			vn_purge(pn->pn_vp, &vmap);
			MUTEX_LOCK(pm->pm_hashlock, -1);
		}
	}
	VMAP(rootvp, vmap);
	MUTEX_UNLOCK(pm->pm_hashlock);
	VN_RELE(rootvp);
	vn_purge(rootvp, &vmap);

	(void)p9_clunk(pm->pm_c, pm->pm_rootfid);
	VFS_REMOVEBHV(vfsp, &pm->pm_bhv);
	MUTEX_DEALLOC(pm->pm_hashlock);
	kmem_free(pm, sizeof(*pm));
	p9dev_rele();
	return 0;
}

static int
p9fs_root(bhv_desc_t *bdp, struct vnode **vpp)
{
	vnode_t *vp = BHV_TO_PM(bdp)->pm_root->pn_vp;

	VN_HOLD(vp);
	*vpp = vp;
	return 0;
}

/*ARGSUSED*/
static int
p9fs_statvfs(bhv_desc_t *bdp, struct statvfs *sp, struct vnode *vp)
{
	struct p9mount *pm = BHV_TO_PM(bdp);
	struct vfs *vfsp = pm->pm_vfs;
	struct p9_statfs st;
	int err;

	if ((err = p9_statfs(pm->pm_c, pm->pm_rootfid, &st)) != 0)
		return err;
	bzero(sp, sizeof(*sp));
	sp->f_bsize = P9FS_BSIZE;
	sp->f_frsize = st.bsize;
	sp->f_blocks = (fsblkcnt_t)st.blocks;
	sp->f_bfree = (fsblkcnt_t)st.bfree;
	sp->f_bavail = (fsblkcnt_t)st.bavail;
	sp->f_files = (fsfilcnt_t)st.files;
	sp->f_ffree = (fsfilcnt_t)st.ffree;
	sp->f_favail = (fsfilcnt_t)st.ffree;
	sp->f_fsid = vfsp->vfs_dev;
	strcpy(sp->f_basetype, P9FS_NAME);
	sp->f_flag = vf_to_stf(vfsp->vfs_flag);
	sp->f_namemax = st.namelen != 0 ? st.namelen : 255;
	return 0;
}

/*ARGSUSED*/
static int
p9fs_sync(bhv_desc_t *bdp, int flags, struct cred *cr)
{
	/* bdflush pushes old buffers by itself and must not block here. */
	if (flags & SYNC_BDFLUSH)
		return 0;
	if (flags & (SYNC_DELWRI | SYNC_CLOSE | SYNC_WAIT | SYNC_PDFLUSH))
		p9fs_sync_all(BHV_TO_PM(bdp), (flags & (SYNC_WAIT | SYNC_CLOSE)) != 0);
	return 0;
}

vfsops_t p9fs_vfsops = {
	BHV_IDENTITY_INIT_POSITION(VFS_POSITION_BASE),
	p9fs_mount,
	(int (*)(struct vfs *))fs_nosys,			/* rootinit */
	(int (*)(bhv_desc_t *, struct vnode *, struct mounta *,
	    struct cred *))fs_nosys,				/* mntupdate */
	fs_dounmount,
	p9fs_unmount,
	p9fs_root,
	p9fs_statvfs,
	p9fs_sync,
	(int (*)(bhv_desc_t *, struct vnode **, struct fid *))fs_nosys, /* vget */
	(int (*)(bhv_desc_t *, enum whymountroot))fs_nosys,	/* mountroot */
	fs_realvfsops,
	fs_import,
	fs_quotactl,
	(int (*)(bhv_desc_t *))fs_noerr,			/* force_pinned */
	VFSOPS_MAGIC,
	VFS_OPSVER_65MR,
	0,
	0,							/* init_vnode */
};

/*ARGSUSED*/
int
p9fs_init(struct vfssw *vswp, int fstype)
{
	int err;

	p9fs_fstype = fstype;
	p9fs_major = getudev();
	/* With no device the type exists and mounting it says so. */
	if ((err = p9dev_init()) != 0 && err != ENODEV)
		cmn_err(CE_WARN, "9p: attach failed, error %d", err);
	return 0;
}

int
p9fs_unload(void)
{
	return p9dev_fini();
}
