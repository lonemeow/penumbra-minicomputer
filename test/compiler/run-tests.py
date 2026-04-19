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
    def __init__(self, name, success, reason, output=""):
        self.name = name
        self.success = success
        self.reason = reason
        self.output = output

def run_single_test(args):
    test_path, opt, harness_dir, build_dir, iss_path, cc, objcopy, bin2hex, builtins, resource_dir = args
    name = os.path.splitext(os.path.basename(test_path))[0]
    obj = os.path.join(build_dir, f"{name}.elf")
    hex_file = os.path.join(build_dir, f"{name}.hex")
    bin_file = os.path.join(build_dir, f"{name}.bin")
    
    # 1. Compile
    cmd_compile = shlex.split(cc) + [
        opt, "-ffreestanding", "-nostdlib", "-nostdinc",
        "-I", harness_dir,
    ]
    if resource_dir:
        cmd_compile += ["-isystem", os.path.join(resource_dir, "include")]
    
    cmd_compile += [
        "-T", os.path.join(harness_dir, "test.ld"),
        os.path.join(harness_dir, "crt0.S"),
        os.path.join(harness_dir, "libc_stub.c"),
        test_path
    ]
    if builtins:
        cmd_compile.append(builtins)
    
    cmd_compile += ["-o", obj]
    try:
        subprocess.run(cmd_compile, check=True, capture_output=True, text=True)
    except subprocess.CalledProcessError as e:
        return TestResult(name, False, "compile error", e.stderr)

    # 2. Objcopy + Bin2Hex
    try:
        subprocess.run(shlex.split(objcopy) + ["-O", "binary", obj, bin_file], check=True, capture_output=True)
        subprocess.run([sys.executable, bin2hex, bin_file, "-o", hex_file], check=True, capture_output=True)
    except subprocess.CalledProcessError as e:
        return TestResult(name, False, "objcopy/bin2hex error", e.stderr)

    # 3. Run in ISS
    # We use a 1M instruction limit and +quiet to get only program output
    cmd_iss = [iss_path, hex_file, "+hosted", "+quiet", "+max-insn=1000000"]
    try:
        proc = subprocess.run(cmd_iss, capture_output=True, text=True, timeout=30)
        actual_output = proc.stdout
        exit_code = proc.returncode
    except subprocess.TimeoutExpired:
        return TestResult(name, False, "timeout (30s)")
    except Exception as e:
        return TestResult(name, False, "simulator error", str(e))

    # 4. Verify output if reference exists
    ref_path = os.path.splitext(test_path)[0] + ".reference_output"
    if os.path.exists(ref_path):
        with open(ref_path, "r") as f:
            expected_output = f.read()
        
        # llvm-test-suite reference outputs often end with "exit 0"
        expected_lines = [l.strip() for l in expected_output.strip().splitlines() 
                         if l.strip() and not l.strip().startswith("exit ")]
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
            return TestResult(name, False, "output mismatch", mismatch_info)

    if exit_code != 0:
        return TestResult(name, False, f"exit code {exit_code}", actual_output)

    return TestResult(name, True, "pass", actual_output)

def main():
    parser = argparse.ArgumentParser(description="Penumbra Compiler Test Runner")
    parser.add_argument("--test-dir", action="append", required=True, help="Directory containing tests")
    parser.add_argument("--opt", default="-O2", help="Optimization level")
    parser.add_argument("--harness-dir", required=True, help="Harness directory")
    parser.add_argument("--build-dir", required=True, help="Build directory")
    parser.add_argument("--iss", required=True, help="Path to penumbra-iss")
    parser.add_argument("--cc", required=True, help="Path to clang")
    parser.add_argument("--objcopy", required=True, help="Path to llvm-objcopy")
    parser.add_argument("--bin2hex", required=True, help="Path to bin2hex.py")
    parser.add_argument("--builtins", help="Path to libclang_rt.builtins-penumbra.a")
    parser.add_argument("--resource-dir", help="Clang resource directory")
    parser.add_argument("--exclude", action="append", help="Glob pattern to exclude tests")
    parser.add_argument("--report", default="test-report.txt", help="Report file path")
    parser.add_argument("-j", "--jobs", type=int, default=multiprocessing.cpu_count(), help="Number of parallel jobs")
    
    args = parser.parse_args()

    os.makedirs(args.build_dir, exist_ok=True)

    test_files = []
    for d in args.test_dir:
        for root, _, files in os.walk(d):
            for f in files:
                if f.endswith(".c"):
                    full_path = os.path.join(root, f)
                    
                    # Check exclusions
                    excluded = False
                    if args.exclude:
                        for pattern in args.exclude:
                            if fnmatch.fnmatch(full_path, pattern) or fnmatch.fnmatch(os.path.basename(full_path), pattern):
                                excluded = True
                                break
                    
                    if not excluded:
                        test_files.append(full_path)
    
    test_files.sort()
    print(f"Found {len(test_files)} tests in {args.test_dir}")
    print(f"Running with {args.jobs} parallel jobs at {args.opt}...")

    worker_args = [
        (f, args.opt, args.harness_dir, args.build_dir, args.iss, args.cc, args.objcopy, args.bin2hex, args.builtins, args.resource_dir)
        for f in test_files
    ]

    results = []
    passed = 0
    failed = 0

    start_time = time.time()
    
    # ProcessPoolExecutor for parallelism
    with ProcessPoolExecutor(max_workers=args.jobs) as executor:
        # map ensures we get results in order, but we want to print as they finish
        # for better UX. use as_completed or just use map and accept wait?
        # Actually map is fine if we want deterministic output order, 
        # but let's use imap or similar if we want to print progress.
        
        for result in executor.map(run_single_test, worker_args):
            if result.success:
                print(f"  {GREEN}PASS{RESET}  {result.name}")
                passed += 1
            else:
                print(f"  {RED}FAIL{RESET}  {result.name} ({result.reason})")
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
        f.write(f"Total: {len(results)}, Passed: {passed}, Failed: {failed}\n\n")
        
        for r in results:
            status = "PASS" if r.success else "FAIL"
            f.write(f"[{status}] {r.name} ({r.reason})\n")
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
