/*
 * The vnode operations of the 9P filesystem.
 *
 * File data goes through the buffer cache as the IRIX NFS client's does:
 * VOP_READ and VOP_WRITE move data with chunkread, getchunk and biomove,
 * VOP_BMAP maps a file onto itself in blocks of the mount's size, and
 * VOP_STRATEGY does the I/O.  mmap and exec therefore need nothing of their
 * own: VM reads and writes the same pages through VOP_BMAP and VOP_STRATEGY.
 *
 * Strategy always finishes before it returns, async or not.  It holds no lock
 * across a round trip but pn_fidlock, and takes nothing the paging path could be
 * waiting on, since requests are preallocated and the device always answers.
 *
 * The device does no permission checks on the guest's behalf: it acts as one
 * host user whichever guest user asks, so this file does them, from attributes
 * at most a clock tick old.
 */
#include <sys/types.h>
#include <sys/buf.h>
#include <sys/capability.h>
#include <sys/cmn_err.h>
#include <sys/cred.h>
#include <sys/debug.h>
#include <sys/dirent.h>
#include <sys/errno.h>
#include <sys/fcntl.h>
#include <sys/file.h>
#include <sys/flock.h>
#include <sys/fs_subr.h>
#include <sys/kabi.h>
#include <sys/kmem.h>
#include <sys/ksynch.h>
#include <sys/param.h>
#include <sys/pathname.h>
#include <sys/sema.h>
#include <sys/stat.h>
#include <sys/systm.h>
#include <sys/uio.h>
#include <sys/uthread.h>
#include <sys/ddi.h>
#include <ksys/behavior.h>
#include <sys/vfs.h>
#include <sys/vnode.h>

#include "virtio_9p_impl.h"

extern time_t lbolt;

static int	virtio_9p_getattr(bhv_desc_t *, struct vattr *, int, struct cred *);

/*
 * A page operation's whole file, inclusive, as vn_reclaim itself asks for it.
 * Not the size: pages from when the file was longer are as much the file's.
 */
#define FILE_LAST	((off_t)0x7fffffffffffffffLL)

#define PN_TO_VFS(pn)	((pn)->pn_vp->v_vfsp)
#define IS_RDONLY(pn)	(PN_TO_VFS(pn)->vfs_flag & VFS_RDONLY)

/* -------- nodes -------- */

static vtype_t
mode_to_vtype(p9_u32 mode)
{
	switch (mode & S_IFMT) {
	case S_IFREG:	return VREG;
	case S_IFDIR:	return VDIR;
	case S_IFLNK:	return VLNK;
	case S_IFCHR:	return VCHR;
	case S_IFBLK:	return VBLK;
	case S_IFIFO:	return VFIFO;
	case S_IFSOCK:	return VSOCK;
	default:	return VNON;
	}
}

/*
 * Inode numbers are for a 32-bit stat to hold as well: a host's may not fit,
 * and the old stat calls fail with EOVERFLOW on one that does not.
 */
static ino_t
pn_ino(const struct p9node *pn)
{
	p9_u64 p = pn->pn_path;
	ino_t ino = (ino_t)((p ^ (p >> 32)) & 0xffffffffULL);

	return ino != 0 ? ino : 1;
}

static struct p9node *
pn_find_locked(struct p9mount *pm, p9_u64 path)
{
	struct p9node *pn;

	for (pn = pm->pm_hash[path % VIRTIO_9P_HASHSZ]; pn != 0; pn = pn->pn_hnext) {
		if (pn->pn_path == path)
			return pn;
	}
	return 0;
}

static void
p9node_free(struct p9node *pn)
{
	mrfree(&pn->pn_rwlock);
	MUTEX_DEALLOC(pn->pn_lock);
	MUTEX_DEALLOC(pn->pn_fidlock);
	kmem_free(pn, sizeof(*pn));
}

/*
 * Take new attributes, from a lookup or a round trip of the node's own.  This is
 * where a change on the host is noticed, whichever of those saw it first; the
 * next open or read acts on P9N_INVAL.
 */
static void
pn_set_attr(struct p9node *pn, const struct p9_attr *a)
{
	vnode_t *vp = pn->pn_vp;
	int busy;

	MUTEX_LOCK(pn->pn_lock, -1);
	busy = VN_DIRTY(vp) || pn->pn_writing != 0;
	if (vp->v_type == VREG && !busy && !(pn->pn_flags & P9N_SELFMOD) &&
	    (a->mtime.sec != pn->pn_attr.mtime.sec ||
	    a->mtime.nsec != pn->pn_attr.mtime.nsec ||
	    a->size != pn->pn_attr.size))
		pn->pn_flags |= P9N_INVAL;
	/* Our own writes are over when nothing is dirty and none is under way. */
	if (!busy)
		pn->pn_flags &= ~P9N_SELFMOD;
	pn->pn_attr = *a;
	if (!busy || (off_t)a->size > pn->pn_size)
		pn->pn_size = (off_t)a->size;
	pn->pn_attrtime = (unsigned long)lbolt;
	MUTEX_UNLOCK(pn->pn_lock);
}

int
p9node_get(struct p9mount *pm, p9_u32 fid, const struct p9_attr *a,
	   struct p9node **pnp)
{
	struct p9node *pn, *fresh = 0;
	vnode_t *vp;
	vmap_t vmap;
	p9_u32 old;

	for (;;) {
		MUTEX_LOCK(pm->pm_hashlock, -1);
		pn = pn_find_locked(pm, a->qid.path);
		if (pn != 0) {
			VMAP(pn->pn_vp, vmap);
			MUTEX_UNLOCK(pm->pm_hashlock);
			/* Null: being reclaimed, and gone from the hash next look. */
			vp = vn_get(pn->pn_vp, &vmap, 0);
			if (vp == 0)
				continue;
			if (fresh != 0) {
				vnode_t *fvp = fresh->pn_vp;

				vn_bhv_remove(VN_BHV_HEAD(fvp), &fresh->pn_bhv);
				vn_free(fvp);
				p9node_free(fresh);
			}
			/* The name just walked is the one known to work. */
			MUTEX_LOCK(pn->pn_lock, -1);
			old = pn->pn_oldfid;
			pn->pn_oldfid = pn->pn_fid;
			pn->pn_fid = fid;
			pn->pn_flags &= ~P9N_REMOVED;
			MUTEX_UNLOCK(pn->pn_lock);
			if (old != P9_NOFID)
				(void)p9_clunk(pm->pm_c, old);
			pn_set_attr(pn, a);
			*pnp = pn;
			return 0;
		}
		if (fresh != 0)
			break;
		MUTEX_UNLOCK(pm->pm_hashlock);

		/* vn_alloc can reclaim a vnode, which takes the hash lock. */
		fresh = kmem_zalloc(sizeof(*fresh), KM_SLEEP);
		fresh->pn_mnt = pm;
		fresh->pn_path = a->qid.path;
		fresh->pn_fid = fid;
		fresh->pn_oldfid = P9_NOFID;
		fresh->pn_rfid = P9_NOFID;
		fresh->pn_wfid = P9_NOFID;
		fresh->pn_lock = MUTEX_ALLOC(MUTEX_DEFAULT, KM_SLEEP, "p9node");
		fresh->pn_fidlock = MUTEX_ALLOC(MUTEX_DEFAULT, KM_SLEEP, "p9fid");
		mrlock_init(&fresh->pn_rwlock, MRLOCK_DEFAULT, "p9rw", -1);
		fresh->pn_attr = *a;
		fresh->pn_size = (off_t)a->size;
		fresh->pn_attrtime = (unsigned long)lbolt;
		vp = vn_alloc(pm->pm_vfs, mode_to_vtype(a->mode), 0);
		fresh->pn_vp = vp;
		bhv_desc_init(&fresh->pn_bhv, fresh, vp, &virtio_9p_vnodeops);
		vn_bhv_insert_initial(VN_BHV_HEAD(vp), &fresh->pn_bhv);
	}
	pn = fresh;
	pn->pn_hnext = pm->pm_hash[pn->pn_path % VIRTIO_9P_HASHSZ];
	pm->pm_hash[pn->pn_path % VIRTIO_9P_HASHSZ] = pn;
	MUTEX_UNLOCK(pm->pm_hashlock);
	*pnp = pn;
	return 0;
}

