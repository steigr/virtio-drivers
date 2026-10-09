/*
 * 9P2000.L as C calls: one function per message, each building its request in the device's
 * request buffer, sending it and taking its reply apart, with the mount's channel held only for
 * that long. See p9fs.h for why the channel is never held across anything else.
 *
 * The protocol is http://ericvh.github.io/9p-rfc/rfc9p2000.html with the Linux ("dot-L")
 * messages of QEMU's server (hw/9pfs/9p.c) and Linux's net/9p; the set of messages is the one
 * elliotnunn/classicvirtio's 9p.c uses for the classic Mac OS File Manager, plus link, symlink
 * and readlink. 9P is little-endian; every field is put together a byte at a time.
 */

#include "p9fs.h"
#include "p9_transport.h"

#include <sys/errno.h>
#include <sys/malloc.h>
#include <sys/proc.h>

enum {
    P9_TLERROR = 6,     P9_RLERROR,
    P9_TSTATFS = 8,     P9_RSTATFS,
    P9_TLOPEN = 12,     P9_RLOPEN,
    P9_TLCREATE = 14,   P9_RLCREATE,
    P9_TSYMLINK = 16,   P9_RSYMLINK,
    P9_TREADLINK = 22,  P9_RREADLINK,
    P9_TGETATTR = 24,   P9_RGETATTR,
    P9_TSETATTR = 26,   P9_RSETATTR,
    P9_TREADDIR = 40,   P9_RREADDIR,
    P9_TFSYNC = 50,     P9_RFSYNC,
    P9_TLINK = 70,      P9_RLINK,
    P9_TMKDIR = 72,     P9_RMKDIR,
    P9_TRENAMEAT = 74,  P9_RRENAMEAT,
    P9_TUNLINKAT = 76,  P9_RUNLINKAT,
    P9_TVERSION = 100,  P9_RVERSION,
    P9_TATTACH = 104,   P9_RATTACH,
    P9_TWALK = 110,     P9_RWALK,
    P9_TREAD = 116,     P9_RREAD,
    P9_TWRITE = 118,    P9_RWRITE,
    P9_TCLUNK = 120,    P9_RCLUNK
};

#define P9_NOTAG            0xffff
#define P9_TAG              1           /* only ever one message with the device */
#define P9_HDRSZ            7           /* size[4] type[1] tag[2] */

/* Where a message is being put together or taken apart. A put past the end, or a get past it,
 * is remembered and fails the call rather than corrupting anything. */
struct p9msg {
    u_int8_t   *buf;
    u_int32_t   pos;
    u_int32_t   end;
    int         bad;
};

/* ------------------------------------------------------------------------------------------ */
/* errors                                                                                      */
/* ------------------------------------------------------------------------------------------ */

/* An Rlerror carries a Linux errno. 1 to 34 are the same numbers on Darwin but for EAGAIN (11
 * on Linux, 35 here); above that they differ. What has no equivalent is EIO. */
int
p9_errno(u_int32_t e)
{
    switch (e) {
    case 11:    return EAGAIN;
    case 35:    return EDEADLK;
    case 36:    return ENAMETOOLONG;
    case 37:    return ENOLCK;
    case 38:    return ENOSYS;
    case 39:    return ENOTEMPTY;
    case 40:    return ELOOP;
    case 75:    return EOVERFLOW;
    case 90:    return EMSGSIZE;
    case 95:    return EOPNOTSUPP;
    case 110:   return ETIMEDOUT;
    case 116:   return ESTALE;
    case 122:   return EDQUOT;
    default:
        if (e >= 1 && e <= 34)
            return (int) e;
        return EIO;
    }
}

/* ------------------------------------------------------------------------------------------ */
/* fids                                                                                        */
/* ------------------------------------------------------------------------------------------ */

