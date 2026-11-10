/*
 * 9P2000.L messages: see p9.h.
 *
 * A message is size[4] type[1] tag[2] and its fields.  The writer never runs
 * past the buffer: a request that cannot fit is ENAMETOOLONG, which only long
 * names can cause.  The reader checks every field against the bytes received,
 * since what comes back is another program's output.
 */
#include "p9.h"

#ifdef __sgi
#include <sys/types.h>
#include <sys/systm.h>
#define P9_COPY(from, to, n)	bcopy((from), (to), (n))
#else
#include <string.h>
#define P9_COPY(from, to, n)	memcpy((to), (from), (n))
#endif

#define TSTATFS		8
#define TLOPEN		12
#define TLCREATE	14
#define TSYMLINK	16
#define TREADLINK	22
#define TGETATTR	24
#define TSETATTR	26
#define TREADDIR	40
#define TFSYNC		50
#define TLINK		70
#define TMKDIR		72
#define TRENAMEAT	74
#define TUNLINKAT	76
#define TVERSION	100
#define TATTACH		104
#define TWALK		110
#define TREAD		116
#define TWRITE		118
#define TCLUNK		120
#define RLERROR		7

/* Everything Rgetattr can hold that this client reads. */
#define P9_GETATTR_BASIC	0x7ffULL

/* errno values as IRIX numbers them, for the host tests' sake kept literal. */
#define E_PERM		1
#define E_NOENT		2
#define E_IO		5
#define E_NOMEM		12
#define E_ACCES		13
#define E_BUSY		16
#define E_EXIST		17
#define E_XDEV		18
#define E_NODEV		19
#define E_NOTDIR	20
#define E_ISDIR		21
#define E_INVAL		22
#define E_NFILE		23
#define E_MFILE		24
#define E_TXTBSY	26
#define E_FBIG		27
#define E_NOSPC		28
#define E_ROFS		30
#define E_MLINK		31
#define E_RANGE		34
#define E_NOLCK		46
#define E_OVERFLOW	79
#define E_NAMETOOLONG	78
#define E_NOSYS		89
#define E_LOOP		90
#define E_NOTEMPTY	93
#define E_NOTSUP	122
#define E_STALE		151
#define E_DQUOT		1133

int
p9_errno(p9_u32 e)
{
	/* Linux and IRIX agree up to here, from the common System V ancestor. */
	if (e != 0 && e <= 34)
		return (int)e;
	switch (e) {
	case 36:	return E_NAMETOOLONG;
	case 37:	return E_NOLCK;
	case 38:	return E_NOSYS;
	case 39:	return E_NOTEMPTY;
	case 40:	return E_LOOP;
	case 75:	return E_OVERFLOW;
	case 95:	return E_NOTSUP;
	case 116:	return E_STALE;
	case 122:	return E_DQUOT;
	default:	return E_IO;
	}
}

/* -------- writing a message -------- */

struct wr {
	p9_u8	*p;
	p9_u32	cap;
	p9_u32	len;
	int	over;
};

static void
wr_start(struct wr *w, const struct p9_client *c, struct p9_req *r, p9_u8 type)
{
	w->p = r->tx;
	w->cap = c->msize;
	w->len = 7;
	w->over = 0;
	w->p[4] = type;
	w->p[5] = (p9_u8)r->tag;
	w->p[6] = (p9_u8)(r->tag >> 8);
}

static p9_u8 *
wr_room(struct wr *w, p9_u32 n)
{
	p9_u8 *at;

	if (w->over || n > w->cap - w->len) {
		w->over = 1;
		return 0;
	}
	at = w->p + w->len;
	w->len += n;
	return at;
}

static void
wr16(struct wr *w, p9_u16 v)
{
	p9_u8 *p = wr_room(w, 2);

	if (p != 0) {
		p[0] = (p9_u8)v;
		p[1] = (p9_u8)(v >> 8);
	}
}

static void
wr32(struct wr *w, p9_u32 v)
{
	p9_u8 *p = wr_room(w, 4);

	if (p != 0) {
		p[0] = (p9_u8)v;
		p[1] = (p9_u8)(v >> 8);
		p[2] = (p9_u8)(v >> 16);
		p[3] = (p9_u8)(v >> 24);
	}
}