/* The fid to ask about the file: one with a descriptor outlives the name. */
static p9_u32
pn_stat_fid(struct p9node *pn)
{
	if (pn->pn_rfid != P9_NOFID)
		return pn->pn_rfid;
	if (pn->pn_wfid != P9_NOFID)
		return pn->pn_wfid;
	return pn->pn_fid;
}

/* Bring pn_attr up to date, at most once a tick. */
static int
pn_refresh(struct p9node *pn)
{
	struct p9_attr a;
	int err;

	if (pn->pn_attrtime == (unsigned long)lbolt)
		return 0;
	err = p9_getattr(PN_CLIENT(pn), pn_stat_fid(pn), &a);
	/* A fid is a name: another file may have taken it by now. */
	if (err == 0 && a.qid.path != pn->pn_path)
		err = ENOENT;
	/* Nothing better to report on a file the host has taken away. */
	if (err == ENOENT)
		return 0;
	if (err != 0)
		return err;
	pn_set_attr(pn, &a);
	return 0;
}

/* Drop every cached page.  The caller may not hold the rwlock. */
static void
pn_invalidate(struct p9node *pn)
{
	mrupdate(&pn->pn_rwlock);
	VOP_FLUSHINVAL_PAGES(pn->pn_vp, 0, FILE_LAST, FI_REMAPF_LOCKED);
	mrunlock(&pn->pn_rwlock);
}

/* What an open or read does first: see what the host has done to the file. */
static int
pn_revalidate(struct p9node *pn)
{
	int err, inval;

	if ((err = pn_refresh(pn)) != 0)
		return err;
	MUTEX_LOCK(pn->pn_lock, -1);
	inval = (pn->pn_flags & P9N_INVAL) != 0;
	pn->pn_flags &= ~P9N_INVAL;
	MUTEX_UNLOCK(pn->pn_lock);
	if (inval && VN_CACHED(pn->pn_vp) && !VN_DIRTY(pn->pn_vp))
		pn_invalidate(pn);
	return 0;
}

/* Push dirty data out. flags: B_ASYNC not to wait for it. */
static int
pn_flush(struct p9node *pn, uint64_t flags)
{
	vnode_t *vp = pn->pn_vp;
	int err = 0;

	if (vp->v_type != VREG || (!VN_DIRTY(vp) && !VN_MAPPED(vp)))
		return 0;
	VOP_FLUSH_PAGES(vp, 0, FILE_LAST, flags, FI_NONE, err);
	return err;
}

/* An open fid to read or write through, opened the first time it is needed. */
static int
pn_io_fid(struct p9node *pn, int writing, p9_u32 *fidp)
{
	struct p9_client *c = PN_CLIENT(pn);
	struct p9_qid q;
	p9_u32 fid;
	int err = 0;

	MUTEX_LOCK(pn->pn_fidlock, -1);
	if (writing && pn->pn_wfid != P9_NOFID) {
		*fidp = pn->pn_wfid;
	} else if (!writing && pn->pn_rfid != P9_NOFID) {
		*fidp = pn->pn_rfid;
	} else if (!writing && pn->pn_wfid != P9_NOFID && pn->pn_wfid_rw) {
		*fidp = pn->pn_wfid;
	} else {
		fid = p9dev_newfid();
		err = p9_walk(c, pn->pn_fid, fid, 0, 0);
		if (err == 0) {
			if (writing) {
				/* Read and write if it may: a write's block reads first. */
				err = p9_lopen(c, fid, P9_O_RDWR, &q);
				if (err == 0) {
					pn->pn_wfid_rw = 1;
				} else if (err == EACCES) {
					err = p9_lopen(c, fid, P9_O_WRONLY, &q);
					pn->pn_wfid_rw = 0;
				}
			} else {
				err = p9_lopen(c, fid, P9_O_RDONLY, &q);
			}
			/* The name may have passed to another file. */
			if (err == 0 && q.path != pn->pn_path)
				err = ENOENT;
			if (err != 0) {
				(void)p9_clunk(c, fid);
			} else {
				if (writing)
					pn->pn_wfid = fid;
				else
					pn->pn_rfid = fid;
				*fidp = fid;
			}
		}
	}
	MUTEX_UNLOCK(pn->pn_fidlock);
	return err;
}

static void
pn_close_fids(struct p9node *pn)
{
	struct p9_client *c = PN_CLIENT(pn);

	MUTEX_LOCK(pn->pn_fidlock, -1);
	if (pn->pn_rfid != P9_NOFID) {
		(void)p9_clunk(c, pn->pn_rfid);
		pn->pn_rfid = P9_NOFID;
	}
	if (pn->pn_wfid != P9_NOFID) {
		(void)p9_clunk(c, pn->pn_wfid);
		pn->pn_wfid = P9_NOFID;
	}
	MUTEX_UNLOCK(pn->pn_fidlock);
}

static int
pn_access(struct p9node *pn, int mode, struct cred *cr)
{
	p9_u32 m = pn->pn_attr.mode;
	int want = mode;

	if ((mode & VWRITE) && IS_RDONLY(pn) && !ISVDEV(pn->pn_vp->v_type))
		return EROFS;
	if (cr->cr_uid != pn->pn_attr.uid) {
		want >>= 3;
		if (!groupmember(pn->pn_attr.gid, cr))
			want >>= 3;
	}
	if ((m & want) == want)
		return 0;
	/* What the mode bits refuse, a capability may allow. */
	if (mode & VEXEC) {
		if (pn->pn_vp->v_type == VDIR) {
			if (!_CAP_CRABLE(cr, CAP_DAC_READ_SEARCH))
				return EACCES;
		} else if ((m & 0111) == 0 || !_CAP_CRABLE(cr, CAP_DAC_EXECUTE)) {
			return EACCES;
		}
	}
	if ((mode & VREAD) && !_CAP_CRABLE(cr, CAP_DAC_READ_SEARCH))
		return EACCES;
	if ((mode & VWRITE) && !_CAP_CRABLE(cr, CAP_DAC_WRITE))
		return EACCES;
	return 0;
}

/* The owner, or someone allowed to act as one. */
static int
pn_owner(struct p9node *pn, struct cred *cr)
{
	return cr->cr_uid == pn->pn_attr.uid || _CAP_CRABLE(cr, CAP_FOWNER);
}

