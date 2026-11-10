#!/usr/bin/env python3
"""Build a bundle-style Mac OS X installer package (.pkg) on the host, from a manifest.

    mkpkg.py COMPONENT.json --src DIR --out DIR

The format is the one 10.2 to 10.4 write and every later release still installs (flat packages, from
10.5, are unreadable before it): a directory Name.pkg/Contents holding Archive.pax.gz (an odc cpio
archive, gzipped), Archive.bom, Info.plist and Resources/ (Description.plist, Archive.sizes,
package_version and the preflight/postflight scripts). Layout checked against Apple's own
BSDSDK.pkg, DevSDK.pkg and DevTools.pkg of the December 2002 Developer Tools.

The archive is written here and not by pax/cpio, so every entry is root:wheel whoever runs this, and
the BOM is made from the same list with mkbom -i (macOS ships mkbom; the file needs the CRC that
cksum(1) prints, which is computed here). Python 3 standard library only.

Manifest (JSON):
  name, identifier, version, title, description   package name and what Installer shows
  restart      "none" (default), "required" or "recommended"   (IFPkgFlagRestartAction)
  files        [{"src": path under --src, "dest": absolute path in the guest, "mode": "0755"}, ...]
               "dest" alone with "type": "dir" makes a directory; parents get 0755 root:wheel
               ("owner": "root:admin" overrides, as the StartupItems use)
               or {"srcdir": dir under --src, "dest": ..., "filemode": "0644", "exec": [relative paths
               that get 0755]} for a whole tree (directories 0755; kexts and headers need nothing more)
  env MKPKG_BOM_VIA   a command `CMD Archive.pax.gz Archive.bom` that writes the BOM, in place of mkbom -i
  scripts      {"preflight": path, "postflight": path, ...} relative to the manifest
"""
import argparse, gzip, io, json, os, plistlib, shutil, subprocess, sys, tempfile, time

CRC_TABLE = []
for n in range(256):
    c = n << 24
    for _ in range(8):
        c = ((c << 1) ^ 0x04C11DB7) if c & 0x80000000 else c << 1
        c &= 0xFFFFFFFF
    CRC_TABLE.append(c)


def cksum(data):
    """POSIX cksum(1): the CRC a BOM stores for a file."""
    crc = 0
    for b in data:
        crc = ((crc << 8) & 0xFFFFFFFF) ^ CRC_TABLE[((crc >> 24) ^ b) & 0xFF]
    n = len(data)
    while n:
        crc = ((crc << 8) & 0xFFFFFFFF) ^ CRC_TABLE[((crc >> 24) ^ n) & 0xFF]
        n >>= 8
    return (~crc) & 0xFFFFFFFF


GIDS = {"wheel": 0, "admin": 80}


def owner(spec):
    u, _, g = spec.partition(":")
    if u != "root":
        sys.exit("mkpkg: only root-owned files are supported, not " + spec)
    return 0, GIDS[g or "wheel"]


MTIME = int(os.environ.get("SOURCE_DATE_EPOCH", 0)) or int(time.time())
# Installer decides whether a package is already installed, and so whether to unpack it over what is
# there, from the version in its package_version and Info.plist (IFMajorVersion/IFMinorVersion): a
# package of the same version as its receipt reports "The install was successful" and leaves every
# file as it was (live, 10.2.8, 2026-10-04: QemuGuestAgent and VirtioConsole kept their old binaries).
# The minor version is the build time in minutes, so a rebuilt package is always a newer one.
BUILD = MTIME // 60


def odc(path, mode, gid, data):
    """One entry of an odc ("070707") cpio archive, the pax format of Apple's packages."""
    name = path.encode() + b"\0"
    hdr = "070707" + "%06o" * 6 % (0, 0, mode, 0, gid, 1) + "%06o" % 0 + "%011o" % MTIME \
        + "%06o" % len(name) + "%011o" % len(data)
    return hdr.encode() + name + data


def read_manifest(path):
    with open(path) as f:
        m = json.load(f)
    for k in ("name", "identifier", "version"):
        if k not in m:
            sys.exit("mkpkg: %s: missing %s" % (path, k))
    return m


def entries(m, src):
    """[(path relative to /, mode incl. type bits, gid, bytes or None for a directory)], parents first."""
    out = {}
    for f in m.get("files", []):
        dest = f["dest"]
        if not dest.startswith("/"):
            sys.exit("mkpkg: dest must be absolute: " + dest)
        _, gid = owner(f.get("owner", "root:wheel"))
        parts = dest.strip("/").split("/")
        for i in range(1, len(parts)):  # parents
            out.setdefault("/".join(parts[:i]), (0o40755, 0, None))
        rel = "/".join(parts)
        if "srcdir" in f:
            base = os.path.join(src, f["srcdir"])
            execs = set(f.get("exec", []))
            out[rel] = (0o40755, gid, None)
            for dirpath, dirnames, filenames in os.walk(base):
                dirnames.sort()
                r = os.path.relpath(dirpath, base)
                for d in dirnames:
                    out[rel + "/" + os.path.normpath(os.path.join(r, d))] = (0o40755, gid, None)
                for fn in sorted(filenames):
                    if fn in ("PaxHeader", ".DS_Store"):
                        continue
                    sub = os.path.normpath(os.path.join(r, fn))
                    with open(os.path.join(dirpath, fn), "rb") as fh:
                        data = fh.read()
                    m_ = 0o755 if sub in execs else int(f.get("filemode", "0644"), 8)
                    out[rel + "/" + sub] = (0o100000 | m_, gid, data)
            continue
        if f.get("type") == "dir":
            out[rel] = (0o40000 | int(f.get("mode", "0755"), 8), gid, None)
        else:
            with open(os.path.join(src, f["src"]), "rb") as fh:
                data = fh.read()
            out[rel] = (0o100000 | int(f.get("mode", "0644"), 8), gid, data)
    return [("." + "/" + p, ) + out[p] for p in sorted(out)]


