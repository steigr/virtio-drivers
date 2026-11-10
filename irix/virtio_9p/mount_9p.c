/*
 * mount(1M)'s helper for -t 9p, which it runs as:
 *
 *	mount_9p tag dir 9p options
 *
 * The tag is the one the device was configured with.  Options are mount's, of
 * which ro, nosuid and nodev are the kernel's flags; the rest go to the kernel
 * as they are, which reads aname=dir.  Records the mount in /etc/mtab, as a
 * helper has to.
 */
#include <sys/types.h>
#include <sys/mount.h>
#include <sys/fstyp.h>
#include <sys/fsid.h>
#include <errno.h>
#include <mntent.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

#define TYPE	"9p"
#define MTAB	"/etc/mtab"

static int
has_opt(const char *opts, const char *name)
{
	size_t n = strlen(name);
	const char *p = opts;

	while (p != NULL && *p != 0) {
		if (strncmp(p, name, n) == 0 && (p[n] == 0 || p[n] == ','))
			return 1;
		p = strchr(p, ',');
		if (p != NULL)
			p++;
	}
	return 0;
}

static void
record(const char *tag, const char *dir, const char *opts)
{
	struct mntent m;
	FILE *f;

	if ((f = setmntent(MTAB, "a")) == NULL)
		return;
	m.mnt_fsname = (char *)tag;
	m.mnt_dir = (char *)dir;
	m.mnt_type = TYPE;
	m.mnt_opts = (char *)opts;
	m.mnt_freq = 0;
	m.mnt_passno = 0;
	(void)addmntent(f, &m);
	(void)endmntent(f);
}

int
main(int argc, char **argv)
{
	const char *tag, *dir, *opts;
	int fstype, flags = MS_DATA;

	if (argc < 3) {
		fprintf(stderr, "usage: mount_9p tag dir [type [options]]\n");
		return 2;
	}
	tag = argv[1];
	dir = argv[2];
	opts = argc > 4 ? argv[4] : "rw";
	if ((fstype = sysfs(GETFSIND, TYPE)) < 0) {
		fprintf(stderr, "mount_9p: the %s module is not loaded\n", TYPE);
		return 1;
	}
	if (has_opt(opts, "ro"))
		flags |= MS_RDONLY;
	if (has_opt(opts, "nosuid"))
		flags |= MS_NOSUID;
	if (has_opt(opts, "nodev"))
		flags |= MS_NODEV;
	if (mount(tag, dir, flags, (char *)fstype, opts, (int)strlen(opts) + 1) != 0) {
		fprintf(stderr, "mount_9p: %s on %s: %s\n", tag, dir, strerror(errno));
		return 1;
	}
	record(tag, dir, opts);
	return 0;
}
