/*
 * libvsock: see vsock.h. Plain C for GCC 2.95 (10.1's Developer Tools): declarations first, no
 * variadic macros. IOConnectMethod* are 10.0's calls; later releases deprecate them but still
 * have them -- for 32-bit processes. A 64-bit one (the Intel agent's x86_64 slice, 10.6 on) has
 * only 10.5's IOConnectCall*Method, so the calls go through vs_scalar and vs_struct, which take
 * whichever the process has. The kernel side is the same either way: the driver's method table is
 * the old kind, which IOUserClient translates for both.
 */

#include <errno.h>
#include <stdarg.h>
#include <string.h>
#include <sys/time.h>
#include <sys/types.h>
#include <unistd.h>
#include <mach/mach.h>
#include <IOKit/IOKitLib.h>
#include <IOKit/IOReturn.h>

#include "vsock.h"

static io_connect_t gConn = 0;

#if defined(__LP64__)
/* The guest agent's x86_64 slice links IOKit weakly: Tiger runs that slice on a 64-bit processor
 * and has no 64-bit IOKit, so there these are NULL, and there is no vsock. */
extern kern_return_t IOMasterPort(mach_port_t, mach_port_t *) __attribute__((weak_import));
#endif

/* A scalar call: the selector, then nin ints and nout int pointers, as IOConnectMethodScalarIScalarO
 * takes them. */
static kern_return_t vs_scalar(unsigned int sel, int nin, int nout, ...)
{
    int in[4], *out[4], k;
    va_list ap;

    va_start(ap, nout);
    for (k = 0; k < nin; k++)
        in[k] = va_arg(ap, int);
    for (k = 0; k < nout; k++)
        out[k] = va_arg(ap, int *);
    va_end(ap);
#if defined(__LP64__)
    {
        uint64_t i64[4], o64[4];
        uint32_t n = (uint32_t) nout;
        kern_return_t kr;

        for (k = 0; k < nin; k++)
            i64[k] = (uint64_t) (uint32_t) in[k];
        kr = IOConnectCallScalarMethod(gConn, sel, i64, (uint32_t) nin, o64, &n);
        for (k = 0; k < nout && k < (int) n; k++)
            *out[k] = (int) o64[k];
        return kr;
    }
#else
    {
        /* 10.0's call reads its arguments as words, the inputs and then the outputs, and no more
         * of them than the counts say: the rest of these are passed and never looked at. */
        long w[8];

        for (k = 0; k < 8; k++)
            w[k] = 0;
        for (k = 0; k < nin; k++)
            w[k] = in[k];
        for (k = 0; k < nout; k++)
            w[nin + k] = (long) out[k];
        return IOConnectMethodScalarIScalarO(gConn, sel, nin, nout, w[0], w[1], w[2], w[3], w[4], w[5], w[6], w[7]);
    }
#endif
}

/* A structure call: in and out, whole. */
static kern_return_t vs_struct(unsigned int sel, void *in, unsigned long inSize, void *out, unsigned long outSize)
{
#if defined(__LP64__)
    size_t n = outSize;

    return IOConnectCallStructMethod(gConn, sel, in, inSize, out, &n);
#else
    IOByteCount n = outSize;

    return IOConnectMethodStructureIStructureO(gConn, sel, inSize, &n, in, out);
#endif
}

/* A call that would wait is retried: at once for a moment, then in 5 ms steps (select, because 10.1's
 * usleep returns void). A signal ends the sleep early, and then the caller sees EINTR
 * if its handler returned. */
static int wait_a_little(int *spins)
{
    struct timeval tv;

    if ((*spins)++ < 3)
        return 0;
    tv.tv_sec = 0;
    tv.tv_usec = 5000;
    if (select(0, 0, 0, 0, &tv) < 0 && errno == EINTR)
        return -1;
    return 0;
}