static void
wr64(struct wr *w, p9_u64 v)
{
	wr32(w, (p9_u32)v);
	wr32(w, (p9_u32)(v >> 32));
}

static void
wrstr(struct wr *w, const char *s)
{
	p9_u32 n = 0;
	p9_u8 *p;

	while (s[n] != 0)
		n++;
	if (n > 0xffff) {
		w->over = 1;
		return;
	}
	wr16(w, (p9_u16)n);
	if ((p = wr_room(w, n)) != 0 && n != 0)
		P9_COPY(s, p, n);
}

/* -------- reading a reply -------- */

struct rd {
	const p9_u8	*p;
	p9_u32		len;
	p9_u32		at;
	int		bad;
};

static const p9_u8 *
rd_take(struct rd *d, p9_u32 n)
{
	const p9_u8 *at;

	if (d->bad || n > d->len - d->at) {
		d->bad = 1;
		return 0;
	}
	at = d->p + d->at;
	d->at += n;
	return at;
}

static p9_u8
rd8(struct rd *d)
{
	const p9_u8 *p = rd_take(d, 1);

	return p != 0 ? p[0] : 0;
}

static p9_u16
rd16(struct rd *d)
{
	const p9_u8 *p = rd_take(d, 2);

	return p != 0 ? (p9_u16)(p[0] | (p[1] << 8)) : 0;
}

static p9_u32
rd32(struct rd *d)
{
	const p9_u8 *p = rd_take(d, 4);

	return p != 0 ? (p9_u32)p[0] | ((p9_u32)p[1] << 8) |
	    ((p9_u32)p[2] << 16) | ((p9_u32)p[3] << 24) : 0;
}

static p9_u64
rd64(struct rd *d)
{
	p9_u64 lo = rd32(d);

	return lo | ((p9_u64)rd32(d) << 32);
}

static void
rdqid(struct rd *d, struct p9_qid *q)
{
	q->type = rd8(d);
	q->version = rd32(d);
	q->path = rd64(d);
}

static void
rdtime(struct rd *d, struct p9_time *t)
{
	t->sec = rd64(d);
	t->nsec = rd64(d);
}

/* -------- a round trip -------- */

/*
 * Finish the message in w, send it, and check the reply is the one expected:
 * right type, right tag, and a size that agrees with the bytes that arrived.
 * On success d is positioned at the reply's first field.
 */
static int
xfer(struct p9_client *c, struct p9_req *r, struct wr *w, struct rd *d)
{
	p9_u32 size;
	p9_u8 type;
	int err;

	if (w->over)
		return E_NAMETOOLONG;
	w->p[0] = (p9_u8)w->len;
	w->p[1] = (p9_u8)(w->len >> 8);
	w->p[2] = (p9_u8)(w->len >> 16);
	w->p[3] = (p9_u8)(w->len >> 24);
	if ((err = c->trans->rpc(c->ctx, r, w->len)) != 0)
		return err;

	d->p = r->rx;
	d->len = r->rxlen;
	d->at = 0;
	d->bad = 0;
	size = rd32(d);
	type = rd8(d);
	if (rd16(d) != r->tag || d->bad || size != r->rxlen)
		return E_IO;
	if (type == RLERROR) {
		size = rd32(d);
		return d->bad ? E_IO : p9_errno(size);
	}
	return type == w->p[4] + 1 ? 0 : E_IO;
}

/* The reply was short: what the message's own fields promised did not fit. */
#define REPLY(d)	((d)->bad ? E_IO : 0)

#define BEGIN(c, r, w, type) do { \
	(r) = (c)->trans->get((c)->ctx); \
	wr_start((w), (c), (r), (type)); \
} while (0)

#define END(c, r)	(c)->trans->put((c)->ctx, (r))

/* A message whose reply is empty. */
static int
simple(struct p9_client *c, struct p9_req *r, struct wr *w)
{
	struct rd d;
	int err = xfer(c, r, w, &d);

	END(c, r);
	return err;
}