u_int32_t
p9_fid_alloc(struct p9mount *pm)
{
    u_int32_t i, fid;

    for (i = 0; i < P9_MAXFIDS; i++) {
        fid = (pm->pm_fidnext + i) % P9_MAXFIDS;
        if (!(pm->pm_fidmap[fid >> 3] & (1 << (fid & 7)))) {
            pm->pm_fidmap[fid >> 3] |= (1 << (fid & 7));
            pm->pm_fidnext = (fid + 1) % P9_MAXFIDS;
            return fid;
        }
    }
    printf("9p: out of fids\n");
    return P9_NOFID;
}

void
p9_fid_free(struct p9mount *pm, u_int32_t fid)
{
    if (fid < P9_MAXFIDS)
        pm->pm_fidmap[fid >> 3] &= ~(1 << (fid & 7));
}

/* ------------------------------------------------------------------------------------------ */
/* marshalling                                                                                 */
/* ------------------------------------------------------------------------------------------ */

static void
pu8(struct p9msg *m, u_int8_t v)
{
    if (m->pos + 1 > m->end) {
        m->bad = 1;
        return;
    }
    m->buf[m->pos++] = v;
}

static void
pu16(struct p9msg *m, u_int16_t v)
{
    pu8(m, (u_int8_t) v);
    pu8(m, (u_int8_t) (v >> 8));
}

static void
pu32(struct p9msg *m, u_int32_t v)
{
    pu16(m, (u_int16_t) v);
    pu16(m, (u_int16_t) (v >> 16));
}

static void
pu64(struct p9msg *m, u_int64_t v)
{
    pu32(m, (u_int32_t) v);
    pu32(m, (u_int32_t) (v >> 32));
}

static void
pstr(struct p9msg *m, const char *s, int len)
{
    if (len < 0 || len > 0xffff || m->pos + 2 + (u_int32_t) len > m->end) {
        m->bad = 1;
        return;
    }
    pu16(m, (u_int16_t) len);
    bcopy(s, m->buf + m->pos, len);
    m->pos += len;
}

static u_int8_t
gu8(struct p9msg *m)
{
    if (m->pos + 1 > m->end) {
        m->bad = 1;
        return 0;
    }
    return m->buf[m->pos++];
}

static u_int16_t
gu16(struct p9msg *m)
{
    u_int16_t lo = gu8(m);
    return lo | ((u_int16_t) gu8(m) << 8);
}

static u_int32_t
gu32(struct p9msg *m)
{
    u_int32_t lo = gu16(m);
    return lo | ((u_int32_t) gu16(m) << 16);
}

static u_int64_t
gu64(struct p9msg *m)
{
    u_int64_t lo = gu32(m);
    return lo | ((u_int64_t) gu32(m) << 32);
}

static void
gqid(struct p9msg *m, struct p9_qid *qid)
{
    struct p9_qid q;

    q.type = gu8(m);
    q.version = gu32(m);
    q.path = gu64(m);
    if (qid)
        *qid = q;
}

/* A string in place: *s points into the reply, not NUL-terminated. */
static u_int16_t
gstr(struct p9msg *m, char **s)
{
    u_int16_t len = gu16(m);

    if (m->bad || m->pos + len > m->end) {
        m->bad = 1;
        return 0;
    }
    *s = (char *) m->buf + m->pos;
    m->pos += len;
    return len;
}

/* ------------------------------------------------------------------------------------------ */
/* the channel                                                                                 */
/* ------------------------------------------------------------------------------------------ */

/* Takes the channel and starts a request of the given type in the request buffer. */
static int
p9_begin(struct p9mount *pm, struct p9msg *t, u_int8_t type)
{
    while (pm->pm_chan) {
        pm->pm_chanwant = 1;
        P9_CHANSLEEP(pm);
    }
    if (pm->pm_dead)
        return ENXIO;
    pm->pm_chan = 1;
    t->buf = pm->pm_tbuf;
    t->pos = P9_HDRSZ;
    t->end = pm->pm_msize;
    t->bad = 0;
    t->buf[4] = type;
    return 0;
}

static void
p9_end(struct p9mount *pm)
{
    pm->pm_chan = 0;
    if (pm->pm_chanwant) {
        pm->pm_chanwant = 0;
        wakeup((caddr_t) &pm->pm_chan);
    }
}

