/*
 * mount_9p: mounts a QEMU virtio 9P share, Virtio9P.kext's "9p" filesystem.
 *
 *   mount_9p [-o options] [-u uid] [-g gid] [-a aname] [-n uname] tag node
 *
 * tag is the device's mount tag (QEMU's -device virtio-9p-pci,mount_tag=...). mount(8) runs it
 * for "mount -t 9p tag node", passing -o. Options: ro, rw, nosuid, nodev, noexec, and
 * uid=, gid=, aname=, uname= as the flags. Plain C for GCC 2.95 and 10.0's libc.
 */

#include <sys/param.h>
#include <sys/mount.h>
#include <errno.h>
#include <grp.h>
#include <pwd.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

#include "p9fs_args.h"

static const char *prog = "mount_9p";

static void
usage(void)
{
    fprintf(stderr, "usage: %s [-o options] [-u uid] [-g gid] [-a aname] [-n uname] tag node\n", prog);
    exit(64);
}

static void
copyarg(char *dst, const char *src, size_t size, const char *what)
{
    if (strlen(src) >= size) {
        fprintf(stderr, "%s: %s is longer than %d bytes\n", prog, what, (int) size - 1);
        exit(64);
    }
    strcpy(dst, src);
}

static unsigned int
parseuid(const char *s)
{
    struct passwd *pw;
    char *end;
    unsigned long n = strtoul(s, &end, 10);

    if (*s && !*end)
        return (unsigned int) n;
    if ((pw = getpwnam(s)) == NULL) {
        fprintf(stderr, "%s: no user %s\n", prog, s);
        exit(67);
    }
    return pw->pw_uid;
}

static unsigned int
parsegid(const char *s)
{
    struct group *gr;
    char *end;
    unsigned long n = strtoul(s, &end, 10);

    if (*s && !*end)
        return (unsigned int) n;
    if ((gr = getgrnam(s)) == NULL) {
        fprintf(stderr, "%s: no group %s\n", prog, s);
        exit(67);
    }
    return gr->gr_gid;
}

/* -o: a comma-separated list. Anything unknown is an error rather than ignored. */
static void
parseopts(char *list, int *mntflags, struct p9fs_args *args)
{
    char *o, *next;

    for (o = list; o && *o; o = next) {
        next = strchr(o, ',');
        if (next)
            *next++ = 0;
        if (strcmp(o, "ro") == 0 || strcmp(o, "rdonly") == 0)
            *mntflags |= MNT_RDONLY;
        else if (strcmp(o, "rw") == 0)
            *mntflags &= ~MNT_RDONLY;
        else if (strcmp(o, "nosuid") == 0)
            *mntflags |= MNT_NOSUID;
        else if (strcmp(o, "nodev") == 0)
            *mntflags |= MNT_NODEV;
        else if (strcmp(o, "noexec") == 0)
            *mntflags |= MNT_NOEXEC;
        else if (strcmp(o, "auto") == 0 || strcmp(o, "noauto") == 0)
            ;   /* fstab's, not ours */
        else if (strncmp(o, "uid=", 4) == 0) {
            args->uid = parseuid(o + 4);
            args->flags |= P9FS_MNT_UID;
        } else if (strncmp(o, "gid=", 4) == 0) {
            args->gid = parsegid(o + 4);
            args->flags |= P9FS_MNT_GID;
        } else if (strncmp(o, "aname=", 6) == 0)
            copyarg(args->aname, o + 6, sizeof(args->aname), "aname");
        else if (strncmp(o, "uname=", 6) == 0)
            copyarg(args->uname, o + 6, sizeof(args->uname), "uname");
        else {
            fprintf(stderr, "%s: unknown option %s\n", prog, o);
            exit(64);
        }
    }
}

int
main(int argc, char **argv)
{
    struct p9fs_args args;
    char node[MAXPATHLEN];
    int ch, mntflags = 0;

    memset(&args, 0, sizeof(args));
    args.version = P9FS_ARGSVERSION;
    strcpy(args.uname, "root");
    args.n_uname = getuid();

    while ((ch = getopt(argc, argv, "o:u:g:a:n:")) != -1) {
        switch (ch) {
        case 'o':
            parseopts(optarg, &mntflags, &args);
            break;
        case 'u':
            args.uid = parseuid(optarg);
            args.flags |= P9FS_MNT_UID;
            break;
        case 'g':
            args.gid = parsegid(optarg);
            args.flags |= P9FS_MNT_GID;
            break;
        case 'a':
            copyarg(args.aname, optarg, sizeof(args.aname), "aname");
            break;
        case 'n':
            copyarg(args.uname, optarg, sizeof(args.uname), "uname");
            break;
        default:
            usage();
        }
    }
    argc -= optind;
    argv += optind;
    if (argc != 2)
        usage();
    copyarg(args.tag, argv[0], sizeof(args.tag), "the mount tag");
    if (realpath(argv[1], node) == NULL) {
        fprintf(stderr, "%s: %s: %s\n", prog, argv[1], strerror(errno));
        exit(71);
    }

    if (mount(P9FS_NAME, node, mntflags, &args) == 0)
        return 0;
    switch (errno) {
    case ENODEV:
    case EOPNOTSUPP:
        fprintf(stderr, "%s: the 9p filesystem is not in the kernel; is Virtio9P.kext loaded?\n", prog);
        break;
    case ENXIO:
        fprintf(stderr, "%s: no virtio 9P device has the mount tag \"%s\"\n", prog, args.tag);
        break;
    case EBUSY:
        fprintf(stderr, "%s: %s is mounted already, or %s is busy\n", prog, args.tag, node);
        break;
    default:
        fprintf(stderr, "%s: %s on %s: %s\n", prog, args.tag, node, strerror(errno));
        break;
    }
    exit(71);
}
