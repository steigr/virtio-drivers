/*
 * Host test of driver/p9.c: every message against bytes written out by hand
 * from the protocol's definition, and replies that lie about their lengths.
 *
 *   make -C tests/host
 */
#include <assert.h>
#include <errno.h>
#include <stdio.h>
#include <string.h>

#include "p9.h"

#define MSIZE	8192

static p9_u8 txbuf[MSIZE], rxbuf[MSIZE];
static struct p9_req req = { txbuf, rxbuf, 0, 3 };
static p9_u32 sent;
static int rpc_err, n_get, n_put;

/* What the fake server answers with, in place of a real one. */
static const p9_u8 *reply;
static p9_u32 reply_len;

static struct p9_req *
t_get(void *ctx)
{
	(void)ctx;
	n_get++;
	return &req;
}

static int
t_rpc(void *ctx, struct p9_req *r, p9_u32 txlen)
{
	(void)ctx;
	sent = txlen;
	if (rpc_err != 0)
		return rpc_err;
	memcpy(r->rx, reply, reply_len);
	r->rxlen = reply_len;
	return 0;
}

static void
t_put(void *ctx, struct p9_req *r)
{
	(void)ctx;
	(void)r;
	n_put++;
}

static const struct p9_transport trans = { t_get, t_rpc, t_put };
static struct p9_client cli = { &trans, 0, MSIZE };

/* A reply: size, type and tag, then the body. */
static p9_u8 rep[MSIZE];
static void
set_reply(p9_u8 type, const void *body, p9_u32 n)
{
	p9_u32 size = 7 + n;

	rep[0] = (p9_u8)size;
	rep[1] = (p9_u8)(size >> 8);
	rep[2] = 0;
	rep[3] = 0;
	rep[4] = type;
	rep[5] = 3;
	rep[6] = 0;
	if (n != 0)
		memcpy(rep + 7, body, n);
	reply = rep;
	reply_len = size;
}

#define SENT_IS(...) do { \
	static const p9_u8 want[] = { __VA_ARGS__ }; \
	assert(sent == sizeof(want)); \
	assert(memcmp(txbuf, want, sizeof(want)) == 0); \
} while (0)

/* Every message must give its request back, however it ended. */
#define BALANCED()	assert(n_get == n_put)

static const p9_u8 qid_bytes[] = {
	0x80,				/* type */
	0x11, 0x22, 0x33, 0x44,		/* version */
	1, 2, 3, 4, 5, 6, 7, 8,		/* path */
};

static void
test_version(void)
{
	static const p9_u8 ok[] = { 0x00, 0x20, 0, 0, 8, 0, '9', 'P', '2', '0', '0', '0', '.', 'L' };
	static const p9_u8 small[] = { 0x00, 0x10, 0, 0, 8, 0, '9', 'P', '2', '0', '0', '0', '.', 'L' };
	static const p9_u8 other[] = { 0x00, 0x20, 0, 0, 6, 0, '9', 'P', '2', '0', '0', '0' };
	static const p9_u8 huge[] = { 0x00, 0, 1, 0, 8, 0, '9', 'P', '2', '0', '0', '0', '.', 'L' };
	static const p9_u8 tiny[] = { 0x00, 1, 0, 0, 8, 0, '9', 'P', '2', '0', '0', '0', '.', 'L' };
	struct p9_client c = cli;

	set_reply(101, ok, sizeof(ok));
	assert(p9_version(&c) == 0);
	assert(c.msize == MSIZE);
	SENT_IS(21, 0, 0, 0, 100, 3, 0, 0x00, 0x20, 0, 0,
	    8, 0, '9', 'P', '2', '0', '0', '0', '.', 'L');

	/* The server may offer less; never more, and not too little to use. */
	c = cli;
	set_reply(101, small, sizeof(small));
	assert(p9_version(&c) == 0);
	assert(c.msize == 4096);
	c = cli;
	set_reply(101, huge, sizeof(huge));
	assert(p9_version(&c) == EIO);
	c = cli;
	set_reply(101, tiny, sizeof(tiny));
	assert(p9_version(&c) == EIO);

	/* A server speaking something else. */
	c = cli;
	set_reply(101, other, sizeof(other));
	assert(p9_version(&c) == 122);
	BALANCED();
}

