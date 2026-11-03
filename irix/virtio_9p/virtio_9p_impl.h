/*
 * What the parts of the 9P filesystem share.
 *
 * p9fs_dev.c owns the virtio device and carries 9P messages over it; p9.c is
 * the protocol; p9fs_vfs.c and p9fs_vnops.c are the filesystem.
 */
#ifndef _P9FS_IMPL_H_
#define _P9FS_IMPL_H_

#include "p9.h"

/* The name of the filesystem type, as mount and ml know it. */
#define P9FS_NAME	"9p"

/*
 * The block the buffer cache works in: one Tread or Twrite each, so it fits
 * in a message.  Large enough for any page size these machines have.
 */
#define P9FS_BSIZE	32768

/* Most data in one message, and the message's size. */
#define P9FS_MSIZE	(65536 + P9_IOHDR)

/* -------- the device, p9fs_dev.c -------- */

/* The device's client, or null with no device or before Tversion. */
struct p9_client *p9dev_client(void);
/* The tag the device was configured with, to match against mount's first word. */
const char	*p9dev_tag(void);
p9_u32		p9dev_newfid(void);
/* Count of mounts: the module does not unload under one. */
void		p9dev_hold(void);
void		p9dev_rele(void);
int		p9dev_init(void);
int		p9dev_fini(void);

/* -------- the filesystem -------- */

#define P9FS_HASHSZ	257

struct p9node;

struct p9mount {
	bhv_desc_t	pm_bhv;
	struct vfs	*pm_vfs;
	struct p9_client *pm_c;
	mutex_t		*pm_hashlock;
	struct p9node	*pm_hash[P9FS_HASHSZ];
	struct p9node	*pm_root;
	p9_u32		pm_rootfid;
};

/* pn_flags */
#define P9N_SELFMOD	0x1	/* mtime moved because of us: no need to invalidate */
#define P9N_INVAL	0x2	/* the host changed the file: cached pages are suspect */
#define P9N_REMOVED	0x4	/* removed through this mount */

/*
 * A file.  There is one per qid, so that two paths to a file share their pages.
 *
 * pn_fid is a walked fid, for anything by name, and a fid is a name: the host
 * may rename the file or remove that name, so every lookup that finds the file
 * replaces it with the fid that just worked.  The one before stays in pn_oldfid
 * until the next replacement, for a round trip that had read it already.
 * pn_rfid and pn_wfid are open fids for I/O; the buffer cache writes after close,
 * so they outlive an open and go when the vnode goes inactive.  pn_lock covers
 * pn_attr, pn_size and pn_flags and is held only briefly; pn_fidlock
 * serializes opening the fids and is held across the round trip.
 */
struct p9node {
	bhv_desc_t	pn_bhv;
	vnode_t		*pn_vp;
	struct p9mount	*pn_mnt;
	struct p9node	*pn_hnext;
	p9_u64		pn_path;
	p9_u32		pn_fid;
	p9_u32		pn_oldfid;	/* P9_NOFID if none */
	p9_u32		pn_rfid;	/* P9_NOFID until opened */
	p9_u32		pn_wfid;
	int		pn_wfid_rw;	/* pn_wfid can also read */
	mutex_t		*pn_lock;
	mutex_t		*pn_fidlock;
	mrlock_t	pn_rwlock;
	struct p9_attr	pn_attr;
	off_t		pn_size;	/* with what the buffer cache has yet to write */
	unsigned long	pn_attrtime;	/* lbolt when pn_attr was read */
	int		pn_flags;
	int		pn_writing;	/* strategy writes under way */
	int		pn_error;	/* a delayed write failed: for fsync and close */
};

#define BHV_TO_PN(bdp)	((struct p9node *)BHV_PDATA(bdp))
#define BHV_TO_PM(bdp)	((struct p9mount *)BHV_PDATA(bdp))
#define VP_TO_PN(vp)	BHV_TO_PN(vn_bhv_base_unlocked(VN_BHV_HEAD(vp)))
#define PN_CLIENT(pn)	((pn)->pn_mnt->pm_c)

extern vnodeops_t	p9fs_vnodeops;
extern vfsops_t		p9fs_vfsops;
extern int		p9fs_fstype;

/*
 * The node for what fid names, with a hold on its vnode.  Takes over fid: the
 * node keeps it, or clunks it if the node has one already.
 */
int	p9node_get(struct p9mount *pm, p9_u32 fid, const struct p9_attr *a,
		   struct p9node **pnp);
/* Push what the buffer cache holds for every file, and wait if asked. */
void	p9fs_sync_all(struct p9mount *pm, int wait);

#endif /* _P9FS_IMPL_H_ */