/* Sends the request and checks the reply's header; r is left at its first field. An Rlerror
 * becomes its errno. */
static int
p9_send(struct p9mount *pm, struct p9msg *t, u_int8_t rtype, struct p9msg *r)
{
    struct p9msg h;
    u_int32_t rlen, size;
    u_int16_t tag;
    u_int8_t type;
    int error;

    if (t->bad)
        return ENAMETOOLONG;    /* the only thing that does not fit is a long name */
    tag = (t->buf[4] == P9_TVERSION) ? P9_NOTAG : P9_TAG;
    h.buf = t->buf;
    h.pos = 0;
    h.end = P9_HDRSZ;
    h.bad = 0;
    pu32(&h, t->pos);
    h.pos = 5;
    pu16(&h, tag);

    error = p9t_rpc(pm->pm_dev, t->pos, &rlen);
    if (error) {
        if (error == ENXIO && !pm->pm_dead) {
            printf("9p: the device has gone\n");
            pm->pm_dead = 1;
        }
        return error;
    }

    r->buf = pm->pm_rbuf;
    r->pos = 0;
    r->end = rlen;
    r->bad = 0;
    size = gu32(r);
    type = gu8(r);
    (void) gu16(r);
    if (r->bad || size < P9_HDRSZ || size > rlen) {
        printf("9p: a reply of %u bytes in %u\n", size, rlen);
        return EIO;
    }
    r->end = size;
    if (type == P9_RLERROR) {
        u_int32_t e = gu32(r);
        return r->bad ? EIO : p9_errno(e);
    }
    if (type != rtype) {
        printf("9p: reply type %u to request type %u\n", type, t->buf[4]);
        return EIO;
    }
    return 0;
}

/* The fid a call works on, read now that the channel is held. */
static int
p9_fid(u_int32_t *fidp, u_int32_t *fid)
{
    *fid = *fidp;
    return (*fid == P9_NOFID) ? EBADF : 0;
}

#define P9_CHECK(r)     ((r)->bad ? EIO : 0)

/* ------------------------------------------------------------------------------------------ */
/* the messages                                                                                */
/* ------------------------------------------------------------------------------------------ */

/* size[4] Tversion tag[2] msize[4] version[s] / Rversion msize[4] version[s] */
int
p9_version(struct p9mount *pm)
{
    struct p9msg t, r;
    char *v;
    u_int32_t msize;
    u_int16_t vlen;
    int error;

    if ((error = p9_begin(pm, &t, P9_TVERSION)))
        return error;
    pu32(&t, pm->pm_msize);
    pstr(&t, "9P2000.L", 8);
    error = p9_send(pm, &t, P9_RVERSION, &r);
    if (!error) {
        msize = gu32(&r);
        vlen = gstr(&r, &v);
        error = P9_CHECK(&r);
        if (!error && (vlen != 8 || bcmp(v, "9P2000.L", 8) != 0)) {
            printf("9p: the server does not speak 9P2000.L\n");
            error = EPROTONOSUPPORT;
        }
        if (!error && msize < 4096)
            error = EIO;
        if (!error && msize < pm->pm_msize)
            pm->pm_msize = msize;
    }
    p9_end(pm);
    pm->pm_iosize = pm->pm_msize - P9_IOHDRSZ;
    return error;
}

/* size[4] Tattach tag[2] fid[4] afid[4] uname[s] aname[s] n_uname[4] / Rattach qid[13] */
int
p9_attach(struct p9mount *pm, u_int32_t fid, struct p9_qid *qid)
{
    struct p9msg t, r;
    int error;

    if ((error = p9_begin(pm, &t, P9_TATTACH)))
        return error;
    pu32(&t, fid);
    pu32(&t, P9_NOFID);
    pstr(&t, pm->pm_args.uname, strlen(pm->pm_args.uname));
    pstr(&t, pm->pm_args.aname, strlen(pm->pm_args.aname));
    pu32(&t, pm->pm_args.n_uname);
    error = p9_send(pm, &t, P9_RATTACH, &r);
    if (!error) {
        gqid(&r, qid);
        error = P9_CHECK(&r);
    }
    p9_end(pm);
    return error;
}

