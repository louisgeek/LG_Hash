#!/usr/bin/env python3
"""比对 hashtest.exe 的输出与 reference.txt。

reference.txt 由标准库（zlib/hashlib）生成，与被测的 bcrypt 实现互不相关，
因此这是交叉验证而非自证。
"""
import os
import subprocess
import sys

HERE = os.path.dirname(os.path.abspath(__file__))
ROOT = os.path.dirname(HERE)
EXE = os.path.join(ROOT, "build", "tests", "hashtest.exe")

# UI 里显示的名字 -> reference.txt 里的算法键
KEY = {
    "CRC32": "CRC32",
    "MD5": "MD5",
    "SHA-1": "SHA1",
    "SHA-256": "SHA256",
    "SHA-512": "SHA512",
}
FILES = ["sample.txt", "large.bin"]


def load_reference():
    ref = {}
    path = os.path.join(HERE, "reference.txt")
    with open(path, encoding="utf-8-sig") as fh:
        for line in fh:
            parts = line.split()
            if len(parts) != 3:
                continue
            name, algo, digest = parts
            ref[(name, algo)] = digest
    return ref


def main() -> int:
    if not os.path.exists(EXE):
        print(f"missing {EXE}; run build first", file=sys.stderr)
        return 1
    if not os.path.exists(os.path.join(HERE, "reference.txt")):
        print("missing reference.txt; run make_reference.py first", file=sys.stderr)
        return 1

    ref = load_reference()
    failures = 0
    total = 0
    for name in FILES:
        target = os.path.join(HERE, name)
        if not os.path.exists(target):
            print(f"missing fixture {target}", file=sys.stderr)
            return 1
        proc = subprocess.run([EXE, target], capture_output=True, text=True, encoding="utf-8")
        if proc.returncode != 0:
            print(f"hashtest failed for {name}: {proc.stdout} {proc.stderr}", file=sys.stderr)
            return 1
        got = {}
        for line in proc.stdout.strip().splitlines():
            parts = line.split()
            if len(parts) == 2:
                got[parts[0]] = parts[1]
        for algo, refkey in KEY.items():
            total += 1
            want = ref.get((name, refkey))
            if want is None:
                print(f"MISSING reference for {name} {refkey}", file=sys.stderr)
                failures += 1
                continue
            if got.get(algo) != want:
                print(f"FAIL {name} {algo}: got {got.get(algo)} want {want}")
                failures += 1

    if failures:
        print(f"VALUE CHECK FAILED ({failures}/{total})")
        return 1
    print(f"VALUE CHECK PASS ({total}/{total})")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
