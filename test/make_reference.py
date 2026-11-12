#!/usr/bin/env python3
"""生成测试参考值：用 Python 标准库独立实现，不复用被测代码。

CRC32 用 zlib，MD5/SHA 用 hashlib，与 HashPage 内部的 bcrypt 实现互不相关，
因此可作为交叉验证的基准。
"""
import hashlib
import os
import sys
import zlib

HERE = os.path.dirname(os.path.abspath(__file__))
FILES = ["sample.txt", "large.bin"]
ALGOS = [
    ("CRC32", lambda d: format(zlib.crc32(d) & 0xFFFFFFFF, "08x")),
    ("MD5", lambda d: hashlib.md5(d).hexdigest()),
    ("SHA1", lambda d: hashlib.sha1(d).hexdigest()),
    ("SHA256", lambda d: hashlib.sha256(d).hexdigest()),
    ("SHA512", lambda d: hashlib.sha512(d).hexdigest()),
]


def main() -> int:
    lines = []
    for name in FILES:
        path = os.path.join(HERE, name)
        if not os.path.exists(path):
            print(f"missing fixture: {path}", file=sys.stderr)
            return 1
        with open(path, "rb") as fh:
            data = fh.read()
        for algo, fn in ALGOS:
            lines.append(f"{name} {algo} {fn(data)}")

    out = os.path.join(HERE, "reference.txt")
    with open(out, "w", encoding="utf-8", newline="\n") as fh:
        fh.write("\n".join(lines) + "\n")
    print(f"wrote {out} ({len(lines)} entries)")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