/*
 * With the owners mount option, the host should record who made an entry.  A
 * create carries the creator's group but 9P2000.L's has no room for the user,
 * so the user and group follow in a Tsetattr, on any fid that names the entry.
 * The device refuses nothing here: it does as its owners option says, and a
 * device that squashes owners accepts and ignores this.
 */
static void
pn_give(struct p9node *dpn, p9_u32 fid, struct cred *cr)
{
	struct p9_setattr s;

	if (!PN_OWNERS(dpn))
		return;
	bzero(&s, sizeof(s));
	s.valid = P9_SETATTR_UID | P9_SETATTR_GID;
	s.uid = (p9_u32)cr->cr_uid;
	s.gid = (p9_u32)cr->cr_gid;
	(void)p9_setattr(PN_CLIENT(dpn), fid, &s);
}

/* The same for an entry that has no fid yet: walk to it by name. */
static void
pn_give_name(struct p9node *dpn, char *name, struct cred *cr)
{
	p9_u32 fid;

	if (!PN_OWNERS(dpn))
		return;
	fid = p9dev_newfid();
	if (p9_walk(PN_CLIENT(dpn), dpn->pn_fid, fid, name, 0) == 0)
		pn_give(dpn, fid, cr);
	(void)p9_clunk(PN_CLIENT(dpn), fid);
}

/* Walk dpn to name and make the node, with its vnode held. */
static int
pn_walk(struct p9node *dpn, char *name, struct p9node **pnp)
{
	struct p9_client *c = PN_CLIENT(dpn);
	struct p9_attr a;
	struct p9_qid q;
	p9_u32 fid = p9dev_newfid();
	int err;

	err = p9_walk(c, dpn->pn_fid, fid, name, &q);
	if (err != 0)
		return err;
	if ((err = p9_getattr(c, fid, &a)) != 0) {
		(void)p9_clunk(c, fid);
		return err;
	}
	return p9node_get(dpn->pn_mnt, fid, &a, pnp);
}

/* dpn gained or lost an entry: its times and link count moved. */
static void
pn_dir_changed(struct p9node *dpn)
{
	dpn->pn_attrtime = 0;
}

/* -------- open, close, attributes -------- */

/*ARGSUSED*/
static int
virtio_9p_open(bhv_desc_t *bdp, vnode_t **vpp, mode_t mode, struct cred *cr)
{
	struct p9node *pn = BHV_TO_PN(bdp);
	vnode_t *vp = pn->pn_vp;
	p9_u32 fid;
	int err;

	switch (vp->v_type) {
	case VREG:
		break;
	case VDIR:
		return (mode & FWRITE) ? EISDIR : 0;
	case VLNK:
		return ELOOP;
	default:
		/* A host's FIFOs and devices are the host's, and have no driver here. */
		return ENXIO;
	}
	if ((err = pn_revalidate(pn)) != 0)
		return err;
	/*
	 * Open now, while the file has its name: a program may remove it and
	 * carry on using it, and a bad open belongs to open(2).
	 */
	if ((mode & FREAD) && (err = pn_io_fid(pn, 0, &fid)) != 0)
		return err;
	if ((mode & FWRITE) && (err = pn_io_fid(pn, 1, &fid)) != 0)
		return err;
	return 0;
}

/*ARGSUSED*/
static int
virtio_9p_close(bhv_desc_t *bdp, int flags, lastclose_t lastclose, struct cred *cr)
{
	struct p9node *pn = BHV_TO_PN(bdp);
	vnode_t *vp = pn->pn_vp;
	flid_t fl;
	int err = 0;

	if (vp->v_flag & VFRLOCKS) {
		get_current_flid(&fl);
		cleanlocks(vp, fl.fl_pid, fl.fl_sysid);
	}
	if (lastclose != L_TRUE || !(flags & FWRITE) || vp->v_type != VREG)
		return 0;
	/* Close to open: another reader, the host's included, sees it all now. */
	err = pn_flush(pn, 0);
	MUTEX_LOCK(pn->pn_lock, -1);
	if (err == 0)
		err = pn->pn_error;
	pn->pn_error = 0;
	MUTEX_UNLOCK(pn->pn_lock);
	return err;
}

/*ARGSUSED*/
static int
virtio_9p_getattr(bhv_desc_t *bdp, struct vattr *vap, int flags, struct cred *cr)
{
	struct p9node *pn = BHV_TO_PN(bdp);
	vnode_t *vp = pn->pn_vp;
	int err;

	/* ATTR_COMM and ATTR_LAZY ask for what is known without a round trip. */
	if (!(flags & (ATTR_COMM | ATTR_LAZY))) {
		if ((err = pn_refresh(pn)) != 0)
			return err;
	}
	MUTEX_LOCK(pn->pn_lock, -1);
	vap->va_type = vp->v_type;
	vap->va_mode = pn->pn_attr.mode & MODEMASK;
	vap->va_uid = pn->pn_attr.uid;
	vap->va_gid = pn->pn_attr.gid;
	vap->va_fsid = vp->v_vfsp->vfs_dev;
	vap->va_nodeid = pn_ino(pn);
	vap->va_nlink = (nlink_t)pn->pn_attr.nlink;
	vap->va_size = pn->pn_size;
	vap->va_atime.tv_sec = (time_t)pn->pn_attr.atime.sec;
	vap->va_atime.tv_nsec = (long)pn->pn_attr.atime.nsec;
	vap->va_mtime.tv_sec = (time_t)pn->pn_attr.mtime.sec;
	vap->va_mtime.tv_nsec = (long)pn->pn_attr.mtime.nsec;
	vap->va_ctime.tv_sec = (time_t)pn->pn_attr.ctime.sec;
	vap->va_ctime.tv_nsec = (long)pn->pn_attr.ctime.nsec;
	vap->va_rdev = 0;
	vap->va_blksize = VIRTIO_9P_BSIZE;
	vap->va_nblocks = (blkcnt_t)pn->pn_attr.blocks;
	vap->va_vcode = 0;
	vap->va_xflags = 0;
	vap->va_extsize = 0;
	vap->va_nextents = 0;
	vap->va_anextents = 0;
	vap->va_projid = 0;
	vap->va_gencount = 0;
	MUTEX_UNLOCK(pn->pn_lock);
	return 0;
}

