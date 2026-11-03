/*
 * What the 9p filesystem has to do for a program, run in a directory on a
 * mount.  Prints a line per check and exits non-zero if any failed:
 *
 *	p9test all DIR          every check here
 *	p9test readdir DIR N    N files in a fresh directory, listed back
 *	p9test mkbig FILE MB    a file of MB megabytes of the pattern
 *	p9test ckbig FILE MB    check it
 *	p9test hold FILE        open it and wait, for a busy unmount
 *
 * C89, for MIPSpro.  Builds for any ABI, to exercise each of the getdents and
 * stat paths.
 */
#include <sys/types.h>
#include <sys/mman.h>
#include <sys/stat.h>
#include <sys/statvfs.h>
#include <sys/time.h>
#include <sys/wait.h>
#include <dirent.h>
#include <errno.h>
#include <fcntl.h>
#include <limits.h>
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <utime.h>

static int failed;
static char dirpath[1024];

#define CHECK(name, cond) check((name), (cond) != 0, __LINE__)

static void
check(const char *name, int ok, int line)
{
	printf("%s  %s", ok ? "ok  " : "FAIL", name);
	if (!ok) {
		printf(" (line %d, errno %d: %s)", line, errno, strerror(errno));
		failed++;
	}
	printf("\n");
	fflush(stdout);
}

static char *
at(const char *name)
{
	static char buf[4][1280];
	static int i;
	char *p = buf[i++ & 3];

	sprintf(p, "%s/%s", dirpath, name);
	return p;
}

static void
put(const char *path, const char *s)
{
	int fd = open(path, O_WRONLY | O_CREAT | O_TRUNC, 0644);

	if (fd >= 0) {
		(void)write(fd, s, strlen(s));
		(void)close(fd);
	}
}

static int
same(const char *path, const char *s)
{
	char buf[4096];
	int fd, n;

	if ((fd = open(path, O_RDONLY)) < 0)
		return 0;
	n = read(fd, buf, sizeof(buf) - 1);
	(void)close(fd);
	if (n < 0)
		return 0;
	buf[n] = 0;
	return strcmp(buf, s) == 0;
}

static void
test_basic(void)
{
	char buf[64];
	struct stat st;
	int fd;

	fd = open(at("a"), O_RDWR | O_CREAT | O_EXCL, 0640);
	CHECK("create exclusive", fd >= 0);
	CHECK("create exclusive again is EEXIST",
	    open(at("a"), O_RDWR | O_CREAT | O_EXCL, 0640) < 0 && errno == EEXIST);
	CHECK("write", write(fd, "hello", 5) == 5);
	CHECK("seek and write", lseek(fd, 2, SEEK_SET) == 2 && write(fd, "XY", 2) == 2);
	CHECK("seek and read", lseek(fd, 0, SEEK_SET) == 0 && read(fd, buf, 64) == 5 &&
	    memcmp(buf, "heXYo", 5) == 0);
	CHECK("read at end of file is 0", read(fd, buf, 64) == 0);
	CHECK("fstat size", fstat(fd, &st) == 0 && st.st_size == 5 && S_ISREG(st.st_mode));
	CHECK("mode as created, under umask", (st.st_mode & 0777) == (0640 & ~umask(022)));
	CHECK("close", close(fd) == 0);
	CHECK("reopen sees it", same(at("a"), "heXYo"));
	CHECK("open missing is ENOENT", open(at("nope"), O_RDONLY) < 0 && errno == ENOENT);
	CHECK("path through a file is ENOTDIR",
	    open(at("a/b"), O_RDONLY) < 0 && errno == ENOTDIR);

	fd = open(at("a"), O_WRONLY | O_APPEND);
	CHECK("append", fd >= 0 && write(fd, "!!", 2) == 2 && close(fd) == 0);
	CHECK("append went to the end", same(at("a"), "heXYo!!"));
	fd = open(at("a"), O_WRONLY | O_TRUNC);
	CHECK("O_TRUNC", fd >= 0 && fstat(fd, &st) == 0 && st.st_size == 0);
	(void)close(fd);
	CHECK("remove", unlink(at("a")) == 0);
	CHECK("gone", stat(at("a"), &st) < 0 && errno == ENOENT);
}

