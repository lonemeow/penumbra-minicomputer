#!/usr/bin/env python3
import os
import sys
import subprocess
import multiprocessing
import argparse
import time
import shlex
import re
import fnmatch
from concurrent.futures import ProcessPoolExecutor

# ANSI color codes
GREEN = "\033[32m"
RED = "\033[31m"
YELLOW = "\033[33m"
RESET = "\033[0m"

class TestResult:
    def __init__(self, name, display_name, success, reason, output=""):
        self.name = name
        self.display_name = display_name
        self.success = success
        self.reason = reason
        self.output = output

# Per-test flags from GCC `{ dg-options "..." }` and
# `{ dg-additional-options "..." }` directives.  We only forward
# flags that clang understands and that change semantics the test
# legitimately depends on — wrapping overflow, aliasing model,
# dialect, inline semantics, etc.  GCC-only flags (-ftree-*,
# -fexpensive-optimizations), target selection (-mno-mmx), and
# warning flags (redundant with `-w`) are ignored.
DG_SAFE_FLAGS = {
    "-fwrapv",
    "-fno-strict-overflow",
    "-fno-strict-aliasing",
    "-fgnu89-inline",
    "-fnon-call-exceptions",
    "-fno-inline",
    "-fno-common",
    "-fsigned-char",
    "-funsigned-char",
    # -ffast-math is intentionally excluded: it makes clang pattern-match
    # a<b?a:b into llvm.minnum/maxnum, which lower to fmin/fmax libcalls
    # our harness doesn't stub.  The tests using it pass fine without.
}
DG_SAFE_PREFIXES = ("-fno-builtin", "-std=", "-finput-charset=")

# Match `{ dg-options "..." }` or `{ dg-additional-options "..." }`
# (with optional inner braces) and capture both the flag body and
# any trailing `{ target ... }` qualifier so we can skip
# architecture-conditional forms.  For our harness the two
# directives behave identically: GCC's dg-options replaces the
# default set while dg-additional-options appends, but we have no
# dg-provided default to replace, so both simply append.
DG_OPTIONS_RE = re.compile(
    r'\{\s*dg-(?:additional-)?options\s+(?:\{\s*)?"([^"]*)"(?:\s*\})?\s*(\{[^}]*\})?',
)

def extract_dg_options(source_path):
    try:
        with open(source_path, "r", errors="replace") as f:
            text = f.read(4096)
    except OSError:
        return []
    flags = []
    for m in DG_OPTIONS_RE.finditer(text):
        qualifier = m.group(2) or ""
        if "target" in qualifier:
            continue  # architecture-specific, not for us
        for tok in m.group(1).split():
            if tok in DG_SAFE_FLAGS or tok.startswith(DG_SAFE_PREFIXES):
                flags.append(tok)
    return flags