def build(manifest_path, src, outdir):
    m = read_manifest(manifest_path)
    here = os.path.dirname(os.path.abspath(manifest_path))
    ents = entries(m, src)
    root = os.path.join(outdir, m["name"] + ".pkg")
    shutil.rmtree(root, ignore_errors=True)
    res = os.path.join(root, "Contents", "Resources")
    os.makedirs(res)

    archive = io.BytesIO()
    bom_lines = [".\t40755\t0/0"]
    nfiles = installed = 0
    # The "." entry first, as in Apple's archives: Installer runs pax with the pattern "." and fails the
    # install ("These patterns were not matched") when the archive has no such member.
    archive.write(odc(".", 0o40755, 0, b""))
    for path, mode, gid, data in ents:
        archive.write(odc(path, mode, gid, data or b""))
        if data is None:
            bom_lines.append("%s\t%o\t0/%d" % (path, mode, gid))
        else:
            bom_lines.append("%s\t%o\t0/%d\t%d\t%d" % (path, mode, gid, len(data), cksum(data)))
            installed += len(data)
        nfiles += 1
    archive.write(odc("TRAILER!!!", 0, 0, b""))
    # mtime 0 and no name in the gzip header: the same inputs give the same package
    with open(os.path.join(root, "Contents", "Archive.pax.gz"), "wb") as raw:
        with gzip.GzipFile(filename="", mode="wb", compresslevel=9, fileobj=raw, mtime=0) as gz:
            gz.write(archive.getvalue())
    compressed = os.path.getsize(os.path.join(root, "Contents", "Archive.pax.gz"))

    bom = os.path.join(root, "Contents", "Archive.bom")
    via = os.environ.get("MKPKG_BOM_VIA")
    if via:
        # No mkbom -i here (Linux has no mkbom at all; 10.3's takes a directory only): the command is
        # given the finished archive and the BOM to write, and makes it where a mkbom is -- in a
        # PowerPC guest, as root, from the archive unpacked into a directory (guest/ladder/guest-mkbom.sh).
        subprocess.run([via, os.path.join(root, "Contents", "Archive.pax.gz"), bom], check=True)
    else:
        with tempfile.NamedTemporaryFile("w", suffix=".txt") as lst:
            lst.write("\n".join(bom_lines) + "\n")
            lst.flush()
            subprocess.run(["mkbom", "-i", lst.name, bom], check=True)

    restart = {"none": "None", "required": "RequiredRestart", "recommended": "RecommendedRestart"}[
        m.get("restart", "none")]
    info = {
        "CFBundleGetInfoString": "%s %s" % (m["name"], m["version"]),
        "CFBundleIdentifier": m["identifier"],
        "CFBundleName": m["name"],
        "CFBundleShortVersionString": m["version"],
        "IFMajorVersion": 1,
        "IFMinorVersion": BUILD,
        "IFPkgFlagAuthorizationAction": "RootAuthorization",
        "IFPkgFlagDefaultLocation": "/",
        "IFPkgFlagFollowLinks": True,
        "IFPkgFlagInstallFat": False,
        "IFPkgFlagIsRequired": False,
        # False: a directory already there keeps its owner and mode (/Library is root:admin 1775)
        "IFPkgFlagOverwritePermissions": False,
        "IFPkgFlagRelocatable": False,
        "IFPkgFlagRestartAction": restart,
        "IFPkgFlagRootVolumeOnly": True,
        "IFPkgFlagUpdateInstalledLanguages": False,
        "IFPkgFormatVersion": 0.1,
    }
    with open(os.path.join(root, "Contents", "Info.plist"), "wb") as f:
        plistlib.dump(info, f)
    desc = {
        "IFPkgDescriptionDescription": m.get("description", ""),
        "IFPkgDescriptionTitle": m.get("title", m["name"]),
        "IFPkgDescriptionVersion": m["version"],
    }
    with open(os.path.join(res, "Description.plist"), "wb") as f:
        plistlib.dump(desc, f)
    with open(os.path.join(res, "Archive.sizes"), "w") as f:
        f.write("NumFiles %d\nInstalledSize %d\nCompressedSize %d\n"
                % (nfiles, (installed + 1023) // 1024, (compressed + 1023) // 1024))
    with open(os.path.join(res, "package_version"), "w") as f:
        f.write("Major:\t1\nMinor:\t%d\n" % BUILD)
    for name, rel in m.get("scripts", {}).items():
        dst = os.path.join(res, name)
        shutil.copyfile(os.path.join(here, rel), dst)
        os.chmod(dst, 0o755)
    return root


def main():
    ap = argparse.ArgumentParser(description=__doc__.split("\n")[0])
    ap.add_argument("manifest", nargs="+")
    ap.add_argument("--src", required=True, help="directory the manifest's src paths are relative to")
    ap.add_argument("--out", required=True, help="directory to write Name.pkg into")
    a = ap.parse_args()
    os.makedirs(a.out, exist_ok=True)
    for mf in a.manifest:
        print(build(mf, a.src, a.out))


if __name__ == "__main__":
    main()