static void
test_sizes(void)
{
	char *b;
	struct stat st;
	int fd, i, ok;
	off_t where;

	/* Across several of the mount's blocks, unaligned. */
	b = malloc(200000);
	for (i = 0; i < 200000; i++)
		b[i] = (char)(i * 7);
	fd = open(at("big"), O_RDWR | O_CREAT | O_TRUNC, 0644);
	CHECK("large unaligned write", write(fd, b, 12345) == 12345 &&
	    write(fd, b + 12345, 200000 - 12345) == 200000 - 12345);
	CHECK("close flushes", close(fd) == 0);
	bzero(b, 200000);
	fd = open(at("big"), O_RDONLY);
	CHECK("large read", read(fd, b, 200000) == 200000);
	ok = 1;
	for (i = 0; i < 200000; i++)
		ok &= b[i] == (char)(i * 7);
	CHECK("large read matches", ok);
	(void)close(fd);

	/* A hole reads as zeroes, and the file ends where the last write did. */
	fd = open(at("sparse"), O_RDWR | O_CREAT | O_TRUNC, 0644);
	CHECK("write past the end", lseek(fd, 100000, SEEK_SET) == 100000 &&
	    write(fd, "end", 3) == 3);
	CHECK("sparse size", fstat(fd, &st) == 0 && st.st_size == 100003);
	(void)lseek(fd, 50000, SEEK_SET);
	CHECK("hole reads zeroes", read(fd, b, 100) == 100 && b[0] == 0 && b[99] == 0);
	(void)close(fd);
	CHECK("hole on the host too", stat(at("sparse"), &st) == 0 && st.st_size == 100003);

	/* Truncating down, then growing: the old tail must not come back. */
	fd = open(at("trunc"), O_RDWR | O_CREAT | O_TRUNC, 0644);
	(void)write(fd, b + 1000, 40000);
	memset(b, 'x', 40000);
	(void)lseek(fd, 0, SEEK_SET);
	(void)write(fd, b, 40000);
	CHECK("ftruncate down", ftruncate(fd, 100) == 0);
	CHECK("size after truncate", fstat(fd, &st) == 0 && st.st_size == 100);
	CHECK("ftruncate up", ftruncate(fd, 40000) == 0);
	(void)lseek(fd, 99, SEEK_SET);
	CHECK("read over the old end", read(fd, b, 2) == 2 && b[0] == 'x' && b[1] == 0);
	(void)close(fd);
	CHECK("truncate by name", truncate(at("trunc"), 7) == 0 &&
	    stat(at("trunc"), &st) == 0 && st.st_size == 7);

	/* Past 4 GB, in a hole: offsets are 64 bits all the way down. */
	fd = open(at("huge"), O_RDWR | O_CREAT | O_TRUNC, 0644);
	where = (off_t)5 * 1024 * 1024 * 1024;
	CHECK("seek past 4G", lseek(fd, where, SEEK_SET) == where);
	CHECK("write past 4G", write(fd, "far", 3) == 3);
	(void)close(fd);
	fd = open(at("huge"), O_RDONLY);
	CHECK("read past 4G", lseek(fd, where, SEEK_SET) == where &&
	    read(fd, b, 3) == 3 && memcmp(b, "far", 3) == 0);
	(void)close(fd);
	(void)unlink(at("huge"));
	(void)unlink(at("big"));
	(void)unlink(at("sparse"));
	(void)unlink(at("trunc"));
	free(b);
}