/* A message whose reply is a qid. */
static int
with_qid(struct p9_client *c, struct p9_req *r, struct wr *w, struct p9_qid *q)
{
	struct rd d;
	int err = xfer(c, r, w, &d);

	if (err == 0) {
		rdqid(&d, q);
		err = REPLY(&d);
	}
	END(c, r);
	return err;
}

/* -------- messages -------- */

int
p9_version(struct p9_client *c)
{
	static const char dialect[] = P9_DIALECT;
	struct p9_req *r;
	struct wr w;
	struct rd d;
	p9_u32 msize, i;
	p9_u16 n;
	const p9_u8 *s;
	int err;

	BEGIN(c, r, &w, TVERSION);
	/* The one message with no session yet to give its tag a meaning. */
	wr32(&w, c->msize);
	wrstr(&w, dialect);
	err = xfer(c, r, &w, &d);
	if (err == 0) {
		msize = rd32(&d);
		n = rd16(&d);
		s = rd_take(&d, n);
		err = REPLY(&d);
		if (err == 0 && n != sizeof(dialect) - 1)
			err = E_NOTSUP;
		for (i = 0; err == 0 && i < n; i++) {
			if (s[i] != (p9_u8)dialect[i])
				err = E_NOTSUP;
		}
		/* A message must at least hold a directory entry's name. */
		if (err == 0 && (msize < 4096 || msize > c->msize))
			err = E_IO;
		if (err == 0)
			c->msize = msize;
	}
	END(c, r);
	return err;
}

int
p9_attach(struct p9_client *c, p9_u32 fid, const char *aname, struct p9_qid *qid)
{
	struct p9_req *r;
	struct wr w;

	BEGIN(c, r, &w, TATTACH);
	wr32(&w, fid);
	wr32(&w, P9_NOFID);
	wrstr(&w, "root");
	wrstr(&w, aname);
	wr32(&w, 0);
	return with_qid(c, r, &w, qid);
}

int
p9_walk(struct p9_client *c, p9_u32 fid, p9_u32 newfid, const char *name,
	struct p9_qid *qid)
{
	struct p9_req *r;
	struct wr w;
	struct rd d;
	p9_u16 n;
	int err;

	BEGIN(c, r, &w, TWALK);
	wr32(&w, fid);
	wr32(&w, newfid);
	wr16(&w, name != 0 ? 1 : 0);
	if (name != 0)
		wrstr(&w, name);
	err = xfer(c, r, &w, &d);
	if (err == 0) {
		n = rd16(&d);
		if (n != 0 && qid != 0)
			rdqid(&d, qid);
		err = REPLY(&d);
		/* A short walk made no new fid. */
		if (err == 0 && name != 0 && n != 1)
			err = E_NOENT;
	}
	END(c, r);
	return err;
}

int
p9_clunk(struct p9_client *c, p9_u32 fid)
{
	struct p9_req *r;
	struct wr w;

	BEGIN(c, r, &w, TCLUNK);
	wr32(&w, fid);
	return simple(c, r, &w);
}

int
p9_getattr(struct p9_client *c, p9_u32 fid, struct p9_attr *a)
{
	struct p9_req *r;
	struct wr w;
	struct rd d;
	int err;

	BEGIN(c, r, &w, TGETATTR);
	wr32(&w, fid);
	wr64(&w, P9_GETATTR_BASIC);
	err = xfer(c, r, &w, &d);
	if (err == 0) {
		(void)rd64(&d);		/* valid */
		rdqid(&d, &a->qid);
		a->mode = rd32(&d);
		a->uid = rd32(&d);
		a->gid = rd32(&d);
		a->nlink = rd64(&d);
		(void)rd64(&d);		/* rdev: no use without device files */
		a->size = rd64(&d);
		(void)rd64(&d);		/* blksize: the mount's is used */
		a->blocks = rd64(&d);
		rdtime(&d, &a->atime);
		rdtime(&d, &a->mtime);
		rdtime(&d, &a->ctime);
		err = REPLY(&d);
	}
	END(c, r);
	return err;
}