/* size[4] Twalk tag[2] fid[4] newfid[4] nwname[2] nwname*(wname[s]) / Rwalk nwqid[2]
 * nwqid*(qid[13]). One name or none (a clone). newfid exists afterwards only if this succeeds;
 * if it fails, newfid is given back to the allocator here. */
int
p9_walk(struct p9mount *pm, u_int32_t *fidp, u_int32_t newfid, const char *name, int namelen,
        struct p9_qid *qid)
{
    struct p9msg t, r;
    u_int32_t fid;
    u_int16_t nwqid;
    int error;

    if ((error = p9_begin(pm, &t, P9_TWALK)))
        goto out;
    if ((error = p9_fid(fidp, &fid)))
        goto end;
    pu32(&t, fid);
    pu32(&t, newfid);
    if (name) {
        pu16(&t, 1);
        pstr(&t, name, namelen);
    } else
        pu16(&t, 0);
    error = p9_send(pm, &t, P9_RWALK, &r);
    if (!error) {
        nwqid = gu16(&r);
        if (name && nwqid < 1)
            error = ENOENT;
        else if (name)
            gqid(&r, qid);
        if (!error)
            error = P9_CHECK(&r);
    }
end:
    p9_end(pm);
out:
    if (error)
        p9_fid_free(pm, newfid);
    return error;
}

/* size[4] Tclunk tag[2] fid[4] / Rclunk. The fid is free afterwards whatever the reply. */
int
p9_clunk(struct p9mount *pm, u_int32_t fid)
{
    struct p9msg t, r;
    int error;

    if (fid == P9_NOFID)
        return 0;
    if (!(error = p9_begin(pm, &t, P9_TCLUNK))) {
        pu32(&t, fid);
        error = p9_send(pm, &t, P9_RCLUNK, &r);
        p9_end(pm);
    }
    p9_fid_free(pm, fid);
    return error;
}

/* size[4] Tlopen tag[2] fid[4] flags[4] / Rlopen qid[13] iounit[4] */
int
p9_lopen(struct p9mount *pm, u_int32_t *fidp, u_int32_t flags)
{
    struct p9msg t, r;
    u_int32_t fid;
    int error;

    if ((error = p9_begin(pm, &t, P9_TLOPEN)))
        return error;
    if (!(error = p9_fid(fidp, &fid))) {
        pu32(&t, fid);
        pu32(&t, flags);
        error = p9_send(pm, &t, P9_RLOPEN, &r);
    }
    p9_end(pm);
    return error;
}

/* size[4] Tlcreate tag[2] fid[4] name[s] flags[4] mode[4] gid[4] / Rlcreate qid[13] iounit[4].
 * The fid, a directory's, is the new file's afterwards, opened. */
int
p9_lcreate(struct p9mount *pm, u_int32_t *fidp, const char *name, int namelen, u_int32_t flags,
           u_int32_t mode, u_int32_t gid, struct p9_qid *qid)
{
    struct p9msg t, r;
    u_int32_t fid;
    int error;

    if ((error = p9_begin(pm, &t, P9_TLCREATE)))
        return error;
    if (!(error = p9_fid(fidp, &fid))) {
        pu32(&t, fid);
        pstr(&t, name, namelen);
        pu32(&t, flags);
        pu32(&t, mode);
        pu32(&t, gid);
        error = p9_send(pm, &t, P9_RLCREATE, &r);
        if (!error) {
            gqid(&r, qid);
            error = P9_CHECK(&r);
        }
    }
    p9_end(pm);
    return error;
}

/* size[4] Tgetattr tag[2] fid[4] request_mask[8] / Rgetattr valid[8] qid[13] mode[4] uid[4]
 * gid[4] nlink[8] rdev[8] size[8] blksize[8] blocks[8] atime_sec[8] atime_nsec[8] mtime_sec[8]
 * mtime_nsec[8] ctime_sec[8] ctime_nsec[8] btime_sec[8] btime_nsec[8] gen[8] data_version[8] */