static int
virtio_9p_setattr(bhv_desc_t *bdp, struct vattr *vap, int flags, struct cred *cr)
{
	struct p9node *pn = BHV_TO_PN(bdp);
	vnode_t *vp = pn->pn_vp;
	struct p9_setattr s;
	struct p9_attr a;
	int mask = vap->va_mask;
	int err;

	if (mask & AT_NOSET)
		return EINVAL;
	if (IS_RDONLY(pn))
		return EROFS;
	if ((err = pn_refresh(pn)) != 0)
		return err;

	bzero(&s, sizeof(s));
	if (mask & AT_MODE) {
		if (!pn_owner(pn, cr))
			return EPERM;
		s.valid |= P9_SETATTR_MODE;
		s.mode = vap->va_mode & MODEMASK;
	}
	if (mask & (AT_UID | AT_GID)) {
		if (PN_OWNERS(pn)) {
			/*
			 * The owner is real: it takes CAP_CHOWN to change it, or
			 * for the owner to hand the file to a group it is in.
			 */
			if ((mask & AT_UID) && vap->va_uid != pn->pn_attr.uid &&
			    !_CAP_CRABLE(cr, CAP_CHOWN))
				return EPERM;
			if ((mask & AT_GID) && vap->va_gid != pn->pn_attr.gid &&
			    !_CAP_CRABLE(cr, CAP_CHOWN) &&
			    !(cr->cr_uid == pn->pn_attr.uid &&
			      groupmember(vap->va_gid, cr)))
				return EPERM;
			if (mask & AT_UID) {
				s.valid |= P9_SETATTR_UID;
				s.uid = (p9_u32)vap->va_uid;
			}
			if (mask & AT_GID) {
				s.valid |= P9_SETATTR_GID;
				s.gid = (p9_u32)vap->va_gid;
			}
		} else if (!_CAP_CRABLE(cr, CAP_CHOWN) && !pn_owner(pn, cr)) {
			/* Files have the one owner the device reports; nothing to record. */
			return EPERM;
		}
	}
	if (mask & (AT_ATIME | AT_MTIME)) {
		/* Setting a given time takes the owner; "now" takes write access. */
		if (flags & ATTR_UTIME) {
			if (!pn_owner(pn, cr))
				return EPERM;
		} else if (!pn_owner(pn, cr) && (err = pn_access(pn, VWRITE, cr)) != 0) {
			return err;
		}
		if (mask & AT_ATIME) {
			s.valid |= P9_SETATTR_ATIME;
			if (flags & ATTR_UTIME) {
				s.valid |= P9_SETATTR_ATIME_SET;
				s.atime.sec = (p9_u64)vap->va_atime.tv_sec;
				s.atime.nsec = (p9_u64)vap->va_atime.tv_nsec;
			}
		}
		if (mask & AT_MTIME) {
			s.valid |= P9_SETATTR_MTIME;
			if (flags & ATTR_UTIME) {
				s.valid |= P9_SETATTR_MTIME_SET;
				s.mtime.sec = (p9_u64)vap->va_mtime.tv_sec;
				s.mtime.nsec = (p9_u64)vap->va_mtime.tv_nsec;
			}
		}
	}
	if (mask & AT_SIZE) {
		if (vp->v_type == VDIR)
			return EISDIR;
		if (vp->v_type != VREG)
			return EINVAL;
		if (!(mask & AT_SIZE_NOPERM) && (err = pn_access(pn, VWRITE, cr)) != 0)
			return err;
		s.valid |= P9_SETATTR_SIZE;
		s.size = (p9_u64)vap->va_size;
	}

	if (s.valid & P9_SETATTR_SIZE) {
		/* What is cached past the new end must not be written back after. */
		mrupdate(&pn->pn_rwlock);
		VOP_FLUSHINVAL_PAGES(vp, 0, FILE_LAST, FI_REMAPF_LOCKED);
		MUTEX_LOCK(pn->pn_lock, -1);
		pn->pn_flags |= P9N_SELFMOD;
		MUTEX_UNLOCK(pn->pn_lock);
		err = p9_setattr(PN_CLIENT(pn), pn_stat_fid(pn), &s);
		mrunlock(&pn->pn_rwlock);
	} else if (s.valid != 0) {
		err = p9_setattr(PN_CLIENT(pn), pn_stat_fid(pn), &s);
	}
	if (err != 0)
		return err;
	if (s.valid != 0) {
		pn->pn_attrtime = 0;
		if (p9_getattr(PN_CLIENT(pn), pn_stat_fid(pn), &a) == 0)
			pn_set_attr(pn, &a);
	}
	return 0;
}

static int
virtio_9p_access(bhv_desc_t *bdp, int mode, struct cred *cr)
{
	struct p9node *pn = BHV_TO_PN(bdp);
	int err;

	if ((err = pn_refresh(pn)) != 0)
		return err;
	return pn_access(pn, mode, cr);
}

/*ARGSUSED*/
static int
virtio_9p_ioctl(bhv_desc_t *bdp, int cmd, void *arg, int flag, struct cred *cr,
	   int *rvalp, struct vopbd *vbds)
{
	return ENOTTY;
}

/*ARGSUSED*/
static int
virtio_9p_fcntl(bhv_desc_t *bdp, int cmd, void *arg, int flags, off_t offset,
	   struct cred *cr, union rval *rvp)
{
	return EINVAL;
}

/*ARGSUSED*/
static int
virtio_9p_seek(bhv_desc_t *bdp, off_t ooff, off_t *noffp)
{
	return *noffp < 0 ? EINVAL : 0;
}

/* -------- names -------- */

/*ARGSUSED*/
static int
virtio_9p_lookup(bhv_desc_t *bdp, char *name, vnode_t **vpp, struct pathname *pnp,
	    int flags, vnode_t *rdir, struct cred *cr)
{
	struct p9node *dpn = BHV_TO_PN(bdp);
	struct p9node *pn;
	int err;

	if (dpn->pn_vp->v_type != VDIR)
		return ENOTDIR;
	if ((err = pn_refresh(dpn)) != 0)
		return err;
	if ((err = pn_access(dpn, VEXEC, cr)) != 0)
		return err;
	if (name[0] == 0 || (name[0] == '.' && name[1] == 0)) {
		VN_HOLD(dpn->pn_vp);
		*vpp = dpn->pn_vp;
		return 0;
	}
	if ((err = pn_walk(dpn, name, &pn)) != 0)
		return err;
	*vpp = pn->pn_vp;
	return 0;
}

/*ARGSUSED*/
static int
virtio_9p_create(bhv_desc_t *bdp, char *name, struct vattr *vap, int flags, int mode,
	    vnode_t **vpp, struct cred *cr)
{
	struct p9node *dpn = BHV_TO_PN(bdp);
	struct p9_client *c = PN_CLIENT(dpn);
	struct p9node *pn;
	struct p9_attr a;
	struct p9_qid q;
	p9_u32 ofid, pfid;
	int err;

	/* The kernel's own lookup of an existing file comes in here, held. */
	if (*vpp != 0) {
		VN_RELE(*vpp);
		*vpp = 0;
	}
	if (vap->va_type != VREG)
		return EPERM;
	if (name[0] == 0)
		return EEXIST;
	if ((err = pn_refresh(dpn)) != 0)
		return err;
	if ((err = pn_access(dpn, VEXEC, cr)) != 0)
		return err;

	err = pn_walk(dpn, name, &pn);
	if (err == 0) {
		/* Already there: what opening it would have done. */
		if (flags & VEXCL)
			err = EEXIST;
		else if (pn->pn_vp->v_type == VDIR && (mode & VWRITE))
			err = EISDIR;
		else if (mode != 0)
			err = pn_access(pn, mode, cr);
		if (err == 0 && (vap->va_mask & AT_SIZE) && pn->pn_vp->v_type == VREG) {
			struct vattr va;

			va.va_mask = AT_SIZE;
			va.va_size = vap->va_size;
			err = virtio_9p_setattr(&pn->pn_bhv, &va, 0, cr);
		}
		if (err != 0) {
			VN_RELE(pn->pn_vp);
			return err;
		}
		*vpp = pn->pn_vp;
		return 0;
	}
	if (err != ENOENT)
		return err;
	if (IS_RDONLY(dpn))
		return EROFS;
	/* The caller may not grow a file at all (RLIMIT_FSIZE of 0). */
	if (flags & VZFS)
		return EFBIG;
	if ((err = pn_access(dpn, VWRITE, cr)) != 0)
		return err;

	/* A clone of the directory becomes the new file, open for I/O. */
	ofid = p9dev_newfid();
	if ((err = p9_walk(c, dpn->pn_fid, ofid, 0, 0)) != 0)
		return err;
	err = p9_lcreate(c, ofid, name, P9_O_RDWR | ((flags & VEXCL) ? P9_O_EXCL : 0),
	    vap->va_mode & MODEMASK, cr->cr_gid, &q);
	if (err != 0) {
		(void)p9_clunk(c, ofid);
		return err;
	}
	pn_dir_changed(dpn);
	pn_give(dpn, ofid, cr);
	pfid = p9dev_newfid();
	if ((err = p9_walk(c, dpn->pn_fid, pfid, name, 0)) != 0 ||
	    (err = p9_getattr(c, pfid, &a)) != 0) {
		(void)p9_clunk(c, pfid);
		(void)p9_clunk(c, ofid);
		return err;
	}
	if ((err = p9node_get(dpn->pn_mnt, pfid, &a, &pn)) != 0) {
		(void)p9_clunk(c, ofid);
		return err;
	}
	MUTEX_LOCK(pn->pn_fidlock, -1);
	if (pn->pn_wfid == P9_NOFID) {
		pn->pn_wfid = ofid;
		pn->pn_wfid_rw = 1;
		ofid = P9_NOFID;
	}
	MUTEX_UNLOCK(pn->pn_fidlock);
	if (ofid != P9_NOFID)
		(void)p9_clunk(c, ofid);
	*vpp = pn->pn_vp;
	return 0;
}

