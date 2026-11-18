#!/usr/bin/env python3
"""Writes a tiny synthetic Alpine v2 package, for the release smoke test.

    python tests/make_smoke_package.py smoke.apk

The package contains the minimum the v2 reader needs (a `.PKGINFO` and one file), so
a build can be checked end to end without any sample packages or the aapt2 toolchain.
"""
import io
import sys
import tarfile

PKGINFO = b"""pkgname = smoke-test
pkgver = 1.0.0-r0
pkgdesc = synthetic package used by the release smoke test
url = https://example.invalid/smoke
builddate = 1763500000
packager = CI
size = 2
arch = x86_64
license = MIT
origin = smoke-test
"""


def main(argv):
    target = argv[1] if len(argv) > 1 else "smoke.apk"
    with tarfile.open(target, "w:gz") as archive:
        def add(name, data):
            info = tarfile.TarInfo(name)
            info.size = len(data)
            info.mtime = 1763500000
            archive.addfile(info, io.BytesIO(data))

        add(".PKGINFO", PKGINFO)
        add("usr/bin/hello", b"hi")
    print("wrote %s" % target)
    return 0


if __name__ == "__main__":
    sys.exit(main(sys.argv))