def run_single_test(args):
    test_path, opt, harness_dir, build_dir, iss_path, cc, objcopy, bin2hex, builtins, resource_dir, display_name, override_flags = args
    # Use a sanitized name for filesystem paths
    name = display_name.replace("/", "_").replace(".", "_")
    
    # We create a per-test sub-directory to avoid object name collisions
    test_build_dir = os.path.join(build_dir, name)
    os.makedirs(test_build_dir, exist_ok=True)
    
    obj_crt0 = os.path.join(test_build_dir, "crt0.o")
    obj_libc = os.path.join(test_build_dir, "libc_stub.o")
    obj_test = os.path.join(test_build_dir, "test.o")
    elf_file = os.path.join(test_build_dir, "test.elf")
    hex_file = os.path.join(test_build_dir, "test.hex")
    bin_file = os.path.join(test_build_dir, "test.bin")
    
    common_flags = shlex.split(cc) + [
        opt, "-ffreestanding", "-nostdlib", "-nostdinc",
        "-I", harness_dir, "-w",
        "-std=gnu89",
        "-Wno-implicit-int",
        "-Wno-implicit-function-declaration",
        "-Wno-return-type",
    ]
    if resource_dir:
        common_flags += ["-isystem", os.path.join(resource_dir, "include")]

    # 1. Compile crt0.S
    try:
        subprocess.run(common_flags + ["-c", os.path.join(harness_dir, "crt0.S"), "-o", obj_crt0], 
                       check=True, capture_output=True, text=True)
    except subprocess.CalledProcessError as e:
        return TestResult(name, display_name, False, "crt0.S compile error", e.stderr)

    # 2. Compile libc_stub.c
    try:
        subprocess.run(common_flags + ["-c", os.path.join(harness_dir, "libc_stub.c"), "-o", obj_libc], 
                       check=True, capture_output=True, text=True)
    except subprocess.CalledProcessError as e:
        return TestResult(name, display_name, False, "libc_stub.c compile error", e.stderr)

    # 3. Compile test.c — honor safe `dg-options` flags from the source
    # (things like -fwrapv or -fgnu89-inline that tests explicitly
    # request), plus any per-test overrides from the flags file (for
    # tests that need flags their source doesn't declare, e.g. -std=gnu99
    # for tests that rely on C99 constant promotion or scoping).
    # Appended after common_flags so they override defaults.
    dg_flags = extract_dg_options(test_path)
    cmd_test = common_flags + [
        "-include", "stdlib.h",
        "-include", "stdio.h",
        "-include", "string.h",
        "-include", "alloca.h",
    ] + dg_flags + list(override_flags) + [
        "-c", test_path, "-o", obj_test
    ]
    try:
        subprocess.run(cmd_test, check=True, capture_output=True, text=True)
    except subprocess.CalledProcessError as e:
        return TestResult(name, display_name, False, "compile error", e.stderr)

    # 4. Link
    cmd_link = shlex.split(cc) + [
        "-target", "penumbra-unknown-none", # Ensure target is passed for linking
        "-ffreestanding", "-nostdlib",
        "-T", os.path.join(harness_dir, "test.ld"),
        obj_crt0, obj_libc, obj_test
    ]
    if builtins:
        cmd_link.append(builtins)
    cmd_link += ["-o", elf_file]
    
    try:
        subprocess.run(cmd_link, check=True, capture_output=True, text=True)
    except subprocess.CalledProcessError as e:
        return TestResult(name, display_name, False, "link error", e.stderr)

    # 5. Objcopy + Bin2Hex
    try:
        subprocess.run(shlex.split(objcopy) + ["-O", "binary", elf_file, bin_file], check=True, capture_output=True)
        subprocess.run([sys.executable, bin2hex, bin_file, "-o", hex_file], check=True, capture_output=True)
    except subprocess.CalledProcessError as e:
        return TestResult(name, display_name, False, "objcopy/bin2hex error", e.stderr)

    # 6. Run in ISS
    # Bound execution by instruction count (deterministic failure on
    # runaway loops) and by wall-clock (catches ISS hangs).  Some torture
    # tests iterate 10k+ times over i64 soft-libcalls, which balloons to
    # ~10k instructions per iteration; 2B gives enough headroom without
    # letting true infinite loops waste all 120s of wallclock.
    cmd_iss = [iss_path, hex_file, "+hosted", "+quiet", "+max-insn=2000000000"]
    try:
        proc = subprocess.run(cmd_iss, capture_output=True, text=True, timeout=120)
        actual_output = proc.stdout
        exit_code = proc.returncode
        # llvm-test-suite expects the exit code to be printed at the end of the output
        actual_output += f"exit {exit_code}\n"
    except subprocess.TimeoutExpired:
        return TestResult(name, display_name, False, "timeout (120s)")
    except Exception as e:
        return TestResult(name, display_name, False, "simulator error", str(e))

    # 4. Verify output if reference exists
    ref_path = os.path.splitext(test_path)[0] + ".reference_output"
    if os.path.exists(ref_path):
        with open(ref_path, "r") as f:
            expected_output = f.read()
        
        expected_lines = [l.strip() for l in expected_output.strip().splitlines() if l.strip()]
        actual_lines = [l.strip() for l in actual_output.strip().splitlines() if l.strip()]
        
        # Normalized comparison: check if all non-empty expected lines exist 
        # in actual output in the same relative order.
        found_all = True
        actual_idx = 0
        for exp_line in expected_lines:
            match_found = False
            for i in range(actual_idx, len(actual_lines)):
                if exp_line == actual_lines[i]:
                    actual_idx = i + 1
                    match_found = True
                    break
            if not match_found:
                found_all = False
                break
        
        if not found_all:
            mismatch_info = f"--- Expected (essential lines) ---\n" + "\n".join(expected_lines) + \
                            f"\n\n--- Actual ---\n{actual_output}"
            return TestResult(name, display_name, False, "output mismatch", mismatch_info)

    if exit_code != 0:
        return TestResult(name, display_name, False, f"exit code {exit_code}", actual_output)

    return TestResult(name, display_name, True, "pass", actual_output)