static int
virtio_9p_remove(bhv_desc_t *bdp, char *name, struct cred *cr)
{
	struct p9node *dpn = BHV_TO_PN(bdp);
	struct p9node *pn;
	int err;

	if (IS_RDONLY(dpn))
		return EROFS;
	if ((err = pn_refresh(dpn)) != 0)
		return err;
	if ((err = pn_access(dpn, VWRITE | VEXEC, cr)) != 0)
		return err;
	if ((err = pn_walk(dpn, name, &pn)) != 0)
		return err;
	if (pn->pn_vp->v_type == VDIR) {
		VN_RELE(pn->pn_vp);
		return EPERM;
	}
	/* In a sticky directory, only the file's owner or the directory's. */
	if ((dpn->pn_attr.mode & VSVTX) && !pn_owner(pn, cr) && !pn_owner(dpn, cr)) {
		VN_RELE(pn->pn_vp);
		return EPERM;
	}
	err = p9_unlinkat(PN_CLIENT(dpn), dpn->pn_fid, name, 0);
	if (err == 0) {
		pn_dir_changed(dpn);
		pn->pn_attrtime = 0;
		MUTEX_LOCK(pn->pn_lock, -1);
		pn->pn_flags |= P9N_REMOVED;
		if (pn->pn_attr.nlink > 0)
			pn->pn_attr.nlink--;
		MUTEX_UNLOCK(pn->pn_lock);
	}
	VN_RELE(pn->pn_vp);
	return err;
}

static int
virtio_9p_link(bhv_desc_t *bdp, vnode_t *svp, char *name, struct cred *cr)
{
	struct p9node *dpn = BHV_TO_PN(bdp);
	struct p9node *pn;
	bhv_desc_t *sbdp;
	int err;

	sbdp = vn_bhv_lookup_unlocked(VN_BHV_HEAD(svp), &virtio_9p_vnodeops);
	if (sbdp == 0)
		return EXDEV;
	pn = BHV_TO_PN(sbdp);
	if (pn->pn_mnt != dpn->pn_mnt)
		return EXDEV;
	if (svp->v_type == VDIR)
		return EPERM;
	if (IS_RDONLY(dpn))
		return EROFS;
	if ((err = pn_refresh(dpn)) != 0)
		return err;
	if ((err = pn_access(dpn, VWRITE | VEXEC, cr)) != 0)
		return err;
	err = p9_link(PN_CLIENT(dpn), dpn->pn_fid, pn->pn_fid, name);
	if (err == 0) {
		pn_dir_changed(dpn);
		pn->pn_attrtime = 0;
	}
	return err;
}

/*ARGSUSED*/
static int
virtio_9p_rename(bhv_desc_t *bdp, char *oname, vnode_t *ndvp, char *nname,
	    struct pathname *npnp, struct cred *cr)
{
	struct p9node *odpn = BHV_TO_PN(bdp);
	struct p9node *ndpn, *pn;
	bhv_desc_t *nbdp;
	int err;

	nbdp = vn_bhv_lookup_unlocked(VN_BHV_HEAD(ndvp), &virtio_9p_vnodeops);
	if (nbdp == 0)
		return EXDEV;
	ndpn = BHV_TO_PN(nbdp);
	if (ndpn->pn_mnt != odpn->pn_mnt)
		return EXDEV;
	if (IS_RDONLY(odpn))
		return EROFS;
	if (strcmp(oname, ".") == 0 || strcmp(oname, "..") == 0 ||
	    strcmp(nname, ".") == 0 || strcmp(nname, "..") == 0)
		return EINVAL;
	if ((err = pn_refresh(odpn)) != 0 || (err = pn_refresh(ndpn)) != 0)
		return err;
	if ((err = pn_access(odpn, VWRITE | VEXEC, cr)) != 0)
		return err;
	if (ndpn != odpn && (err = pn_access(ndpn, VWRITE | VEXEC, cr)) != 0)
		return err;
	/* Sticky directories, as for remove. */
	if ((odpn->pn_attr.mode & VSVTX) != 0) {
		if ((err = pn_walk(odpn, oname, &pn)) != 0)
			return err;
		if (!pn_owner(pn, cr) && !pn_owner(odpn, cr))
			err = EPERM;
		VN_RELE(pn->pn_vp);
		if (err != 0)
			return err;
	}
	err = p9_renameat(PN_CLIENT(odpn), odpn->pn_fid, oname, ndpn->pn_fid, nname);
	if (err == 0) {
		pn_dir_changed(odpn);
		pn_dir_changed(ndpn);
	}
	return err;
}

/*ARGSUSED*/
static int
virtio_9p_mkdir(bhv_desc_t *bdp, char *name, struct vattr *vap, vnode_t **vpp,
	   struct cred *cr)
{
	struct p9node *dpn = BHV_TO_PN(bdp);
	struct p9node *pn;
	struct p9_qid q;
	int err;

	if (IS_RDONLY(dpn))
		return EROFS;
	if ((err = pn_refresh(dpn)) != 0)
		return err;
	if ((err = pn_access(dpn, VWRITE | VEXEC, cr)) != 0)
		return err;
	err = p9_mkdir(PN_CLIENT(dpn), dpn->pn_fid, name, vap->va_mode & MODEMASK,
	    cr->cr_gid, &q);
	if (err != 0)
		return err;
	pn_dir_changed(dpn);
	pn_give_name(dpn, name, cr);
	if ((err = pn_walk(dpn, name, &pn)) != 0)
		return err;
	*vpp = pn->pn_vp;
	return 0;
}

