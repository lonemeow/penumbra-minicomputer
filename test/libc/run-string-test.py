#!/usr/bin/env python3
"""Build and run the libc string-routine test on the ISS.

Assembles the hand-written routines from
netbsd/common/lib/libc/arch/penumbra/string/ with each symbol renamed to
pen_<name>, links them with string-test.c on the compiler-test harness
(crt0 and linker script, no libc), and runs the result in hosted mode.
Exit status 0 means every check passed.
"""

import argparse
import os
import shlex
import subprocess
import sys

ROUTINES = ["memcpy", "memset", "memcmp", "strlen", "strcmp", "strcpy"]


def run(cmd):
    result = subprocess.run(cmd, capture_output=True, text=True)
    if result.returncode != 0:
        sys.exit("string-test: %s failed:\n%s%s" % (cmd[0], result.stdout, result.stderr))


def main():
    ap = argparse.ArgumentParser(description=__doc__.splitlines()[0])
    ap.add_argument("--cc", required=True)
    ap.add_argument("--objcopy", required=True)
    ap.add_argument("--iss", required=True)
    ap.add_argument("--bin2hex", required=True)
    ap.add_argument("--harness-dir", required=True)
    ap.add_argument("--src-dir", required=True, help="directory holding the routines' .S files")
    ap.add_argument("--build-dir", required=True)
    args = ap.parse_args()

    here = os.path.dirname(os.path.abspath(__file__))
    os.makedirs(args.build_dir, exist_ok=True)
    out = lambda name: os.path.join(args.build_dir, name)

    cc = shlex.split(args.cc)
    # No libc headers: stddef.h comes from the compiler's own resource
    # directory.
    resource_dir = subprocess.run(cc + ["-print-resource-dir"], capture_output=True,
                                  text=True, check=True).stdout.strip()
    flags = cc + ["-O2", "-fno-builtin", "-nostdlib", "-nostdinc",
                  "-isystem", os.path.join(resource_dir, "include"),
                  "-std=c99", "-Wall", "-Werror"]
    renames = ["-D%s=pen_%s" % (r, r) for r in ROUTINES]

    objs = []
    for src, obj in ((os.path.join(args.harness_dir, "crt0.S"), "crt0.o"),
                     (os.path.join(args.harness_dir, "hosted.S"), "hosted.o"),
                     (os.path.join(here, "string-test.c"), "string-test.o")):
        run(flags + ["-c", src, "-o", out(obj)])
        objs.append(out(obj))
    for r in ROUTINES:
        run(flags + renames + ["-c", os.path.join(args.src_dir, r + ".S"),
                               "-o", out(r + ".o")])
        objs.append(out(r + ".o"))

    elf, binary, hexfile = out("string-test.elf"), out("string-test.bin"), out("string-test.hex")
    run(cc + ["-ffreestanding", "-nostdlib",
              "-T", os.path.join(args.harness_dir, "test.ld")] + objs + ["-o", elf])
    run(shlex.split(args.objcopy) + ["-O", "binary", elf, binary])
    run([sys.executable, args.bin2hex, binary, "-o", hexfile])

    result = subprocess.run([args.iss, hexfile, "+hosted", "+quiet", "+max-insn=2000000000"],
                            capture_output=True, text=True)
    sys.stdout.write(result.stdout)
    if result.returncode != 0:
        sys.stderr.write(result.stderr[-2000:])
        sys.exit(1)


if __name__ == "__main__":
    main()
