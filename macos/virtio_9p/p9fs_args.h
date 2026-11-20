/*
 * What mount_9p hands mount(2) for the "9p" filesystem. Shared by the kernel and mount_9p; plain
 * fixed-size fields, so that GCC 2.95 and 3 lay it out alike.
 */

#ifndef _P9FS_ARGS_H
#define _P9FS_ARGS_H

#define P9FS_NAME           "9p"
#define P9FS_ARGSVERSION    1

#define P9FS_TAGLEN         64
#define P9FS_ANAMELEN       256
#define P9FS_UNAMELEN       32

/* p9fs_args.flags */
#define P9FS_MNT_UID        0x0001      /* present every file as owned by uid */
#define P9FS_MNT_GID        0x0002      /* ... and by group gid */

struct p9fs_args {
    int             version;                /* P9FS_ARGSVERSION */
    char            tag[P9FS_TAGLEN];       /* the device's mount tag */
    char            aname[P9FS_ANAMELEN];   /* the tree to attach to, "" for the export's root */
    char            uname[P9FS_UNAMELEN];   /* the user to attach as */
    unsigned int    n_uname;                /* ... and by number */
    int             flags;
    unsigned int    uid;
    unsigned int    gid;
};

#endif /* !_P9FS_ARGS_H */