static int
virtio_9p_rmdir(bhv_desc_t *bdp, char *name, vnode_t *cdir, struct cred *cr)
{
	struct p9node *dpn = BHV_TO_PN(bdp);
	struct p9node *pn;
	int err;

	if (IS_RDONLY(dpn))
		return EROFS;
	if (strcmp(name, ".") == 0)
		return EINVAL;
	if (strcmp(name, "..") == 0)
		return EEXIST;
	if ((err = pn_refresh(dpn)) != 0)
		return err;
	if ((err = pn_access(dpn, VWRITE | VEXEC, cr)) != 0)
		return err;
	if ((err = pn_walk(dpn, name, &pn)) != 0)
		return err;
	if (pn->pn_vp->v_type != VDIR)
		err = ENOTDIR;
	else if (VN_CMP(pn->pn_vp, cdir))
		err = EINVAL;
	else if ((dpn->pn_attr.mode & VSVTX) && !pn_owner(pn, cr) && !pn_owner(dpn, cr))
		err = EPERM;
	else
		err = p9_unlinkat(PN_CLIENT(dpn), dpn->pn_fid, name, P9_AT_REMOVEDIR);
	if (err == 0) {
		pn_dir_changed(dpn);
		MUTEX_LOCK(pn->pn_lock, -1);
		pn->pn_flags |= P9N_REMOVED;
		MUTEX_UNLOCK(pn->pn_lock);
	}
	VN_RELE(pn->pn_vp);
	return err;
}

/*ARGSUSED*/
static int
virtio_9p_symlink(bhv_desc_t *bdp, char *name, struct vattr *vap, char *target,
	     struct cred *cr)
{
	struct p9node *dpn = BHV_TO_PN(bdp);
	struct p9_qid q;
	int err;

	if (IS_RDONLY(dpn))
		return EROFS;
	if ((err = pn_refresh(dpn)) != 0)
		return err;
	if ((err = pn_access(dpn, VWRITE | VEXEC, cr)) != 0)
		return err;
	err = p9_symlink(PN_CLIENT(dpn), dpn->pn_fid, name, target, cr->cr_gid, &q);
	if (err == 0) {
		pn_dir_changed(dpn);
		pn_give_name(dpn, name, cr);
	}
	return err;
}

/*ARGSUSED*/
static int
virtio_9p_readlink(bhv_desc_t *bdp, struct uio *uio, struct cred *cr)
{
	struct p9node *pn = BHV_TO_PN(bdp);
	char *buf;
	p9_u32 len;
	int err;

	if (pn->pn_vp->v_type != VLNK)
		return EINVAL;
	buf = kmem_alloc(MAXPATHLEN, KM_SLEEP);
	err = p9_readlink(PN_CLIENT(pn), pn->pn_fid, buf, MAXPATHLEN, &len);
	if (err == 0)
		err = uiomove(buf, len, UIO_READ, uio);
	kmem_free(buf, MAXPATHLEN);
	return err;
}

/* -------- directories -------- */

#define RD_BUFSZ	8192
/* Room for an entry of any ABI: the largest record fs_fmtdirent makes. */
#define RD_NEED(n)	(((n) + 1 + 18 + 7) & ~7)

/*ARGSUSED*/
static int
virtio_9p_readdir(bhv_desc_t *bdp, struct uio *uio, struct cred *cr, int *eofp)
{
	struct p9node *pn = BHV_TO_PN(bdp);
	struct p9_dirent e;
	unsigned char *raw;
	char *out, *name;
	p9_u32 got, pos, used, count;
	off_t next, off;
	int err, abi, reclen;
	ino_t ino;

	if (pn->pn_vp->v_type != VDIR)
		return ENOTDIR;
	if (eofp != 0)
		*eofp = 0;
	if (uio->uio_offset < 0)
		return EINVAL;
	if ((err = pn_io_fid(pn, 0, &got)) != 0)
		return err;

	abi = GETDENTS_ABI(get_current_abi(), uio);
	raw = kmem_alloc(RD_BUFSZ, KM_SLEEP);
	out = kmem_alloc(RD_BUFSZ, KM_SLEEP);
	name = kmem_alloc(MAXNAMELEN + 1, KM_SLEEP);
	off = uio->uio_offset;
	count = uio->uio_resid < RD_BUFSZ ? (p9_u32)uio->uio_resid : RD_BUFSZ;

	/* A raw entry is about as long as a formatted one: ask for as much. */
	err = p9_readdir(PN_CLIENT(pn), pn->pn_rfid != P9_NOFID ? pn->pn_rfid :
	    pn->pn_wfid, (p9_u64)off, count, raw, &got);
	if (err != 0)
		goto out;
	if (got == 0) {
		if (eofp != 0)
			*eofp = 1;
		goto out;
	}
	pos = used = 0;
	next = off;
	while (p9_dirent_next(raw, got, &pos, &e)) {
		if (e.namelen > MAXNAMELEN)
			continue;
		if (used + RD_NEED(e.namelen) > count)
			break;
		bcopy(e.name, name, e.namelen);
		name[e.namelen] = 0;
		ino = (ino_t)((e.qid.path ^ (e.qid.path >> 32)) & 0xffffffffULL);
		if (ino == 0)
			ino = 1;
		if (ABI_IS_IRIX5(abi))
			reclen = irix5_fs_fmtdirent(out + used, ino, (off_t)e.offset, name);
		else
#if _MIPS_SIM == _ABI64
			reclen = fs_fmtdirent(out + used, ino, (off_t)e.offset, name);
#else
			reclen = irix5_n32_fs_fmtdirent(out + used, ino, (off_t)e.offset, name);
#endif
		used += reclen;
		next = (off_t)e.offset;
	}
	if (used == 0) {
		/* Not room for the first entry: the caller's buffer is too small. */
		err = EINVAL;
		goto out;
	}
	if ((err = uiomove(out, used, UIO_READ, uio)) == 0)
		uio->uio_offset = next;
out:
	kmem_free(raw, RD_BUFSZ);
	kmem_free(out, RD_BUFSZ);
	kmem_free(name, MAXNAMELEN + 1);
	return err;
}

/* -------- data -------- */

static void
pn_bmap(struct p9node *pn, off_t off, ssize_t count, struct bmapval *bmv)
{
	off_t mod = off % VIRTIO_9P_BSIZE;

	bmv->bn = bmv->offset = OFFTOBBT(off - mod);
	bmv->pmp = 0;
	bmv->length = BTOBBT(VIRTIO_9P_BSIZE);
	bmv->bsize = VIRTIO_9P_BSIZE;
	bmv->pboff = (int)mod;
	bmv->pbsize = (int)(count < VIRTIO_9P_BSIZE - mod ? count : VIRTIO_9P_BSIZE - mod);
	bmv->pbdev = PN_TO_VFS(pn)->vfs_dev;
	bmv->eof = (off - mod + VIRTIO_9P_BSIZE >= pn->pn_size) ? BMAP_EOF : 0;
}

/*ARGSUSED*/
static int
virtio_9p_bmap(bhv_desc_t *bdp, off_t offset, ssize_t count, int flags, struct cred *cr,
	  struct bmapval *bmv, int *nbmv)
{
	pn_bmap(BHV_TO_PN(bdp), offset, count, bmv);
	*nbmv = 1;
	return 0;
}

