#!/usr/bin/env python3
"""Build and run the kernel's standalone MD routines on the ISS.

Some machine-dependent kernel routines touch nothing but registers and
sysregs, so they can run outside the kernel.  Each test here assembles
such a routine from netbsd/sys/arch/penumbra/ exactly as the kernel
build does (-D_KERNEL -D_LOCORE, <machine/...> from the port's include
directory), links it with a C driver on the compiler-test harness (crt0
and linker script, no libc), and runs the result on the ISS in hosted
mode, where it starts in supervisor mode with the MMU off.  Exit status
0 means every check passed.
"""

import argparse
import os
import shlex
import subprocess
import sys

# test name -> the port's sources it exercises, relative to --port-dir
TESTS = {
    "tlb": ["penumbra/tlb_subr.S"],
}


def run(cmd):
    result = subprocess.run(cmd, capture_output=True, text=True)
    if result.returncode != 0:
        sys.exit("kernel-test: %s failed:\n%s%s" % (cmd[0], result.stdout, result.stderr))


def main():
    ap = argparse.ArgumentParser(description=__doc__.splitlines()[0])
    ap.add_argument("--cc", required=True)
    ap.add_argument("--objcopy", required=True)
    ap.add_argument("--iss", required=True)
    ap.add_argument("--bin2hex", required=True)
    ap.add_argument("--harness-dir", required=True)
    ap.add_argument("--port-dir", required=True, help="netbsd/sys/arch/penumbra")
    ap.add_argument("--build-dir", required=True)
    args = ap.parse_args()

    here = os.path.dirname(os.path.abspath(__file__))
    os.makedirs(args.build_dir, exist_ok=True)
    out = lambda name: os.path.join(args.build_dir, name)

    # <machine/*.h> resolves to the port's headers, as in the kernel build.
    incdir = out("include")
    os.makedirs(incdir, exist_ok=True)
    machine = os.path.join(incdir, "machine")
    if not os.path.islink(machine):
        os.symlink(os.path.abspath(os.path.join(args.port_dir, "include")), machine)

    cc = shlex.split(args.cc)
    resource_dir = subprocess.run(cc + ["-print-resource-dir"], capture_output=True,
                                  text=True, check=True).stdout.strip()
    flags = cc + ["-O2", "-fno-builtin", "-nostdlib", "-nostdinc",
                  "-isystem", os.path.join(resource_dir, "include"), "-I", incdir,
                  "-std=c99", "-Wall", "-Werror"]

    failed = 0
    for name, sources in TESTS.items():
        objs = []
        for src, obj in ((os.path.join(args.harness_dir, "crt0.S"), "crt0.o"),
                         (os.path.join(args.harness_dir, "hosted.S"), "hosted.o"),
                         (os.path.join(here, name + "-test.c"), name + "-test.o")):
            run(flags + ["-c", src, "-o", out(obj)])
            objs.append(out(obj))
        for src in sources:
            obj = out(os.path.basename(src) + ".o")
            run(flags + ["-D_KERNEL", "-D_LOCORE", "-c",
                         os.path.join(args.port_dir, src), "-o", obj])
            objs.append(obj)

        elf, binary, hexfile = out(name + ".elf"), out(name + ".bin"), out(name + ".hex")
        run(cc + ["-ffreestanding", "-nostdlib",
                  "-T", os.path.join(args.harness_dir, "test.ld")] + objs + ["-o", elf])
        run(shlex.split(args.objcopy) + ["-O", "binary", elf, binary])
        run([sys.executable, args.bin2hex, binary, "-o", hexfile])

        result = subprocess.run([args.iss, hexfile, "+hosted", "+quiet", "+max-insn=100000000"],
                                capture_output=True, text=True)
        sys.stdout.write(result.stdout)
        if result.returncode != 0:
            sys.stderr.write(result.stderr[-2000:])
            failed += 1
    sys.exit(1 if failed else 0)


if __name__ == "__main__":
    main()