static void
test_walk_and_attach(void)
{
	struct p9_qid q;
	p9_u8 body[2 + sizeof(qid_bytes)];

	set_reply(105, qid_bytes, sizeof(qid_bytes));
	assert(p9_attach(&cli, 1, "sub", &q) == 0);
	assert(q.type == 0x80 && q.version == 0x44332211);
	assert(q.path == 0x0807060504030201ULL);
	SENT_IS(30, 0, 0, 0, 104, 3, 0,
	    1, 0, 0, 0,  0xff, 0xff, 0xff, 0xff,
	    4, 0, 'r', 'o', 'o', 't',
	    3, 0, 's', 'u', 'b',
	    0, 0, 0, 0);

	body[0] = 1;
	body[1] = 0;
	memcpy(body + 2, qid_bytes, sizeof(qid_bytes));
	set_reply(111, body, sizeof(body));
	assert(p9_walk(&cli, 1, 2, "name", &q) == 0);
	assert(q.path == 0x0807060504030201ULL);
	SENT_IS(23, 0, 0, 0, 110, 3, 0,
	    1, 0, 0, 0, 2, 0, 0, 0, 1, 0, 4, 0, 'n', 'a', 'm', 'e');

	/* A clone has no names and no qids. */
	body[0] = 0;
	set_reply(111, body, 2);
	assert(p9_walk(&cli, 1, 2, 0, 0) == 0);
	SENT_IS(17, 0, 0, 0, 110, 3, 0, 1, 0, 0, 0, 2, 0, 0, 0, 0, 0);

	/* A walk that stops short made no fid: the name is not there. */
	set_reply(111, body, 2);
	assert(p9_walk(&cli, 1, 2, "x", &q) == ENOENT);

	/* Twenty bytes promised, two there. */
	body[0] = 1;
	set_reply(111, body, 2);
	assert(p9_walk(&cli, 1, 2, "x", &q) == EIO);
	BALANCED();
}

static void
test_errors(void)
{
	static const p9_u8 e_notempty[] = { 39, 0, 0, 0 };
	static const p9_u8 e_short[] = { 39 };
	static const p9_u8 big_name[] = { 0 };
	static char name[MSIZE + 10];
	struct p9_req *r = &req;

	/* Rlerror carries Linux's number; callers get IRIX's. */
	set_reply(7, e_notempty, sizeof(e_notempty));
	assert(p9_clunk(&cli, 1) == 93);
	set_reply(7, e_short, sizeof(e_short));
	assert(p9_clunk(&cli, 1) == EIO);

	assert(p9_errno(1) == 1);
	assert(p9_errno(2) == 2);
	assert(p9_errno(13) == 13);
	assert(p9_errno(17) == 17);
	assert(p9_errno(30) == 30);
	assert(p9_errno(36) == 78);
	assert(p9_errno(38) == 89);
	assert(p9_errno(39) == 93);
	assert(p9_errno(40) == 90);
	assert(p9_errno(95) == 122);
	assert(p9_errno(116) == 151);
	assert(p9_errno(9999) == 5);
	assert(p9_errno(0) == 5);

	/* The wrong type of reply, the wrong tag, and a size that does not add up. */
	set_reply(99, 0, 0);
	assert(p9_clunk(&cli, 1) == EIO);
	set_reply(121, 0, 0);
	rep[5] = 9;
	assert(p9_clunk(&cli, 1) == EIO);
	set_reply(121, 0, 0);
	rep[0] = 8;
	assert(p9_clunk(&cli, 1) == EIO);
	reply_len = 3;
	assert(p9_clunk(&cli, 1) == EIO);

	/* The transport's own failure comes through. */
	rpc_err = ETIMEDOUT;
	assert(p9_clunk(&cli, 1) == ETIMEDOUT);
	rpc_err = 0;

	/* A name that cannot fit is refused before it runs off the buffer. */
	memset(name, 'n', sizeof(name) - 1);
	set_reply(77, big_name, 0);
	assert(p9_unlinkat(&cli, 1, name, 0) == 78);
	(void)r;
	BALANCED();
}