static int
p9_read_all(struct p9node *pn, off_t off, char *addr, unsigned int len)
{
	p9_u32 fid, got;
	unsigned int done = 0;
	int err;

	if ((err = pn_io_fid(pn, 0, &fid)) != 0)
		return err;
	while (done < len) {
		err = p9_read(PN_CLIENT(pn), fid, (p9_u64)(off + done), len - done,
		    addr + done, &got);
		if (err != 0)
			return err;
		/* The rest of the block is past the end of the file: zeroes. */
		if (got == 0) {
			bzero(addr + done, len - done);
			break;
		}
		done += got;
	}
	return 0;
}

static int
p9_write_all(struct p9node *pn, off_t off, char *addr, unsigned int len)
{
	p9_u32 fid, put;
	unsigned int done = 0;
	int err;

	/* Nothing can see the data of a removed file whose fids have gone. */
	if ((pn->pn_flags & P9N_REMOVED) && pn->pn_wfid == P9_NOFID)
		return 0;
	if ((err = pn_io_fid(pn, 1, &fid)) != 0)
		return err;
	while (done < len) {
		err = p9_write(PN_CLIENT(pn), fid, (p9_u64)(off + done), len - done,
		    addr + done, &put);
		if (err != 0)
			return err;
		if (put == 0)
			return EIO;
		done += put;
	}
	return 0;
}

static void
virtio_9p_strategy(bhv_desc_t *bdp, struct buf *bp)
{
	struct p9node *pn = BHV_TO_PN(bdp);
	off_t off = BBTOOFF(bp->b_offset);
	unsigned int count = bp->b_bcount;
	off_t size;
	char *addr;
	int err;

	addr = bp_mapin(bp);
	MUTEX_LOCK(pn->pn_lock, -1);
	size = pn->pn_size;
	if (!(bp->b_flags & B_READ)) {
		pn->pn_writing++;
		pn->pn_flags |= P9N_SELFMOD;
	}
	MUTEX_UNLOCK(pn->pn_lock);

	if (bp->b_flags & B_READ) {
		/* Past the end needs no round trip: a write's new block, say. */
		if (off >= size) {
			bzero(addr, count);
			err = 0;
		} else {
			err = p9_read_all(pn, off, addr, count);
		}
	} else {
		/* A block is written up to the end of the file, not past it. */
		if (off >= size)
			count = 0;
		else if (off + count > size)
			count = (unsigned int)(size - off);
		err = count != 0 ? p9_write_all(pn, off, addr, count) : 0;
		MUTEX_LOCK(pn->pn_lock, -1);
		pn->pn_writing--;
		if (err != 0)
			pn->pn_error = err;
		pn->pn_attrtime = 0;
		MUTEX_UNLOCK(pn->pn_lock);
	}
	bp_mapout(bp);

	if (err != 0) {
		bp->b_error = err;
		bp->b_flags |= B_ERROR;
	} else {
		bp->b_resid = 0;
	}
	iodone(bp);
}

static int
virtio_9p_rw(struct p9node *pn, struct uio *uio, enum uio_rw rw, int ioflag,
	struct cred *cr)
{
	vnode_t *vp = pn->pn_vp;
	struct bmapval bmv;
	struct buf *bp;
	off_t on;
	ssize_t n, limit;
	int err = 0;

	if (uio->uio_offset < 0)
		return EINVAL;
	if (rw == UIO_WRITE && (ioflag & IO_APPEND))
		uio->uio_offset = pn->pn_size;

	while (uio->uio_resid > 0 && err == 0) {
		on = uio->uio_offset % VIRTIO_9P_BSIZE;
		n = VIRTIO_9P_BSIZE - on;
		if (n > uio->uio_resid)
			n = uio->uio_resid;
		if (rw == UIO_READ) {
			if (uio->uio_offset >= pn->pn_size)
				break;
			limit = pn->pn_size - uio->uio_offset;
			if (n > limit)
				n = limit;
		} else {
			if (uio->uio_offset >= uio->uio_limit)
				return err != 0 ? err : EFBIG;
			if (n > uio->uio_limit - uio->uio_offset)
				n = uio->uio_limit - uio->uio_offset;
		}
		pn_bmap(pn, uio->uio_offset, n, &bmv);
		bmv.pmp = uio->uio_pmp;
		/* A whole block written need not be read first. */
		if (rw == UIO_WRITE && n == VIRTIO_9P_BSIZE)
			bp = getchunk(vp, &bmv, cr);
		else
			bp = chunkread(vp, &bmv, 1, cr);
		if (bp->b_flags & B_ERROR) {
			err = geterror(bp);
			brelse(bp);
			break;
		}
		err = biomove(bp, (u_int)on, (size_t)n, rw, uio);
		if (rw == UIO_READ) {
			brelse(bp);
			continue;
		}
		if (err != 0) {
			/* The block holds part of a copy that faulted: drop it. */
			bp->b_flags |= B_STALE;
			brelse(bp);
			break;
		}
		MUTEX_LOCK(pn->pn_lock, -1);
		if (uio->uio_offset > pn->pn_size)
			pn->pn_size = uio->uio_offset;
		pn->pn_flags |= P9N_SELFMOD;
		pn->pn_attrtime = 0;
		MUTEX_UNLOCK(pn->pn_lock);
		if (ioflag & (IO_SYNC | IO_DSYNC))
			err = bwrite(bp);
		else if (on + n == VIRTIO_9P_BSIZE)
			bawrite(bp);
		else
			bdwrite(bp);
	}
	return err;
}

/*ARGSUSED*/
static int
virtio_9p_read(bhv_desc_t *bdp, struct uio *uio, int ioflag, struct cred *cr,
	  struct flid *fl)
{
	struct p9node *pn = BHV_TO_PN(bdp);
	int err;

	if (pn->pn_vp->v_type == VDIR)
		return EISDIR;
	if (pn->pn_vp->v_type != VREG)
		return EINVAL;
	if (ioflag & IO_ISLOCKED) {
		/* The caller holds the rwlock, so pages stay as they are. */
		if ((err = pn_refresh(pn)) != 0)
			return err;
		return virtio_9p_rw(pn, uio, UIO_READ, ioflag, cr);
	}
	if ((err = pn_revalidate(pn)) != 0)
		return err;
	mraccess(&pn->pn_rwlock);
	err = virtio_9p_rw(pn, uio, UIO_READ, ioflag, cr);
	mrunlock(&pn->pn_rwlock);
	return err;
}

/*ARGSUSED*/
static int
virtio_9p_write(bhv_desc_t *bdp, struct uio *uio, int ioflag, struct cred *cr,
	   struct flid *fl)
{
	struct p9node *pn = BHV_TO_PN(bdp);
	int err;

	if (pn->pn_vp->v_type != VREG)
		return EISDIR;
	if (IS_RDONLY(pn))
		return EROFS;
	if (ioflag & IO_ISLOCKED)
		return virtio_9p_rw(pn, uio, UIO_WRITE, ioflag, cr);
	mrupdate(&pn->pn_rwlock);
	err = virtio_9p_rw(pn, uio, UIO_WRITE, ioflag, cr);
	mrunlock(&pn->pn_rwlock);
	return err;
}