int
p9_setattr(struct p9_client *c, p9_u32 fid, const struct p9_setattr *s)
{
	struct p9_req *r;
	struct wr w;

	BEGIN(c, r, &w, TSETATTR);
	wr32(&w, fid);
	wr32(&w, s->valid);
	wr32(&w, s->mode);
	wr32(&w, s->uid);
	wr32(&w, s->gid);
	wr64(&w, s->size);
	wr64(&w, s->atime.sec);
	wr64(&w, s->atime.nsec);
	wr64(&w, s->mtime.sec);
	wr64(&w, s->mtime.nsec);
	return simple(c, r, &w);
}

int
p9_lopen(struct p9_client *c, p9_u32 fid, p9_u32 flags, struct p9_qid *qid)
{
	struct p9_req *r;
	struct wr w;
	struct p9_qid q;

	BEGIN(c, r, &w, TLOPEN);
	wr32(&w, fid);
	wr32(&w, flags);
	/* The iounit after the qid is not read: P9_MAXIO already fits msize. */
	return with_qid(c, r, &w, qid != 0 ? qid : &q);
}

int
p9_lcreate(struct p9_client *c, p9_u32 fid, const char *name, p9_u32 flags,
	   p9_u32 mode, p9_u32 gid, struct p9_qid *qid)
{
	struct p9_req *r;
	struct wr w;

	BEGIN(c, r, &w, TLCREATE);
	wr32(&w, fid);
	wrstr(&w, name);
	wr32(&w, flags);
	wr32(&w, mode);
	wr32(&w, gid);
	return with_qid(c, r, &w, qid);
}

int
p9_mkdir(struct p9_client *c, p9_u32 dfid, const char *name, p9_u32 mode,
	 p9_u32 gid, struct p9_qid *qid)
{
	struct p9_req *r;
	struct wr w;

	BEGIN(c, r, &w, TMKDIR);
	wr32(&w, dfid);
	wrstr(&w, name);
	wr32(&w, mode);
	wr32(&w, gid);
	return with_qid(c, r, &w, qid);
}

int
p9_symlink(struct p9_client *c, p9_u32 dfid, const char *name, const char *target,
	   p9_u32 gid, struct p9_qid *qid)
{
	struct p9_req *r;
	struct wr w;

	BEGIN(c, r, &w, TSYMLINK);
	wr32(&w, dfid);
	wrstr(&w, name);
	wrstr(&w, target);
	wr32(&w, gid);
	return with_qid(c, r, &w, qid);
}

int
p9_link(struct p9_client *c, p9_u32 dfid, p9_u32 fid, const char *name)
{
	struct p9_req *r;
	struct wr w;

	BEGIN(c, r, &w, TLINK);
	wr32(&w, dfid);
	wr32(&w, fid);
	wrstr(&w, name);
	return simple(c, r, &w);
}

int
p9_unlinkat(struct p9_client *c, p9_u32 dfid, const char *name, p9_u32 flags)
{
	struct p9_req *r;
	struct wr w;

	BEGIN(c, r, &w, TUNLINKAT);
	wr32(&w, dfid);
	wrstr(&w, name);
	wr32(&w, flags);
	return simple(c, r, &w);
}

int
p9_renameat(struct p9_client *c, p9_u32 olddfid, const char *oldname,
	    p9_u32 newdfid, const char *newname)
{
	struct p9_req *r;
	struct wr w;

	BEGIN(c, r, &w, TRENAMEAT);
	wr32(&w, olddfid);
	wrstr(&w, oldname);
	wr32(&w, newdfid);
	wrstr(&w, newname);
	return simple(c, r, &w);
}

int
p9_fsync(struct p9_client *c, p9_u32 fid)
{
	struct p9_req *r;
	struct wr w;

	BEGIN(c, r, &w, TFSYNC);
	wr32(&w, fid);
	wr32(&w, 0);
	return simple(c, r, &w);
}

