#!/usr/bin/env python3
"""Build the zstd-compressed v3 container fixtures used by test.cpp.

Both variants carry the *same* adb stream as the real example package, only the
container marker differs:
    "ADBz" + zstd frame            (defensive marker, no alg/level bytes)
    "ADBc" + [alg=2, level] + frame (apk-tools' own custom spec)
Requires the msys64 zstd CLI (D:\\SDK\\msys64\\ucrt64\\bin\\zstd.exe).
"""
import os
import subprocess
import zlib

HERE = os.path.dirname(os.path.abspath(__file__))
APK = os.path.join(HERE, "..", "..", "..", "example", "OpenWrt_alpine-busybox-1.37.0-r6.apk")
ZSTD = r"D:\SDK\msys64\ucrt64\bin\zstd.exe"


def main():
    raw = open(APK, "rb").read()
    stream = zlib.decompressobj(-15).decompress(raw[4:])
    plain = os.path.join(HERE, "adbstream.bin")
    packed = os.path.join(HERE, "adbstream.zst")
    open(plain, "wb").write(stream)
    subprocess.check_call([ZSTD, "-q", "-19", "-f", plain, "-o", packed])
    frame = open(packed, "rb").read()
    open(os.path.join(HERE, "synthetic_v3_zstd.apk"), "wb").write(b"ADBz" + frame)
    open(os.path.join(HERE, "synthetic_v3_zstc.apk"), "wb").write(b"ADBc" + bytes([2, 19]) + frame)
    os.remove(plain)
    os.remove(packed)
    print("adb stream %d bytes -> zstd frame %d bytes, two containers written" %
          (len(stream), len(frame)))


if __name__ == "__main__":
    main()