static void
virtio_9p_rwlock(bhv_desc_t *bdp, vrwlock_t write)
{
	struct p9node *pn = BHV_TO_PN(bdp);

	if (write == VRWLOCK_READ)
		mraccess(&pn->pn_rwlock);
	else
		mrupdate(&pn->pn_rwlock);
}

/*ARGSUSED*/
static void
virtio_9p_rwunlock(bhv_desc_t *bdp, vrwlock_t write)
{
	mrunlock(&BHV_TO_PN(bdp)->pn_rwlock);
}

/*ARGSUSED*/
static int
virtio_9p_fsync(bhv_desc_t *bdp, int flags, struct cred *cr, off_t start, off_t stop)
{
	struct p9node *pn = BHV_TO_PN(bdp);
	vnode_t *vp = pn->pn_vp;
	p9_u32 fid;
	int err;

	if (vp->v_type != VREG)
		return 0;
	mrupdate(&pn->pn_rwlock);
	if (flags & FSYNC_INVAL) {
		VOP_FLUSHINVAL_PAGES(vp, 0, FILE_LAST, FI_REMAPF_LOCKED);
		err = 0;
	} else {
		err = pn_flush(pn, (flags & FSYNC_WAIT) ? 0 : B_ASYNC);
	}
	mrunlock(&pn->pn_rwlock);
	MUTEX_LOCK(pn->pn_lock, -1);
	if (err == 0)
		err = pn->pn_error;
	pn->pn_error = 0;
	MUTEX_UNLOCK(pn->pn_lock);
	if (err != 0 || !(flags & FSYNC_WAIT))
		return err;
	/* And onto the host's disk, if the file is open to write at all. */
	if (pn->pn_wfid != P9_NOFID && pn_io_fid(pn, 1, &fid) == 0)
		err = p9_fsync(PN_CLIENT(pn), fid);
	return err;
}

/* -------- a vnode's end -------- */

/*ARGSUSED*/
static int
virtio_9p_inactive(bhv_desc_t *bdp, struct cred *cr)
{
	struct p9node *pn = BHV_TO_PN(bdp);
	vnode_t *vp = pn->pn_vp;

	/*
	 * Nobody holds the file: nothing may stay dirty for lack of a later close,
	 * and the host should not keep it open on our account.  The pages may stay
	 * for the next open, unless the file is gone.
	 */
	if (vp->v_type == VREG) {
		if ((pn->pn_flags & P9N_REMOVED) && pn->pn_attr.nlink == 0 &&
		    VN_CACHED(vp)) {
			VOP_TOSS_PAGES(vp, 0, FILE_LAST, FI_NONE);
		} else {
			(void)pn_flush(pn, 0);
		}
	}
	pn_close_fids(pn);
	return VN_INACTIVE_CACHE;
}

/*ARGSUSED*/
static int
virtio_9p_reclaim(bhv_desc_t *bdp, int flag)
{
	struct p9node *pn = BHV_TO_PN(bdp);
	struct p9mount *pm = pn->pn_mnt;
	struct p9node **pp;
	vnode_t *vp = pn->pn_vp;

	/*
	 * Everything, always: vn_reclaim looks after this for pages and buffers
	 * still there, with the behavior this function is about to remove.
	 */
	VOP_FLUSHINVAL_PAGES(vp, 0, FILE_LAST, FI_NONE);

	MUTEX_LOCK(pm->pm_hashlock, -1);
	for (pp = &pm->pm_hash[pn->pn_path % VIRTIO_9P_HASHSZ]; *pp != 0;
	    pp = &(*pp)->pn_hnext) {
		if (*pp == pn) {
			*pp = pn->pn_hnext;
			break;
		}
	}
	MUTEX_UNLOCK(pm->pm_hashlock);

	pn_close_fids(pn);
	(void)p9_clunk(pm->pm_c, pn->pn_fid);
	if (pn->pn_oldfid != P9_NOFID)
		(void)p9_clunk(pm->pm_c, pn->pn_oldfid);
	vn_bhv_remove(VN_BHV_HEAD(vp), bdp);
	p9node_free(pn);
	return 0;
}

/* A flush that fails leaves the data dirty: this many tries a chain, then move on. */
#define SYNC_TRIES	64

void
virtio_9p_sync_all(struct p9mount *pm, int wait)
{
	struct p9node *pn;
	vnode_t *vp;
	vmap_t vmap;
	unsigned int i;
	int tries;

	for (i = 0; i < VIRTIO_9P_HASHSZ; i++) {
		for (tries = 0; tries < SYNC_TRIES; tries++) {
			/* From the top each time: the chain may change during a flush. */
			MUTEX_LOCK(pm->pm_hashlock, -1);
			for (pn = pm->pm_hash[i]; pn != 0; pn = pn->pn_hnext) {
				if (VN_DIRTY(pn->pn_vp))
					break;
			}
			if (pn == 0) {
				MUTEX_UNLOCK(pm->pm_hashlock);
				break;
			}
			VMAP(pn->pn_vp, vmap);
			MUTEX_UNLOCK(pm->pm_hashlock);
			if ((vp = vn_get(pn->pn_vp, &vmap, 0)) == 0)
				continue;
			(void)pn_flush(pn, wait ? 0 : B_ASYNC);
			VN_RELE(vp);
		}
	}
}

/* -------- the table -------- */

vnodeops_t virtio_9p_vnodeops = {
	BHV_IDENTITY_INIT_POSITION(VNODE_POSITION_BASE),
	virtio_9p_open,
	virtio_9p_close,
	virtio_9p_read,
	virtio_9p_write,
	virtio_9p_ioctl,
	fs_setfl,
	virtio_9p_getattr,
	virtio_9p_setattr,
	virtio_9p_access,
	virtio_9p_lookup,
	virtio_9p_create,
	virtio_9p_remove,
	virtio_9p_link,
	virtio_9p_rename,
	virtio_9p_mkdir,
	virtio_9p_rmdir,
	virtio_9p_readdir,
	virtio_9p_symlink,
	virtio_9p_readlink,
	virtio_9p_fsync,
	virtio_9p_inactive,
	(vop_fid_t)fs_nosys,
	(vop_fid2_t)fs_nosys,
	virtio_9p_rwlock,
	virtio_9p_rwunlock,
	virtio_9p_seek,
	fs_cmp,
	fs_frlock,
	(vop_realvp_t)fs_nosys,
	virtio_9p_bmap,
	virtio_9p_strategy,
	(vop_map_t)fs_noerr,
	(vop_addmap_t)fs_noerr,
	(vop_delmap_t)fs_noerr,
	fs_poll,
	(vop_dump_t)fs_nosys,
	fs_pathconf,
	(vop_allocstore_t)fs_nosys,
	virtio_9p_fcntl,
	virtio_9p_reclaim,
	(vop_attr_get_t)fs_nosys,
	(vop_attr_set_t)fs_nosys,
	(vop_attr_remove_t)fs_nosys,
	(vop_attr_list_t)fs_nosys,
	fs_cover,
	(vop_link_removed_t)fs_noval,
	fs_vnode_change,
	fs_tosspages,
	fs_flushinval_pages,
	fs_flush_pages,
	fs_invalfree_pages,
	fs_pages_sethole,
	(vop_commit_t)fs_nosys,
	(vop_readbuf_t)fs_nosys,
	fs_strgetmsg,
	fs_strputmsg,
};