static void
test_names(void)
{
	char buf[256], name[300];
	struct stat st, st2;
	int fd, n;

	CHECK("mkdir", mkdir(at("d"), 0755) == 0);
	CHECK("mkdir again is EEXIST", mkdir(at("d"), 0755) < 0 && errno == EEXIST);
	CHECK("stat directory", stat(at("d"), &st) == 0 && S_ISDIR(st.st_mode));
	put(at("d/f"), "in d");
	CHECK("rmdir of non-empty is ENOTEMPTY", rmdir(at("d")) < 0 &&
	    (errno == ENOTEMPTY || errno == EEXIST));
	CHECK("unlink of a directory is refused", unlink(at("d")) < 0 && errno == EPERM);
	CHECK("rmdir of a file is ENOTDIR", rmdir(at("d/f")) < 0 && errno == ENOTDIR);

	CHECK("rename file", rename(at("d/f"), at("d/g")) == 0 && same(at("d/g"), "in d"));
	put(at("d/h"), "other");
	CHECK("rename over a file", rename(at("d/g"), at("d/h")) == 0 && same(at("d/h"), "in d"));
	CHECK("replaced name gone", stat(at("d/g"), &st) < 0 && errno == ENOENT);
	fd = open(at("d/h"), O_RDONLY);
	CHECK("rename directory", rename(at("d"), at("e")) == 0 && same(at("e/h"), "in d"));
	CHECK("open file survives its directory's rename", fd >= 0 && read(fd, buf, 4) == 4);
	(void)close(fd);
	CHECK("old directory gone", stat(at("d"), &st) < 0 && errno == ENOENT);

	CHECK("symlink", symlink("h", at("e/s")) == 0);
	CHECK("lstat symlink", lstat(at("e/s"), &st) == 0 && S_ISLNK(st.st_mode));
	n = readlink(at("e/s"), buf, sizeof(buf));
	CHECK("readlink", n == 1 && buf[0] == 'h');
	CHECK("follow symlink", same(at("e/s"), "in d"));
	CHECK("symlink absolute", symlink("/etc/passwd", at("e/abs")) == 0 && stat(at("e/abs"), &st) == 0);

	CHECK("hard link", link(at("e/h"), at("e/l")) == 0);
	CHECK("hard link counts", stat(at("e/h"), &st) == 0 && st.st_nlink == 2);
	CHECK("hard link is the same file", stat(at("e/l"), &st2) == 0 && st.st_ino == st2.st_ino);
	CHECK("unlink one name", unlink(at("e/h")) == 0);
	CHECK("other name remains", same(at("e/l"), "in d"));

	memset(name, 'n', 255);
	name[255] = 0;
	sprintf(buf, "%s", "");
	{
		char p[1600];

		sprintf(p, "%s/%s", dirpath, name);
		put(p, "long");
		CHECK("name of 255 bytes", same(p, "long"));
		(void)unlink(p);
		name[255] = 'n';
		name[256] = 0;
		sprintf(p, "%s/%s", dirpath, name);
		CHECK("name of 256 bytes is ENAMETOOLONG", open(p, O_CREAT | O_WRONLY, 0644) < 0 &&
		    errno == ENAMETOOLONG);
	}

	(void)unlink(at("e/l"));
	(void)unlink(at("e/s"));
	(void)unlink(at("e/abs"));
	CHECK("rmdir", rmdir(at("e")) == 0);
}

static void
test_attrs(void)
{
	struct stat st;
	struct utimbuf ut;
	struct statvfs sv;

	put(at("attr"), "x");
	CHECK("chmod", chmod(at("attr"), 0604) == 0 && stat(at("attr"), &st) == 0 &&
	    (st.st_mode & 0777) == 0604);
	ut.actime = 1000000;
	ut.modtime = 2000000;
	CHECK("utime given", utime(at("attr"), &ut) == 0 && stat(at("attr"), &st) == 0 &&
	    st.st_mtime == 2000000 && st.st_atime == 1000000);
	CHECK("utime now", utime(at("attr"), 0) == 0 && stat(at("attr"), &st) == 0 &&
	    st.st_mtime > 2000000);
	CHECK("chown accepted", chown(at("attr"), 5, 5) == 0);
	CHECK("access F_OK", access(at("attr"), F_OK) == 0);
	CHECK("access missing", access(at("nope"), F_OK) < 0 && errno == ENOENT);
	CHECK("statvfs", statvfs(dirpath, &sv) == 0 && sv.f_blocks != 0 &&
	    strcmp(sv.f_basetype, "9p") == 0);
	CHECK("statvfs name length", sv.f_namemax >= 255);
	(void)unlink(at("attr"));
}