static void
put_le64(p9_u8 *p, p9_u64 v)
{
	int i;

	for (i = 0; i < 8; i++)
		p[i] = (p9_u8)(v >> (8 * i));
}

static void
put_le32(p9_u8 *p, p9_u32 v)
{
	int i;

	for (i = 0; i < 4; i++)
		p[i] = (p9_u8)(v >> (8 * i));
}

static void
test_getattr_setattr(void)
{
	p9_u8 b[160];
	struct p9_attr a;
	struct p9_setattr s;
	p9_u8 *p = b;

	put_le64(p, 0x7ff), p += 8;
	memcpy(p, qid_bytes, 13), p += 13;
	put_le32(p, 0100644), p += 4;
	put_le32(p, 7), p += 4;
	put_le32(p, 8), p += 4;
	put_le64(p, 2), p += 8;		/* nlink */
	put_le64(p, 0), p += 8;		/* rdev */
	put_le64(p, 0x123456789aULL), p += 8; /* size, past 32 bits */
	put_le64(p, 4096), p += 8;	/* blksize */
	put_le64(p, 99), p += 8;	/* blocks */
	put_le64(p, 1000), p += 8;
	put_le64(p, 1), p += 8;
	put_le64(p, 2000), p += 8;
	put_le64(p, 2), p += 8;
	put_le64(p, 3000), p += 8;
	put_le64(p, 3), p += 8;
	put_le64(p, 0), p += 8;
	put_le64(p, 0), p += 8;
	put_le64(p, 0), p += 8;
	put_le64(p, 0), p += 8;
	set_reply(25, b, (p9_u32)(p - b));
	assert(p9_getattr(&cli, 5, &a) == 0);
	assert(a.mode == 0100644 && a.uid == 7 && a.gid == 8 && a.nlink == 2);
	assert(a.size == 0x123456789aULL && a.blocks == 99);
	assert(a.atime.sec == 1000 && a.atime.nsec == 1);
	assert(a.mtime.sec == 2000 && a.mtime.nsec == 2);
	assert(a.ctime.sec == 3000 && a.ctime.nsec == 3);
	assert(a.qid.path == 0x0807060504030201ULL);
	SENT_IS(19, 0, 0, 0, 24, 3, 0, 5, 0, 0, 0, 0xff, 0x07, 0, 0, 0, 0, 0, 0);

	/* Cut short anywhere: no use. */
	set_reply(25, b, 40);
	assert(p9_getattr(&cli, 5, &a) == EIO);

	memset(&s, 0, sizeof(s));
	s.valid = P9_SETATTR_SIZE | P9_SETATTR_MODE;
	s.mode = 0600;
	s.size = 0x100000000ULL;
	set_reply(27, 0, 0);
	assert(p9_setattr(&cli, 5, &s) == 0);
	SENT_IS(67, 0, 0, 0, 26, 3, 0,
	    5, 0, 0, 0,  0x09, 0, 0, 0,  0x80, 0x01, 0, 0,
	    0, 0, 0, 0,  0, 0, 0, 0,
	    0, 0, 0, 0, 1, 0, 0, 0,
	    0, 0, 0, 0, 0, 0, 0, 0,  0, 0, 0, 0, 0, 0, 0, 0,
	    0, 0, 0, 0, 0, 0, 0, 0,  0, 0, 0, 0, 0, 0, 0, 0);
	BALANCED();
}

