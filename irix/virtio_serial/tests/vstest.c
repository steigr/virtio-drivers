/*
 * Guest side of the virtio-serial tests: tests/iris/run_tests.py runs it inside
 * IRIX against a host end it controls.
 *
 *   vstest info PATH              the port's VIOSER_GETINFO, one line
 *   vstest send PATH TEXT         write TEXT
 *   vstest recv PATH N            read N bytes, print them
 *   vstest eof PATH               read until end of file, print the byte count
 *   vstest busy PATH              a second open fails with EBUSY
 *   vstest nonblock PATH          with nothing to read, O_NONBLOCK gives EAGAIN
 *   vstest signal PATH            a signal ends a blocked read with EINTR
 *   vstest sum PATH N             send N pattern bytes, read N back, compare,
 *                                 multiplexing with poll(2)
 *
 * Exit status 0 is a pass.
 */
#include <sys/types.h>
#include <errno.h>
#include <fcntl.h>
#include <poll.h>
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

#include "vioser.h"

static void
die(const char *what)
{
	perror(what);
	exit(1);
}

static int
xopen(const char *path, int flags)
{
	int fd = open(path, flags);

	if (fd < 0)
		die(path);
	return fd;
}

/* The pattern depends on position, so a dropped or repeated byte is caught. */
static unsigned char
pat(long i)
{
	return (unsigned char)((i * 7 + (i >> 8) * 13 + (i >> 16)) & 0xff);
}

static int
cmd_info(const char *path)
{
	struct vioser_info vi;
	int fd = xopen(path, O_RDWR);

	if (ioctl(fd, VIOSER_GETINFO, &vi) < 0)
		die("VIOSER_GETINFO");
	printf("port=%u console=%d host_open=%d name=%s\n", vi.vi_port,
	    (vi.vi_flags & VIOSER_F_CONSOLE) != 0,
	    (vi.vi_flags & VIOSER_F_HOST_OPEN) != 0, vi.vi_name);
	return 0;
}

static int
cmd_send(const char *path, const char *text)
{
	int fd = xopen(path, O_RDWR);

	if (write(fd, text, strlen(text)) != (ssize_t)strlen(text))
		die("write");
	return 0;
}

static int
cmd_recv(const char *path, long n)
{
	char buf[4096];
	int fd = xopen(path, O_RDWR);

	printf("ready\n");
	fflush(stdout);
	while (n > 0) {
		long want = n < (long)sizeof(buf) ? n : (long)sizeof(buf);
		ssize_t got = read(fd, buf, want);

		if (got < 0)
			die("read");
		if (got == 0) {
			printf("short: end of file\n");
			return 1;
		}
		fwrite(buf, 1, got, stdout);
		n -= got;
	}
	printf("\n");
	return 0;
}

static int
cmd_eof(const char *path)
{
	char buf[4096];
	long total = 0;
	ssize_t got;
	int fd = xopen(path, O_RDWR);

	printf("ready\n");
	fflush(stdout);
	while ((got = read(fd, buf, sizeof(buf))) > 0)
		total += got;
	if (got < 0)
		die("read");
	printf("eof after %ld bytes\n", total);
	return 0;
}

static int
cmd_busy(const char *path)
{
	int fd = xopen(path, O_RDWR);

	(void)fd;
	if (open(path, O_RDWR) >= 0) {
		printf("second open succeeded\n");
		return 1;
	}
	if (errno != EBUSY)
		die("second open");
	printf("EBUSY\n");
	return 0;
}

static int
cmd_nonblock(const char *path)
{
	char c;
	int fd = xopen(path, O_RDWR | O_NONBLOCK);

	if (read(fd, &c, 1) >= 0) {
		printf("read returned data or end of file\n");
		return 1;
	}
	if (errno != EAGAIN)
		die("read");
	printf("EAGAIN\n");
	return 0;
}

static void
on_alarm(int sig)
{
	(void)sig;
}

static int
cmd_signal(const char *path)
{
	char c;
	int fd = xopen(path, O_RDWR);

	sigset(SIGALRM, on_alarm);
	alarm(2);
	if (read(fd, &c, 1) >= 0) {
		printf("read returned instead of being interrupted\n");
		return 1;
	}
	if (errno != EINTR)
		die("read");
	printf("EINTR\n");
	return 0;
}

static int
cmd_sum(const char *path, long n)
{
	static unsigned char buf[8192];
	struct pollfd pfd;
	long sent = 0, rcvd = 0;
	int fd = xopen(path, O_RDWR);

	fcntl(fd, F_SETFL, O_NONBLOCK);
	while (rcvd < n) {
		pfd.fd = fd;
		pfd.events = POLLIN;
		if (sent < n)
			pfd.events |= POLLOUT;
		pfd.revents = 0;
		if (poll(&pfd, 1, 60000) <= 0) {
			printf("poll timed out or failed: sent %ld rcvd %ld\n",
			    sent, rcvd);
			return 1;
		}
		if (pfd.revents & POLLOUT) {
			long k, want = n - sent;
			ssize_t w;

			if (want > (long)sizeof(buf))
				want = sizeof(buf);
			for (k = 0; k < want; k++)
				buf[k] = pat(sent + k);
			w = write(fd, buf, want);
			if (w < 0 && errno != EAGAIN)
				die("write");
			if (w > 0)
				sent += w;
		}
		if (pfd.revents & POLLIN) {
			ssize_t r = read(fd, buf, sizeof(buf));
			long k;

			if (r < 0 && errno != EAGAIN)
				die("read");
			for (k = 0; k < r; k++) {
				if (buf[k] != pat(rcvd + k)) {
					printf("mismatch at %ld\n", rcvd + k);
					return 1;
				}
			}
			if (r > 0)
				rcvd += r;
		}
		if ((pfd.revents & POLLHUP) && !(pfd.revents & POLLIN)) {
			printf("host went away: sent %ld rcvd %ld\n", sent, rcvd);
			return 1;
		}
	}
	printf("ok %ld bytes\n", rcvd);
	return 0;
}

int
main(int argc, char **argv)
{
	if (argc >= 3 && strcmp(argv[1], "info") == 0)
		return cmd_info(argv[2]);
	if (argc >= 4 && strcmp(argv[1], "send") == 0)
		return cmd_send(argv[2], argv[3]);
	if (argc >= 4 && strcmp(argv[1], "recv") == 0)
		return cmd_recv(argv[2], atol(argv[3]));
	if (argc >= 3 && strcmp(argv[1], "eof") == 0)
		return cmd_eof(argv[2]);
	if (argc >= 3 && strcmp(argv[1], "busy") == 0)
		return cmd_busy(argv[2]);
	if (argc >= 3 && strcmp(argv[1], "nonblock") == 0)
		return cmd_nonblock(argv[2]);
	if (argc >= 3 && strcmp(argv[1], "signal") == 0)
		return cmd_signal(argv[2]);
	if (argc >= 4 && strcmp(argv[1], "sum") == 0)
		return cmd_sum(argv[2], atol(argv[3]));
	fprintf(stderr, "usage: see the comment at the top of vstest.c\n");
	return 2;
}
