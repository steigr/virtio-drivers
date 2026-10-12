/*
 * vsockcat: a netcat for vsock, and the driver's test client.
 *
 *   vsockcat -l PORT           listen; echo each connection's bytes back until it ends, then
 *                              wait for the next (stops after -n connections, if given)
 *   vsockcat -s PORT           listen; write each connection's bytes to stdout until it ends
 *   vsockcat -c CID PORT       connect; send stdin, then read the reply to stdout until it ends
 *   vsockcat -i                print our CID
 *
 * Half-duplex by design: libvsock's handles are not file descriptors, and one thread does it all.
 * Exit status is 0 only if everything asked for was done.
 */

#include <errno.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

#include "vsock.h"

static unsigned char buf[VSOCK_XFER_MAX];

static int die(const char *what)
{
    fprintf(stderr, "vsockcat: %s: %s\n", what, strerror(errno));
    return 1;
}

static int echo_one(int h, int echo)
{
    int n;

    for (;;) {
        n = vsock_read(h, buf, sizeof(buf), 0);
        if (n < 0)
            return die("read");
        if (n == 0)
            return 0;
        if (echo) {
            if (vsock_write(h, buf, n, 0) != n)
                return die("write");
        } else if (fwrite(buf, 1, n, stdout) != (size_t) n)
            return die("stdout");
    }
}

int main(int argc, char **argv)
{
    int h, c, n, max = 0, count = 0, status = 0;
    unsigned int hi, lo, port, peer;

    if (argc >= 2 && !strcmp(argv[1], "-i")) {
        if (vsock_local_cid(&hi, &lo) < 0)
            return die("open");
        printf("%u\n", lo);
        return 0;
    }
    if (argc >= 3 && (!strcmp(argv[1], "-l") || !strcmp(argv[1], "-s"))) {
        int echo = !strcmp(argv[1], "-l");

        port = (unsigned int) atoi(argv[2]);
        if (argc >= 5 && !strcmp(argv[3], "-n"))
            max = atoi(argv[4]);
        h = vsock_listen(port, 8);
        if (h < 0)
            return die("listen");
        fprintf(stderr, "vsockcat: listening on port %u\n", port);
        for (;;) {
            c = vsock_accept(h, &hi, &lo, &peer, 0);
            if (c < 0)
                return die("accept");
            fprintf(stderr, "vsockcat: connection from %u:%u\n", lo, peer);
            if (echo_one(c, echo))
                status = 1;
            vsock_shutdown(c, 2);
            vsock_close(c);
            if (max && ++count >= max)
                break;
        }
        vsock_close(h);
        return status;
    }
    if (argc >= 4 && !strcmp(argv[1], "-c")) {
        unsigned int cid = (unsigned int) strtoul(argv[2], 0, 10);

        port = (unsigned int) atoi(argv[3]);
        h = vsock_connect(0, cid, port, 0);
        if (h < 0)
            return die("connect");
        while ((n = (int) fread(buf, 1, sizeof(buf), stdin)) > 0)
            if (vsock_write(h, buf, n, 0) != n)
                return die("write");
        vsock_shutdown(h, 2);
        status = echo_one(h, 0);
        vsock_close(h);
        return status;
    }
    fprintf(stderr, "usage: vsockcat -l PORT [-n N] | -s PORT [-n N] | -c CID PORT | -i\n");
    return 2;
}