def main():
    parser = argparse.ArgumentParser(description="Penumbra Compiler Test Runner")
    parser.add_argument("--test-dir", action="append", default=[], help="Directory containing tests")
    parser.add_argument("--test-file", action="append", default=[], help="Specific test file (repeatable). When set, skips --test-dir walking.")
    parser.add_argument("--opt", default="-O2", help="Optimization level")
    parser.add_argument("--harness-dir", required=True, help="Harness directory")
    parser.add_argument("--build-dir", required=True, help="Build directory")
    parser.add_argument("--iss", required=True, help="Path to penumbra-iss")
    parser.add_argument("--cc", required=True, help="Path to clang")
    parser.add_argument("--objcopy", required=True, help="Path to llvm-objcopy")
    parser.add_argument("--bin2hex", required=True, help="Path to bin2hex.py")
    parser.add_argument("--builtins", help="Path to libclang_rt.builtins-penumbra.a")
    parser.add_argument("--resource-dir", help="Clang resource directory")
    parser.add_argument("--exclude", action="append", default=[], help="Glob pattern to exclude tests (repeatable)")
    parser.add_argument("--exclude-file", action="append", default=[], help="File listing exclude patterns, one per line (# comments allowed)")
    parser.add_argument("--flags-file", action="append", default=[], help="File of per-test flag overrides: '<pattern>  <flags...>' per line")
    parser.add_argument("--report", default="test-report.txt", help="Report file path")
    parser.add_argument("-j", "--jobs", type=int, default=multiprocessing.cpu_count(), help="Number of parallel jobs")
    
    args = parser.parse_args()

    if not args.test_dir and not args.test_file:
        parser.error("at least one --test-dir or --test-file is required")

    # Merge --exclude-file contents into args.exclude.  One pattern per
    # line; blank lines and lines starting with '#' are ignored.
    # Inline comments (`pattern.c   # note`) are stripped from the tail.
    for ef in args.exclude_file:
        with open(ef) as fh:
            for line in fh:
                # Strip inline comment, then whitespace.  '#' never
                # appears in glob patterns, so this is unambiguous.
                hash_at = line.find("#")
                if hash_at >= 0:
                    line = line[:hash_at]
                line = line.strip()
                if line:
                    args.exclude.append(line)

    # Load flags-file entries into a list of (pattern, [flags...]).
    # Matched against rel_path with the same gitignore-style suffix
    # logic as excludes.
    flag_rules = []
    for ff in args.flags_file:
        with open(ff) as fh:
            for line in fh:
                hash_at = line.find("#")
                if hash_at >= 0:
                    line = line[:hash_at]
                toks = line.split()
                if len(toks) >= 2:
                    flag_rules.append((toks[0], toks[1:]))

    os.makedirs(args.build_dir, exist_ok=True)

    test_data = []
    # --test-file mode: run only the specified files, bypassing directory
    # walking and exclusion patterns.  Used by `make test-compiler
    # COMPILER_TESTS=...` to target individual tests.
    for f in args.test_file:
        full_path = os.path.abspath(f)
        # display_name is relative to cwd so the report is readable
        rel_path = os.path.relpath(full_path)
        test_data.append((full_path, rel_path))

    for d in args.test_dir:
        # Find absolute path of test dir to calculate relative paths correctly
        abs_test_dir = os.path.abspath(d)
        parent_dir = os.path.dirname(abs_test_dir)
        
        for root, _, files in os.walk(d):
            for f in files:
                if f.endswith(".c"):
                    full_path = os.path.abspath(os.path.join(root, f))
                    
                    # display_name is path relative to the test root's parent
                    # e.g. llvm-test-suite/UnitTests/test.c
                    rel_path = os.path.relpath(full_path, parent_dir)
                    
                    # Check exclusions.  Patterns match against any tail
                    # of rel_path (gitignore-style "match at any depth"):
                    # "Stanford/FloatMM.c" fires for both "Stanford/FloatMM.c"
                    # and "Benchmarks/Stanford/FloatMM.c"; "*.c" fires for
                    # any .c file.  Equivalent to checking fnmatch against
                    # rel_path itself and every suffix down to the basename.
                    excluded = False
                    parts = rel_path.split(os.sep)
                    for pattern in args.exclude:
                        if any(fnmatch.fnmatch(os.sep.join(parts[i:]), pattern)
                               for i in range(len(parts))):
                            excluded = True
                            break

                    if not excluded:
                        test_data.append((full_path, rel_path))
    
    test_data.sort(key=lambda x: x[1])
    print(f"Found {len(test_data)} tests")
    print(f"Running with {args.jobs} parallel jobs at {args.opt}...")

    def match_flags(rel_path):
        """Collect flags from every flag-rule pattern matching rel_path."""
        parts = rel_path.split(os.sep)
        hits = []
        for pattern, flags in flag_rules:
            if any(fnmatch.fnmatch(os.sep.join(parts[i:]), pattern)
                   for i in range(len(parts))):
                hits.extend(flags)
        return tuple(hits)

    worker_args = [
        (full_path, args.opt, args.harness_dir, args.build_dir, args.iss, args.cc, args.objcopy, args.bin2hex, args.builtins, args.resource_dir, rel_path, match_flags(rel_path))
        for full_path, rel_path in test_data
    ]

    results = []
    passed = 0
    failed = 0

    start_time = time.time()
    
    # ProcessPoolExecutor for parallelism
    with ProcessPoolExecutor(max_workers=args.jobs) as executor:
        for result in executor.map(run_single_test, worker_args):
            if result.success:
                print(f"  {GREEN}PASS{RESET}  {result.display_name}")
                passed += 1
            else:
                print(f"  {RED}FAIL{RESET}  {result.display_name} ({result.reason})")
                failed += 1
            results.append(result)

    duration = time.time() - start_time
    
    # Write report
    with open(args.report, "w") as f:
        f.write(f"Penumbra Compiler Test Report\n")
        f.write(f"=============================\n")
        f.write(f"Date: {time.ctime()}\n")
        f.write(f"Opt level: {args.opt}\n")
        f.write(f"Duration: {duration:.2f}s\n")
        f.write(f"Total: {len(results)}, Passed: {passed}, Failed: {failed}\n")
        f.write(f"Status: {'PASS' if failed == 0 else 'FAIL'}\n\n")
        
        for r in results:
            status = "PASS" if r.success else "FAIL"
            f.write(f"[{status}] {r.display_name} ({r.reason})\n")
            if not r.success and r.output:
                f.write(f"--- Output ---\n{r.output}\n--------------\n")
            f.write("\n")

    print(f"\nFinished in {duration:.2f}s")
    print(f"{passed}/{len(results)} tests passed")
    if failed > 0:
        print(f"Full report written to {args.report}")
        sys.exit(1)
    
if __name__ == "__main__":
    main()