/* A non-root process: permission checks are this side's to make. */
static void
test_perms(void)
{
	pid_t pid;
	int st;

	put(at("priv"), "secret");
	(void)chmod(at("priv"), 0600);
	(void)mkdir(at("privd"), 0700);
	put(at("pub"), "open");
	(void)chmod(at("pub"), 0644);
	pid = fork();
	if (pid == 0) {
		int bad = 0;

		(void)setgid(60001);
		(void)setuid(60001);
		if (open(at("priv"), O_RDONLY) >= 0 || errno != EACCES)
			bad |= 1;
		if (open(at("pub"), O_RDONLY) < 0)
			bad |= 2;
		if (open(at("pub"), O_WRONLY) >= 0 || errno != EACCES)
			bad |= 4;
		if (open(at("privd/x"), O_RDONLY) >= 0 || errno != EACCES)
			bad |= 8;
		if (unlink(at("pub")) == 0 || errno != EACCES)
			bad |= 16;
		if (chmod(at("pub"), 0777) == 0 || errno != EPERM)
			bad |= 32;
		if (mkdir(at("nobody"), 0777) == 0 || errno != EACCES)
			bad |= 64;
		_exit(bad);
	}
	(void)waitpid(pid, &st, 0);
	if (!WIFEXITED(st) || WEXITSTATUS(st) != 0)
		printf("     nobody's refusals: %x\n", WEXITSTATUS(st));
	CHECK("permission checks for another user", WIFEXITED(st) && WEXITSTATUS(st) == 0);
	(void)unlink(at("priv"));
	(void)unlink(at("pub"));
	(void)rmdir(at("privd"));
}

/* Files in use after their names have gone. */
static void
test_unlinked(void)
{
	char buf[16];
	struct stat st;
	int fd;

	fd = open(at("tmp"), O_RDWR | O_CREAT | O_TRUNC, 0644);
	CHECK("write before removal", write(fd, "before", 6) == 6);
	CHECK("remove while open", unlink(at("tmp")) == 0);
	CHECK("name gone", stat(at("tmp"), &st) < 0 && errno == ENOENT);
	CHECK("fstat after removal", fstat(fd, &st) == 0 && st.st_size == 6);
	CHECK("write after removal", write(fd, "after", 5) == 5);
	(void)lseek(fd, 0, SEEK_SET);
	CHECK("read after removal", read(fd, buf, 11) == 11 && memcmp(buf, "beforeafter", 11) == 0);
	CHECK("close after removal", close(fd) == 0);
}

static void
test_mmap(void)
{
	char *p;
	struct stat st;
	int fd, i, ok;
	size_t len = 100000;

	fd = open(at("map"), O_RDWR | O_CREAT | O_TRUNC, 0644);
	p = malloc(len);
	for (i = 0; i < (int)len; i++)
		p[i] = (char)(i * 3);
	(void)write(fd, p, len);
	free(p);
	(void)close(fd);

	/* Reading through a mapping. */
	fd = open(at("map"), O_RDONLY);
	p = mmap(0, len, PROT_READ, MAP_SHARED, fd, 0);
	CHECK("mmap read", p != (char *)MAP_FAILED);
	if (p == (char *)MAP_FAILED)
		return;
	ok = 1;
	for (i = 0; i < (int)len; i++)
		ok &= p[i] == (char)(i * 3);
	CHECK("mapped bytes match", ok);
	CHECK("munmap", munmap(p, len) == 0);
	(void)close(fd);

	/* Writing through one: read(2) sees it at once, and so does a new open. */
	fd = open(at("map"), O_RDWR);
	p = mmap(0, len, PROT_READ | PROT_WRITE, MAP_SHARED, fd, 0);
	CHECK("mmap write", p != (char *)MAP_FAILED);
	if (p == (char *)MAP_FAILED)
		return;
	p[70000] = 'M';
	CHECK("msync", msync(p, len, MS_SYNC) == 0);
	{
		char c;

		CHECK("read sees mapped write", pread(fd, &c, 1, 70000) == 1 && c == 'M');
		CHECK("write seen through the mapping", pwrite(fd, "W", 1, 80000) == 1 && p[80000] == 'W');
	}
	CHECK("munmap written", munmap(p, len) == 0);
	CHECK("close written", close(fd) == 0);

	/* A private mapping's writes stay private. */
	fd = open(at("map"), O_RDWR);
	p = mmap(0, len, PROT_READ | PROT_WRITE, MAP_PRIVATE, fd, 0);
	CHECK("mmap private", p != (char *)MAP_FAILED);
	if (p == (char *)MAP_FAILED)
		return;
	p[100] = 'P';
	(void)munmap(p, len);
	{
		char c;

		CHECK("private write not in the file", pread(fd, &c, 1, 100) == 1 && c == (char)300);
	}
	(void)close(fd);
	(void)stat(at("map"), &st);
	(void)unlink(at("map"));
}