static int map_error(kern_return_t kr)
{
    switch (kr) {
    case kIOReturnSuccess:          return 0;
    case kIOReturnNotReady:         return EAGAIN;
    case kIOReturnAborted:          return EINTR;
    case kIOReturnTimeout:          return ETIMEDOUT;
    case kIOReturnNotFound:         return ECONNREFUSED;
    case kIOReturnIOError:          return ECONNRESET;
    case kIOReturnNotWritable:      return EPIPE;
    case kIOReturnNotOpen:          return EBADF;
    case kIOReturnBadArgument:      return EINVAL;
    case kIOReturnNoResources:      return EMFILE;
    case kIOReturnNoMemory:         return ENOMEM;
    case kIOReturnExclusiveAccess:  return EADDRINUSE;
    case kIOReturnOffline:
    case kIOReturnNotAttached:      return ENETDOWN;
    case kIOReturnUnsupported:      return ENOSYS;
    default:                        return EIO;
    }
}

static int fail(kern_return_t kr)
{
    errno = map_error(kr);
    return -1;
}

int vsock_open(void)
{
    mach_port_t master;
    io_iterator_t iter;
    io_service_t service;
    kern_return_t kr;

    if (gConn)
        return 0;
#if defined(__LP64__)
    if (IOMasterPort == NULL) {
        errno = ENXIO;
        return -1;
    }
#endif
    kr = IOMasterPort(MACH_PORT_NULL, &master);
    if (kr != KERN_SUCCESS) {
        errno = EIO;
        return -1;
    }
    kr = IOServiceGetMatchingServices(master, IOServiceMatching(kVirtioVsockClass), &iter);
    if (kr != KERN_SUCCESS) {
        errno = ENXIO;
        return -1;
    }
    service = IOIteratorNext(iter);
    IOObjectRelease(iter);
    if (!service) {
        errno = ENXIO;
        return -1;
    }
    kr = IOServiceOpen(service, mach_task_self(), 0, &gConn);
    IOObjectRelease(service);
    if (kr != KERN_SUCCESS) {
        gConn = 0;
        errno = ENXIO;
        return -1;
    }
    return 0;
}

int vsock_local_cid(unsigned int *hi, unsigned int *lo)
{
    int h = 0, l = 0, n = 0;
    kern_return_t kr;

    if (vsock_open() < 0)
        return -1;
    kr = vs_scalar(kVsockInfo, 0, 3, &h, &l, &n);
    if (kr != KERN_SUCCESS)
        return fail(kr);
    if (hi) *hi = (unsigned int) h;
    if (lo) *lo = (unsigned int) l;
    return 0;
}

int vsock_listen(unsigned int port, int backlog)
{
    int handle = -1;
    kern_return_t kr;

    if (vsock_open() < 0)
        return -1;
    kr = vs_scalar(kVsockListen, 2, 1, (int) port, backlog, &handle);
    return kr == KERN_SUCCESS ? handle : fail(kr);
}

int vsock_accept(int handle, unsigned int *peer_hi, unsigned int *peer_lo, unsigned int *peer_port,
                 int nonblock)
{
    int nh = -1, hi = 0, lo = 0, port = 0, spins = 0;
    kern_return_t kr;

    if (vsock_open() < 0)
        return -1;
    for (;;) {
        kr = vs_scalar(kVsockAccept, 2, 4, handle, 0, &nh, &hi, &lo, &port);
        if (kr == KERN_SUCCESS)
            break;
        if (kr != kIOReturnNotReady || nonblock)
            return fail(kr);
        if (wait_a_little(&spins) < 0)
            return -1;
    }
    if (peer_hi) *peer_hi = (unsigned int) hi;
    if (peer_lo) *peer_lo = (unsigned int) lo;
    if (peer_port) *peer_port = (unsigned int) port;
    return nh;
}