static void
test_io(void)
{
	static p9_u8 b[MSIZE];
	char data[64];
	p9_u32 done;

	/* A read's data lands in the caller's buffer. */
	put_le32(b, 5);
	memcpy(b + 4, "hello", 5);
	set_reply(117, b, 9);
	assert(p9_read(&cli, 7, 0x100000001ULL, 64, data, &done) == 0);
	assert(done == 5 && memcmp(data, "hello", 5) == 0);
	SENT_IS(23, 0, 0, 0, 116, 3, 0, 7, 0, 0, 0,
	    1, 0, 0, 0, 1, 0, 0, 0,  64, 0, 0, 0);

	/* End of file. */
	put_le32(b, 0);
	set_reply(117, b, 4);
	assert(p9_read(&cli, 7, 0, 64, data, &done) == 0 && done == 0);

	/* A server that claims more than was asked for, or than it sent. */
	put_le32(b, 65);
	set_reply(117, b, 69);
	assert(p9_read(&cli, 7, 0, 64, data, &done) == EIO);
	put_le32(b, 60);
	set_reply(117, b, 10);
	assert(p9_read(&cli, 7, 0, 64, data, &done) == EIO);

	/* A request larger than a message is cut to what fits. */
	put_le32(b, 1);
	set_reply(117, b, 5);
	assert(p9_read(&cli, 7, 0, 1000000, data, &done) == 0);
	assert(txbuf[19] == (p9_u8)P9_MAXIO(&cli) && txbuf[20] == (p9_u8)(P9_MAXIO(&cli) >> 8));

	/* A write carries its data after the header. */
	put_le32(b, 3);
	set_reply(119, b, 4);
	assert(p9_write(&cli, 7, 2, 3, "abc", &done) == 0 && done == 3);
	SENT_IS(26, 0, 0, 0, 118, 3, 0, 7, 0, 0, 0,
	    2, 0, 0, 0, 0, 0, 0, 0,  3, 0, 0, 0,  'a', 'b', 'c');

	/* The largest write fits a message exactly. */
	put_le32(b, P9_MAXIO(&cli));
	set_reply(119, b, 4);
	assert(p9_write(&cli, 7, 0, 1000000, b, &done) == 0);
	assert(sent <= MSIZE);
	assert(done == P9_MAXIO(&cli));

	/* A server claiming to have written more than it was given. */
	put_le32(b, 4);
	set_reply(119, b, 4);
	assert(p9_write(&cli, 7, 2, 3, "abc", &done) == EIO);
	BALANCED();
}

static void
test_readdir(void)
{
	static p9_u8 b[256], out[256];
	struct p9_dirent e;
	p9_u32 got, pos = 0, n;
	p9_u8 *p = b + 4;

	memcpy(p, qid_bytes, 13), p += 13;
	put_le64(p, 1), p += 8;
	*p++ = 4;
	*p++ = 1, *p++ = 0, *p++ = '.';
	memcpy(p, qid_bytes, 13), p += 13;
	put_le64(p, 2), p += 8;
	*p++ = 8;
	*p++ = 5, *p++ = 0;
	memcpy(p, "hello", 5), p += 5;
	n = (p9_u32)(p - b - 4);
	put_le32(b, n);
	set_reply(41, b, n + 4);
	assert(p9_readdir(&cli, 9, 0, sizeof(out), out, &got) == 0);
	assert(got == n);

	assert(p9_dirent_next(out, got, &pos, &e) == 1);
	assert(e.namelen == 1 && e.name[0] == '.' && e.offset == 1 && e.type == 4);
	assert(e.qid.path == 0x0807060504030201ULL);
	assert(p9_dirent_next(out, got, &pos, &e) == 1);
	assert(e.namelen == 5 && memcmp(e.name, "hello", 5) == 0 && e.offset == 2 && e.type == 8);
	assert(p9_dirent_next(out, got, &pos, &e) == 0);

	/* An entry that runs off the end is no entry. */
	pos = 0;
	assert(p9_dirent_next(out, 30, &pos, &e) == 1);
	assert(p9_dirent_next(out, 34, &pos, &e) == 0);
	BALANCED();
}