static void
test_locks(void)
{
	struct flock fl;
	pid_t pid;
	int fd, st;

	fd = open(at("lock"), O_RDWR | O_CREAT, 0644);
	fl.l_type = F_WRLCK;
	fl.l_whence = SEEK_SET;
	fl.l_start = 0;
	fl.l_len = 10;
	CHECK("fcntl lock", fcntl(fd, F_SETLK, &fl) == 0);
	pid = fork();
	if (pid == 0) {
		int fd2 = open(at("lock"), O_RDWR);
		struct flock f2;

		f2.l_type = F_WRLCK;
		f2.l_whence = SEEK_SET;
		f2.l_start = 5;
		f2.l_len = 1;
		_exit(fcntl(fd2, F_SETLK, &f2) < 0 && (errno == EAGAIN || errno == EACCES) ? 0 : 1);
	}
	(void)waitpid(pid, &st, 0);
	CHECK("lock conflicts in another process", WIFEXITED(st) && WEXITSTATUS(st) == 0);
	CHECK("close releases", close(fd) == 0);
	(void)unlink(at("lock"));
}

/* Children writing their own parts of one file, and each their own file. */
static void
test_concurrent(void)
{
	enum { KIDS = 4, EACH = 262144 };
	char *b;
	pid_t pids[KIDS];
	int k, st, ok = 1, fd;

	for (k = 0; k < KIDS; k++) {
		pids[k] = fork();
		if (pids[k] == 0) {
			char name[32];
			char c = (char)('A' + k);
			int i;

			b = malloc(4096);
			memset(b, c, 4096);
			fd = open(at("shared"), O_WRONLY | O_CREAT, 0644);
			sprintf(name, "own%d", k);
			{
				int own = open(at(name), O_RDWR | O_CREAT | O_TRUNC, 0644);

				for (i = 0; i < EACH / 4096; i++) {
					if (pwrite(fd, b, 4096, (off_t)(k * EACH + i * 4096)) != 4096 ||
					    write(own, b, 4096) != 4096)
						_exit(1);
				}
				(void)close(own);
			}
			(void)close(fd);
			_exit(0);
		}
	}
	for (k = 0; k < KIDS; k++) {
		(void)waitpid(pids[k], &st, 0);
		ok &= WIFEXITED(st) && WEXITSTATUS(st) == 0;
	}
	CHECK("concurrent writers finish", ok);
	b = malloc(EACH);
	fd = open(at("shared"), O_RDONLY);
	ok = 1;
	for (k = 0; k < KIDS; k++) {
		int i;

		ok &= read(fd, b, EACH) == EACH;
		for (i = 0; i < EACH; i++)
			ok &= b[i] == (char)('A' + k);
	}
	(void)close(fd);
	CHECK("concurrent writes all there", ok);
	(void)unlink(at("shared"));
	for (k = 0; k < KIDS; k++) {
		char name[32];

		sprintf(name, "own%d", k);
		(void)unlink(at(name));
	}
	free(b);
}

