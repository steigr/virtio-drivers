/*
 * A 9P2000.L client: the protocol and nothing else.
 *
 * This file and p9.c know nothing about the kernel or virtio, so the same code
 * runs in tests/host on the build host.  The owner supplies buffers and the
 * round trip through struct p9_transport.
 *
 * Everything on the wire is little-endian, stored a byte at a time.  Functions
 * return 0 or an IRIX errno, translated from the Linux numbers in Rlerror.
 */
#ifndef _P9_H_
#define _P9_H_

#ifdef __sgi
typedef unsigned char p9_u8;
typedef unsigned short p9_u16;
typedef unsigned int p9_u32;
typedef unsigned long long p9_u64;
#else
#include <stddef.h>
#include <stdint.h>
typedef uint8_t p9_u8;
typedef uint16_t p9_u16;
typedef uint32_t p9_u32;
typedef uint64_t p9_u64;
#endif

#define P9_DIALECT	"9P2000.L"
#define P9_NOFID	0xffffffffU
#define P9_NOTAG	0xffff
/* Bytes around the data in Tread and Twrite messages. */
#define P9_IOHDR	24

/* qid.type */
#define P9_QTDIR	0x80
#define P9_QTSYMLINK	0x02

/* Tlopen and Tlcreate flags: Linux's. */
#define P9_O_RDONLY	00
#define P9_O_WRONLY	01
#define P9_O_RDWR	02
#define P9_O_EXCL	0200
#define P9_O_TRUNC	01000

#define P9_AT_REMOVEDIR	0x200

/* Tsetattr valid bits. */
#define P9_SETATTR_MODE		0x001
#define P9_SETATTR_UID		0x002
#define P9_SETATTR_GID		0x004
#define P9_SETATTR_SIZE		0x008
#define P9_SETATTR_ATIME	0x010
#define P9_SETATTR_MTIME	0x020
#define P9_SETATTR_ATIME_SET	0x080
#define P9_SETATTR_MTIME_SET	0x100

struct p9_qid {
	p9_u8	type;
	p9_u32	version;
	p9_u64	path;
};

struct p9_time {
	p9_u64	sec;
	p9_u64	nsec;
};

struct p9_attr {
	struct p9_qid	qid;
	p9_u32		mode;
	p9_u32		uid;
	p9_u32		gid;
	p9_u64		nlink;
	p9_u64		size;
	p9_u64		blocks;
	struct p9_time	atime;
	struct p9_time	mtime;
	struct p9_time	ctime;
};

struct p9_setattr {
	p9_u32		valid;
	p9_u32		mode;
	p9_u32		uid;
	p9_u32		gid;
	p9_u64		size;
	struct p9_time	atime;
	struct p9_time	mtime;
};

struct p9_statfs {
	p9_u32	bsize;
	p9_u64	blocks;
	p9_u64	bfree;
	p9_u64	bavail;
	p9_u64	files;
	p9_u64	ffree;
	p9_u64	fsid;
	p9_u32	namelen;
};

struct p9_dirent {
	struct p9_qid	qid;
	p9_u64		offset;		/* of the entry after this one */
	p9_u8		type;
	const char	*name;		/* in the caller's buffer, not terminated */
	p9_u16		namelen;
};

/* One message and its reply, in buffers of the size the client was given. */
struct p9_req {
	p9_u8	*tx;
	p9_u8	*rx;
	p9_u32	rxlen;		/* bytes the server wrote */
	p9_u16	tag;
};

struct p9_transport {
	/* Take a free request, waiting for one.  Its tag is already unique. */
	struct p9_req	*(*get)(void *ctx);
	/* Send txlen bytes of r->tx and wait for the reply.  0 or an errno. */
	int		(*rpc)(void *ctx, struct p9_req *r, p9_u32 txlen);
	void		(*put)(void *ctx, struct p9_req *r);
};

struct p9_client {
	const struct p9_transport *trans;
	void		*ctx;
	p9_u32		msize;		/* buffer size until p9_version agrees less */
};

/* The most data one Tread or Twrite can carry. */
#define P9_MAXIO(c)	((c)->msize - P9_IOHDR)

int	p9_version(struct p9_client *c);
int	p9_attach(struct p9_client *c, p9_u32 fid, const char *aname,
		  struct p9_qid *qid);
/* Walk fid to newfid across one name, or clone it when name is null. */
int	p9_walk(struct p9_client *c, p9_u32 fid, p9_u32 newfid, const char *name,
		struct p9_qid *qid);
int	p9_clunk(struct p9_client *c, p9_u32 fid);
int	p9_getattr(struct p9_client *c, p9_u32 fid, struct p9_attr *a);
int	p9_setattr(struct p9_client *c, p9_u32 fid, const struct p9_setattr *s);
int	p9_lopen(struct p9_client *c, p9_u32 fid, p9_u32 flags, struct p9_qid *qid);
int	p9_lcreate(struct p9_client *c, p9_u32 fid, const char *name, p9_u32 flags,
		   p9_u32 mode, p9_u32 gid, struct p9_qid *qid);
int	p9_mkdir(struct p9_client *c, p9_u32 dfid, const char *name, p9_u32 mode,
		 p9_u32 gid, struct p9_qid *qid);
int	p9_symlink(struct p9_client *c, p9_u32 dfid, const char *name,
		   const char *target, p9_u32 gid, struct p9_qid *qid);
int	p9_link(struct p9_client *c, p9_u32 dfid, p9_u32 fid, const char *name);
int	p9_unlinkat(struct p9_client *c, p9_u32 dfid, const char *name, p9_u32 flags);
int	p9_renameat(struct p9_client *c, p9_u32 olddfid, const char *oldname,
		    p9_u32 newdfid, const char *newname);
int	p9_fsync(struct p9_client *c, p9_u32 fid);
int	p9_statfs(struct p9_client *c, p9_u32 fid, struct p9_statfs *s);
/* The link's target into buf, not terminated; *len is its length. */
int	p9_readlink(struct p9_client *c, p9_u32 fid, char *buf, p9_u32 cap,
		    p9_u32 *len);
/* At most P9_MAXIO bytes; *done is how many moved.  0 at end of file. */
int	p9_read(struct p9_client *c, p9_u32 fid, p9_u64 offset, p9_u32 count,
		void *data, p9_u32 *done);
int	p9_write(struct p9_client *c, p9_u32 fid, p9_u64 offset, p9_u32 count,
		 const void *data, p9_u32 *done);
/* Raw entries into buf; take them apart with p9_dirent_next. *done 0 is the end. */
int	p9_readdir(struct p9_client *c, p9_u32 fid, p9_u64 offset, p9_u32 count,
		   void *buf, p9_u32 *done);
/* The entry at *pos in what p9_readdir returned; 0 when there are no more. */
int	p9_dirent_next(const void *buf, p9_u32 len, p9_u32 *pos, struct p9_dirent *e);

/* An errno from Rlerror, in Linux's numbering, as IRIX's. */
int	p9_errno(p9_u32 linux_errno);

#endif /* _P9_H_ */