int
p9_getattr(struct p9mount *pm, u_int32_t *fidp, struct p9_attr *a)
{
    struct p9msg t, r;
    u_int32_t fid;
    int error;

    if ((error = p9_begin(pm, &t, P9_TGETATTR)))
        return error;
    if (!(error = p9_fid(fidp, &fid))) {
        pu32(&t, fid);
        pu64(&t, P9_GETATTR_BASIC);
        error = p9_send(pm, &t, P9_RGETATTR, &r);
        if (!error) {
            (void) gu64(&r);
            gqid(&r, &a->qid);
            a->mode = gu32(&r);
            a->uid = gu32(&r);
            a->gid = gu32(&r);
            a->nlink = gu64(&r);
            a->rdev = gu64(&r);
            a->size = gu64(&r);
            a->blksize = gu64(&r);
            a->blocks = gu64(&r);
            a->atime_sec = gu64(&r);
            a->atime_nsec = gu64(&r);
            a->mtime_sec = gu64(&r);
            a->mtime_nsec = gu64(&r);
            a->ctime_sec = gu64(&r);
            a->ctime_nsec = gu64(&r);
            error = P9_CHECK(&r);
        }
    }
    p9_end(pm);
    return error;
}

/* size[4] Tsetattr tag[2] fid[4] valid[4] mode[4] uid[4] gid[4] size[8] atime_sec[8]
 * atime_nsec[8] mtime_sec[8] mtime_nsec[8] / Rsetattr */
int
p9_setattr(struct p9mount *pm, u_int32_t *fidp, struct p9_setattr *sa)
{
    struct p9msg t, r;
    u_int32_t fid;
    int error;

    if ((error = p9_begin(pm, &t, P9_TSETATTR)))
        return error;
    if (!(error = p9_fid(fidp, &fid))) {
        pu32(&t, fid);
        pu32(&t, sa->valid);
        pu32(&t, sa->mode);
        pu32(&t, sa->uid);
        pu32(&t, sa->gid);
        pu64(&t, sa->size);
        pu64(&t, sa->atime_sec);
        pu64(&t, sa->atime_nsec);
        pu64(&t, sa->mtime_sec);
        pu64(&t, sa->mtime_nsec);
        error = p9_send(pm, &t, P9_RSETATTR, &r);
    }
    p9_end(pm);
    return error;
}

/* Treadaround Tread and Treaddir, whose replies are both count[4] data[count]. */
static int
p9_readish(struct p9mount *pm, u_int8_t type, u_int32_t *fidp, u_int64_t offset, u_int32_t count,
           void *buf, u_int32_t *got)
{
    struct p9msg t, r;
    u_int32_t fid, n;
    int error;

    *got = 0;
    if (count > pm->pm_iosize)
        count = pm->pm_iosize;
    if ((error = p9_begin(pm, &t, type)))
        return error;
    if (!(error = p9_fid(fidp, &fid))) {
        pu32(&t, fid);
        pu64(&t, offset);
        pu32(&t, count);
        error = p9_send(pm, &t, type + 1, &r);
        if (!error) {
            n = gu32(&r);
            if (r.bad || n > count || r.pos + n > r.end)
                error = EIO;
            else {
                bcopy(r.buf + r.pos, buf, n);
                *got = n;
            }
        }
    }
    p9_end(pm);
    return error;
}

/* size[4] Tread tag[2] fid[4] offset[8] count[4] / Rread count[4] data[count]. At most
 * pm_iosize bytes; fewer means the end of the file. buf must not fault. */
int
p9_read(struct p9mount *pm, u_int32_t *fidp, u_int64_t offset, u_int32_t count, void *buf,
        u_int32_t *got)
{
    return p9_readish(pm, P9_TREAD, fidp, offset, count, buf, got);
}

/* size[4] Treaddir tag[2] fid[4] offset[8] count[4] / Rreaddir count[4] data[count], the data
 * being qid[13] offset[8] type[1] name[s] per entry. None means the end. */