int
p9_statfs(struct p9_client *c, p9_u32 fid, struct p9_statfs *s)
{
	struct p9_req *r;
	struct wr w;
	struct rd d;
	int err;

	BEGIN(c, r, &w, TSTATFS);
	wr32(&w, fid);
	err = xfer(c, r, &w, &d);
	if (err == 0) {
		(void)rd32(&d);		/* type */
		s->bsize = rd32(&d);
		s->blocks = rd64(&d);
		s->bfree = rd64(&d);
		s->bavail = rd64(&d);
		s->files = rd64(&d);
		s->ffree = rd64(&d);
		s->fsid = rd64(&d);
		s->namelen = rd32(&d);
		err = REPLY(&d);
	}
	END(c, r);
	return err;
}

int
p9_readlink(struct p9_client *c, p9_u32 fid, char *buf, p9_u32 cap, p9_u32 *len)
{
	struct p9_req *r;
	struct wr w;
	struct rd d;
	const p9_u8 *s;
	p9_u16 n;
	int err;

	BEGIN(c, r, &w, TREADLINK);
	wr32(&w, fid);
	err = xfer(c, r, &w, &d);
	if (err == 0) {
		n = rd16(&d);
		s = rd_take(&d, n);
		err = REPLY(&d);
		if (err == 0 && n > cap)
			err = E_NAMETOOLONG;
		if (err == 0) {
			if (n != 0)
				P9_COPY(s, buf, n);
			*len = n;
		}
	}
	END(c, r);
	return err;
}

/* Rread and Rreaddir: count[4] and that many bytes, into the caller's buffer. */
static int
read_common(struct p9_client *c, p9_u8 type, p9_u32 fid, p9_u64 offset,
	    p9_u32 count, void *data, p9_u32 *done)
{
	struct p9_req *r;
	struct wr w;
	struct rd d;
	const p9_u8 *s;
	p9_u32 n;
	int err;

	if (count > P9_MAXIO(c))
		count = P9_MAXIO(c);
	BEGIN(c, r, &w, type);
	wr32(&w, fid);
	wr64(&w, offset);
	wr32(&w, count);
	err = xfer(c, r, &w, &d);
	if (err == 0) {
		n = rd32(&d);
		s = rd_take(&d, n);
		err = REPLY(&d);
		if (err == 0 && n > count)
			err = E_IO;
		if (err == 0) {
			if (n != 0)
				P9_COPY(s, data, n);
			*done = n;
		}
	}
	END(c, r);
	return err;
}

int
p9_read(struct p9_client *c, p9_u32 fid, p9_u64 offset, p9_u32 count, void *data,
	p9_u32 *done)
{
	return read_common(c, TREAD, fid, offset, count, data, done);
}

int
p9_readdir(struct p9_client *c, p9_u32 fid, p9_u64 offset, p9_u32 count, void *buf,
	   p9_u32 *done)
{
	return read_common(c, TREADDIR, fid, offset, count, buf, done);
}

int
p9_write(struct p9_client *c, p9_u32 fid, p9_u64 offset, p9_u32 count,
	 const void *data, p9_u32 *done)
{
	struct p9_req *r;
	struct wr w;
	struct rd d;
	p9_u8 *p;
	int err;

	if (count > P9_MAXIO(c))
		count = P9_MAXIO(c);
	BEGIN(c, r, &w, TWRITE);
	wr32(&w, fid);
	wr64(&w, offset);
	wr32(&w, count);
	if ((p = wr_room(&w, count)) != 0 && count != 0)
		P9_COPY(data, p, count);
	err = xfer(c, r, &w, &d);
	if (err == 0) {
		*done = rd32(&d);
		err = REPLY(&d);
		if (err == 0 && *done > count)
			err = E_IO;
	}
	END(c, r);
	return err;
}

int
p9_dirent_next(const void *buf, p9_u32 len, p9_u32 *pos, struct p9_dirent *e)
{
	struct rd d;

	d.p = buf;
	d.len = len;
	d.at = *pos;
	d.bad = 0;
	if (d.at >= len)
		return 0;
	rdqid(&d, &e->qid);
	e->offset = rd64(&d);
	e->type = rd8(&d);
	e->namelen = rd16(&d);
	e->name = (const char *)rd_take(&d, e->namelen);
	if (d.bad)
		return 0;
	*pos = d.at;
	return 1;
}