static void
test_others(void)
{
	static p9_u8 b[64];
	static p9_u8 q_and_iounit[17];
	struct p9_qid q;
	struct p9_statfs st;
	char link[8];
	p9_u32 n;

	memcpy(q_and_iounit, qid_bytes, 13);
	set_reply(13, q_and_iounit, sizeof(q_and_iounit));
	assert(p9_lopen(&cli, 3, P9_O_RDWR, &q) == 0);
	SENT_IS(15, 0, 0, 0, 12, 3, 0, 3, 0, 0, 0, 2, 0, 0, 0);
	set_reply(15, q_and_iounit, sizeof(q_and_iounit));
	assert(p9_lcreate(&cli, 3, "f", P9_O_RDWR | P9_O_EXCL, 0644, 5, &q) == 0);
	SENT_IS(26, 0, 0, 0, 14, 3, 0, 3, 0, 0, 0, 1, 0, 'f',
	    0x82, 0, 0, 0,  0xa4, 0x01, 0, 0,  5, 0, 0, 0);
	set_reply(73, qid_bytes, sizeof(qid_bytes));
	assert(p9_mkdir(&cli, 3, "d", 0755, 5, &q) == 0);
	SENT_IS(22, 0, 0, 0, 72, 3, 0, 3, 0, 0, 0, 1, 0, 'd',
	    0xed, 0x01, 0, 0,  5, 0, 0, 0);
	set_reply(17, qid_bytes, sizeof(qid_bytes));
	assert(p9_symlink(&cli, 3, "s", "tt", 5, &q) == 0);
	SENT_IS(22, 0, 0, 0, 16, 3, 0, 3, 0, 0, 0, 1, 0, 's', 2, 0, 't', 't', 5, 0, 0, 0);
	set_reply(71, 0, 0);
	assert(p9_link(&cli, 3, 4, "l") == 0);
	SENT_IS(18, 0, 0, 0, 70, 3, 0, 3, 0, 0, 0, 4, 0, 0, 0, 1, 0, 'l');
	set_reply(77, 0, 0);
	assert(p9_unlinkat(&cli, 3, "u", P9_AT_REMOVEDIR) == 0);
	SENT_IS(18, 0, 0, 0, 76, 3, 0, 3, 0, 0, 0, 1, 0, 'u', 0, 2, 0, 0);
	set_reply(75, 0, 0);
	assert(p9_renameat(&cli, 3, "a", 4, "b") == 0);
	SENT_IS(21, 0, 0, 0, 74, 3, 0, 3, 0, 0, 0, 1, 0, 'a', 4, 0, 0, 0, 1, 0, 'b');
	set_reply(51, 0, 0);
	assert(p9_fsync(&cli, 3) == 0);
	SENT_IS(15, 0, 0, 0, 50, 3, 0, 3, 0, 0, 0, 0, 0, 0, 0);

	b[0] = 3, b[1] = 0;
	memcpy(b + 2, "tgt", 3);
	set_reply(23, b, 5);
	assert(p9_readlink(&cli, 3, link, sizeof(link), &n) == 0);
	assert(n == 3 && memcmp(link, "tgt", 3) == 0);
	assert(p9_readlink(&cli, 3, link, 2, &n) == 78);

	memset(b, 0, sizeof(b));
	put_le32(b, 0x01021997);
	put_le32(b + 4, 4096);
	put_le64(b + 8, 100);
	put_le64(b + 16, 50);
	put_le64(b + 24, 40);
	put_le64(b + 32, 1000);
	put_le64(b + 40, 500);
	put_le32(b + 56, 255);
	set_reply(9, b, 60);
	assert(p9_statfs(&cli, 3, &st) == 0);
	assert(st.bsize == 4096 && st.blocks == 100 && st.bfree == 50 && st.bavail == 40);
	assert(st.files == 1000 && st.ffree == 500 && st.namelen == 255);
	BALANCED();
}

int
main(void)
{
	test_version();
	test_walk_and_attach();
	test_errors();
	test_getattr_setattr();
	test_io();
	test_readdir();
	test_others();
	puts("test_p9: ok");
	return 0;
}