int
p9_readdir(struct p9mount *pm, u_int32_t *fidp, u_int64_t offset, u_int32_t count, void *buf,
           u_int32_t *got)
{
    return p9_readish(pm, P9_TREADDIR, fidp, offset, count, buf, got);
}

/* size[4] Twrite tag[2] fid[4] offset[8] count[4] data[count] / Rwrite count[4]. At most
 * pm_iosize bytes. buf must not fault. */
int
p9_write(struct p9mount *pm, u_int32_t *fidp, u_int64_t offset, u_int32_t count, const void *buf,
         u_int32_t *done)
{
    struct p9msg t, r;
    u_int32_t fid, n;
    int error;

    *done = 0;
    if (count > pm->pm_iosize)
        count = pm->pm_iosize;
    if ((error = p9_begin(pm, &t, P9_TWRITE)))
        return error;
    if (!(error = p9_fid(fidp, &fid))) {
        pu32(&t, fid);
        pu64(&t, offset);
        pu32(&t, count);
        if (t.pos + count > t.end)
            t.bad = 1;
        else {
            bcopy(buf, t.buf + t.pos, count);
            t.pos += count;
        }
        error = p9_send(pm, &t, P9_RWRITE, &r);
        if (!error) {
            n = gu32(&r);
            if (r.bad || n > count)
                error = EIO;
            else
                *done = n;
        }
    }
    p9_end(pm);
    return error;
}

/* size[4] Tmkdir tag[2] dfid[4] name[s] mode[4] gid[4] / Rmkdir qid[13] */
int
p9_mkdir(struct p9mount *pm, u_int32_t *dfidp, const char *name, int namelen, u_int32_t mode,
         u_int32_t gid, struct p9_qid *qid)
{
    struct p9msg t, r;
    u_int32_t fid;
    int error;

    if ((error = p9_begin(pm, &t, P9_TMKDIR)))
        return error;
    if (!(error = p9_fid(dfidp, &fid))) {
        pu32(&t, fid);
        pstr(&t, name, namelen);
        pu32(&t, mode);
        pu32(&t, gid);
        error = p9_send(pm, &t, P9_RMKDIR, &r);
        if (!error) {
            gqid(&r, qid);
            error = P9_CHECK(&r);
        }
    }
    p9_end(pm);
    return error;
}

/* size[4] Tsymlink tag[2] fid[4] name[s] symtgt[s] gid[4] / Rsymlink qid[13] */
int
p9_symlink(struct p9mount *pm, u_int32_t *dfidp, const char *name, int namelen, const char *target,
           u_int32_t gid, struct p9_qid *qid)
{
    struct p9msg t, r;
    u_int32_t fid;
    int error;

    if ((error = p9_begin(pm, &t, P9_TSYMLINK)))
        return error;
    if (!(error = p9_fid(dfidp, &fid))) {
        pu32(&t, fid);
        pstr(&t, name, namelen);
        pstr(&t, target, strlen(target));
        pu32(&t, gid);
        error = p9_send(pm, &t, P9_RSYMLINK, &r);
        if (!error) {
            gqid(&r, qid);
            error = P9_CHECK(&r);
        }
    }
    p9_end(pm);
    return error;
}

/* size[4] Tlink tag[2] dfid[4] fid[4] name[s] / Rlink */
int
p9_link(struct p9mount *pm, u_int32_t *dfidp, u_int32_t *fidp, const char *name, int namelen)
{
    struct p9msg t, r;
    u_int32_t dfid, fid;
    int error;

    if ((error = p9_begin(pm, &t, P9_TLINK)))
        return error;
    if (!(error = p9_fid(dfidp, &dfid)) && !(error = p9_fid(fidp, &fid))) {
        pu32(&t, dfid);
        pu32(&t, fid);
        pstr(&t, name, namelen);
        error = p9_send(pm, &t, P9_RLINK, &r);
    }
    p9_end(pm);
    return error;
}

/* size[4] Treadlink tag[2] fid[4] / Rreadlink target[s]. Copies at most bufsize bytes, not
 * NUL-terminated. */