int vsock_connect(unsigned int cid_hi, unsigned int cid_lo, unsigned int port, unsigned int timeout_ms)
{
    int handle = -1, spins = 0;
    unsigned int readable, writable, flags;
    struct timeval start, now;
    kern_return_t kr;

    if (vsock_open() < 0)
        return -1;
    kr = vs_scalar(kVsockConnect, 4, 1, (int) cid_hi, (int) cid_lo, (int) port,
                                       0, &handle);
    if (kr != KERN_SUCCESS)
        return fail(kr);
    if (!timeout_ms)
        timeout_ms = 10000;
    gettimeofday(&start, 0);
    for (;;) {
        if (vsock_poll(handle, &readable, &writable, &flags) < 0)
            break;
        if (flags & VSOCK_P_CONNECTED)
            return handle;
        if (flags & VSOCK_P_RESET) {
            vsock_close(handle);
            errno = ECONNREFUSED;
            return -1;
        }
        gettimeofday(&now, 0);
        if ((unsigned long) ((now.tv_sec - start.tv_sec) * 1000 + (now.tv_usec - start.tv_usec) / 1000) >= timeout_ms) {
            vsock_close(handle);
            errno = ETIMEDOUT;
            return -1;
        }
        if (wait_a_little(&spins) < 0)
            break;
    }
    {
        int saved = errno;

        vsock_close(handle);
        errno = saved;
    }
    return -1;
}

int vsock_read(int handle, void *buf, size_t n, int nonblock)
{
    static VsockReadIn in;
    static VsockReadOut out;
    int spins = 0;

    if (vsock_open() < 0)
        return -1;
    if (n > VSOCK_XFER_MAX)
        n = VSOCK_XFER_MAX;
    for (;;) {
        kern_return_t kr;

        in.handle = handle;
        in.maxLength = n;
        in.nonblock = 0;
        kr = vs_struct(kVsockRead, &in, sizeof(in), &out, sizeof(out));
        if (kr == KERN_SUCCESS)
            break;
        if (kr != kIOReturnNotReady || nonblock)
            return fail(kr);
        if (wait_a_little(&spins) < 0)
            return -1;
    }
    memcpy(buf, out.data, out.length);
    return (int) out.length;
}

int vsock_write(int handle, const void *buf, size_t n, int nonblock)
{
    static VsockWriteIn in;
    static VsockWriteOut out;
    size_t done = 0;
    int spins = 0;

    if (vsock_open() < 0)
        return -1;
    while (done < n) {
        size_t chunk = n - done;
        kern_return_t kr;

        if (chunk > VSOCK_XFER_MAX)
            chunk = VSOCK_XFER_MAX;
        in.handle = handle;
        in.nonblock = 0;
        in.length = chunk;
        memcpy(in.data, (const char *) buf + done, chunk);
        out.written = 0;
        kr = vs_struct(kVsockWrite, &in, sizeof(in), &out, sizeof(out));
        if (kr == KERN_SUCCESS) {
            done += out.written;
            if (out.written)
                spins = 0;
            continue;
        }
        if (kr == kIOReturnNotReady && !nonblock) {
            if (wait_a_little(&spins) < 0)
                return done ? (int) done : -1;
            continue;
        }
        if (done)
            break;
        return fail(kr);
    }
    return (int) done;
}

int vsock_shutdown(int handle, int how)
{
    kern_return_t kr;

    if (vsock_open() < 0)
        return -1;
    kr = vs_scalar(kVsockShutdown, 2, 0, handle, how);
    return kr == KERN_SUCCESS ? 0 : fail(kr);
}

int vsock_close(int handle)
{
    kern_return_t kr;

    if (vsock_open() < 0)
        return -1;
    kr = vs_scalar(kVsockClose, 1, 0, handle);
    return kr == KERN_SUCCESS ? 0 : fail(kr);
}

int vsock_poll(int handle, unsigned int *readable, unsigned int *writable, unsigned int *flags)
{
    int r = 0, w = 0, f = 0;
    kern_return_t kr;

    if (vsock_open() < 0)
        return -1;
    kr = vs_scalar(kVsockPoll, 1, 3, handle, &r, &w, &f);
    if (kr != KERN_SUCCESS)
        return fail(kr);
    if (readable) *readable = (unsigned int) r;
    if (writable) *writable = (unsigned int) w;
    if (flags) *flags = (unsigned int) f;
    return 0;
}