static int
test_readdir(const char *dir, int n)
{
	char name[1400];
	char *seen;
	DIR *d;
	struct dirent *e;
	int i, count = 0, dots = 0;

	strcpy(dirpath, dir);
	sprintf(name, "%s/rd", dir);
	if (mkdir(name, 0755) != 0) {
		perror(name);
		return 1;
	}
	for (i = 0; i < n; i++) {
		sprintf(name, "%s/rd/entry-with-a-longish-name-%05d", dir, i);
		put(name, "");
	}
	sprintf(name, "%s/rd", dir);
	seen = calloc(n, 1);
	d = opendir(name);
	CHECK("opendir", d != 0);
	while ((e = readdir(d)) != 0) {
		if (strcmp(e->d_name, ".") == 0 || strcmp(e->d_name, "..") == 0) {
			dots++;
		} else if (sscanf(e->d_name, "entry-with-a-longish-name-%d", &i) == 1 &&
		    i >= 0 && i < n && !seen[i]) {
			seen[i] = 1;
			count++;
		}
	}
	CHECK("every entry once", count == n);
	CHECK(". and ..", dots == 2);
	rewinddir(d);
	CHECK("rewinddir", (e = readdir(d)) != 0);
	(void)closedir(d);
	for (i = 0; i < n; i++) {
		sprintf(name, "%s/rd/entry-with-a-longish-name-%05d", dir, i);
		if (unlink(name) != 0)
			count = -1;
	}
	CHECK("all removed", count == n);
	sprintf(name, "%s/rd", dir);
	CHECK("directory empty again", rmdir(name) == 0);
	return failed != 0;
}

static unsigned char
pattern(off_t at_)
{
	return (unsigned char)((at_ * 2654435761UL) >> 13);
}

static int
big(const char *path, int mb, int writing)
{
	static unsigned char b[65536];
	off_t at_ = 0, total = (off_t)mb * 1024 * 1024;
	int fd, i, chunk, n;
	unsigned int step = 0;

	fd = writing ? open(path, O_WRONLY | O_CREAT | O_TRUNC, 0644) : open(path, O_RDONLY);
	if (fd < 0) {
		perror(path);
		return 1;
	}
	while (at_ < total) {
		/* Sizes that fall across the cache's blocks every which way. */
		chunk = 1 + (int)((step++ * 7919) % sizeof(b));
		if (chunk > total - at_)
			chunk = (int)(total - at_);
		if (writing) {
			for (i = 0; i < chunk; i++)
				b[i] = pattern(at_ + i);
			n = write(fd, b, chunk);
		} else {
			n = read(fd, b, chunk);
			for (i = 0; i < n; i++) {
				if (b[i] != pattern(at_ + i)) {
					printf("FAIL  byte %lld differs\n", (long long)(at_ + i));
					return 1;
				}
			}
		}
		if (n != chunk) {
			printf("FAIL  %d of %d bytes at %lld: %s\n", n, chunk, (long long)at_,
			    strerror(errno));
			return 1;
		}
		at_ += n;
	}
	if (close(fd) != 0) {
		perror("close");
		return 1;
	}
	printf("ok %d MiB %s\n", mb, writing ? "written" : "verified");
	return 0;
}

int
main(int argc, char **argv)
{
	if (argc >= 3 && strcmp(argv[1], "all") == 0) {
		strcpy(dirpath, argv[2]);
		(void)umask(022);
		test_basic();
		test_sizes();
		test_names();
		test_attrs();
		test_perms();
		test_unlinked();
		test_mmap();
		test_locks();
		test_concurrent();
		printf("%d failed\n", failed);
		return failed != 0;
	}
	if (argc >= 4 && strcmp(argv[1], "readdir") == 0)
		return test_readdir(argv[2], atoi(argv[3]));
	if (argc >= 4 && strcmp(argv[1], "mkbig") == 0)
		return big(argv[2], atoi(argv[3]), 1);
	if (argc >= 4 && strcmp(argv[1], "ckbig") == 0)
		return big(argv[2], atoi(argv[3]), 0);
	if (argc >= 3 && strcmp(argv[1], "hold") == 0) {
		int fd = open(argv[2], O_RDONLY);

		if (fd < 0) {
			perror(argv[2]);
			return 1;
		}
		printf("holding\n");
		fflush(stdout);
		(void)pause();
		return 0;
	}
	fprintf(stderr, "usage: see the top of p9test.c\n");
	return 2;
}