int
p9_readlink(struct p9mount *pm, u_int32_t *fidp, char *buf, u_int32_t bufsize, u_int32_t *len)
{
    struct p9msg t, r;
    u_int32_t fid;
    char *s;
    u_int16_t n;
    int error;

    *len = 0;
    if ((error = p9_begin(pm, &t, P9_TREADLINK)))
        return error;
    if (!(error = p9_fid(fidp, &fid))) {
        pu32(&t, fid);
        error = p9_send(pm, &t, P9_RREADLINK, &r);
        if (!error) {
            n = gstr(&r, &s);
            if (!(error = P9_CHECK(&r))) {
                if (n > bufsize)
                    n = bufsize;
                bcopy(s, buf, n);
                *len = n;
            }
        }
    }
    p9_end(pm);
    return error;
}

/* size[4] Tunlinkat tag[2] dirfd[4] name[s] flags[4] / Runlinkat */
int
p9_unlinkat(struct p9mount *pm, u_int32_t *dfidp, const char *name, int namelen, u_int32_t flags)
{
    struct p9msg t, r;
    u_int32_t fid;
    int error;

    if ((error = p9_begin(pm, &t, P9_TUNLINKAT)))
        return error;
    if (!(error = p9_fid(dfidp, &fid))) {
        pu32(&t, fid);
        pstr(&t, name, namelen);
        pu32(&t, flags);
        error = p9_send(pm, &t, P9_RUNLINKAT, &r);
    }
    p9_end(pm);
    return error;
}

/* size[4] Trenameat tag[2] olddirfid[4] oldname[s] newdirfid[4] newname[s] / Rrenameat */
int
p9_renameat(struct p9mount *pm, u_int32_t *odfidp, const char *oname, int onamelen,
            u_int32_t *ndfidp, const char *nname, int nnamelen)
{
    struct p9msg t, r;
    u_int32_t ofid, nfid;
    int error;

    if ((error = p9_begin(pm, &t, P9_TRENAMEAT)))
        return error;
    if (!(error = p9_fid(odfidp, &ofid)) && !(error = p9_fid(ndfidp, &nfid))) {
        pu32(&t, ofid);
        pstr(&t, oname, onamelen);
        pu32(&t, nfid);
        pstr(&t, nname, nnamelen);
        error = p9_send(pm, &t, P9_RRENAMEAT, &r);
    }
    p9_end(pm);
    return error;
}

/* size[4] Tstatfs tag[2] fid[4] / Rstatfs type[4] bsize[4] blocks[8] bfree[8] bavail[8]
 * files[8] ffree[8] fsid[8] namelen[4] */
int
p9_statfs(struct p9mount *pm, u_int32_t *fidp, struct p9_statfs *sf)
{
    struct p9msg t, r;
    u_int32_t fid;
    int error;

    if ((error = p9_begin(pm, &t, P9_TSTATFS)))
        return error;
    if (!(error = p9_fid(fidp, &fid))) {
        pu32(&t, fid);
        error = p9_send(pm, &t, P9_RSTATFS, &r);
        if (!error) {
            sf->type = gu32(&r);
            sf->bsize = gu32(&r);
            sf->blocks = gu64(&r);
            sf->bfree = gu64(&r);
            sf->bavail = gu64(&r);
            sf->files = gu64(&r);
            sf->ffree = gu64(&r);
            sf->fsid = gu64(&r);
            sf->namelen = gu32(&r);
            error = P9_CHECK(&r);
        }
    }
    p9_end(pm);
    return error;
}

/* size[4] Tfsync tag[2] fid[4] datasync[4] / Rfsync */
int
p9_fsync(struct p9mount *pm, u_int32_t *fidp)
{
    struct p9msg t, r;
    u_int32_t fid;
    int error;

    if ((error = p9_begin(pm, &t, P9_TFSYNC)))
        return error;
    if (!(error = p9_fid(fidp, &fid))) {
        pu32(&t, fid);
        pu32(&t, 0);
        error = p9_send(pm, &t, P9_RFSYNC, &r);
    }
    p9_end(pm);
    return error;
}
