#!/usr/bin/env python3
"""Compile and run the real candidate C driver on the host, never on a board."""
import argparse
import hashlib
import json
import os
from pathlib import Path
import resource
import shutil
import subprocess
import tempfile

HERE = Path(__file__).resolve().parent
ROOT = HERE.parent


def run(argv, timeout=15):
    def no_core():
        resource.setrlimit(resource.RLIMIT_CORE, (0, 0))
    try:
        result = subprocess.run(argv, capture_output=True, text=True,
                                timeout=timeout, preexec_fn=no_core)
        return {"returncode": result.returncode, "stdout": result.stdout,
                "stderr": result.stderr, "timeout": False}
    except subprocess.TimeoutExpired as exc:
        return {"returncode": None, "stdout": (exc.stdout or b"").decode(),
                "stderr": (exc.stderr or b"").decode(), "timeout": True}


def compile_and_run(root, output, extra):
    cmd = ["clang", "-std=c11", "-g", "-O1", "-Wall", "-Wextra", "-Werror",
           "-Wno-unused-parameter", "-fno-omit-frame-pointer", *extra,
           "-I" + str(root / "tests/include"), "-I" + str(root / "tests"),
           str(root / "tests/test_pdma_host.c"), "-o", str(output)]
    compiled = run(cmd)
    if compiled["returncode"] != 0:
        return {"compiled": compiled, "run": None}
    return {"compiled": compiled, "run": run([str(output)])}


def main():
    global ROOT
    parser = argparse.ArgumentParser()
    parser.add_argument("--driver-dir", type=Path, default=HERE.parents[1],
                        help="PDMA driver directory (default: ../..)")
    parser.add_argument("--report", type=Path,
                        help="Optional JSON report; an existing file is never overwritten")
    parser.add_argument("--asan", action="store_true")
    parser.add_argument("--mutations", action="store_true")
    args = parser.parse_args()
    if args.report is not None and args.report.exists():
        raise FileExistsError(args.report)
    report = {
        "driver_sha256": hashlib.sha256((args.driver_dir / "drv_pdma.c").read_bytes()).hexdigest(),
        "header_sha256": hashlib.sha256((args.driver_dir / "drv_pdma.h").read_bytes()).hexdigest(),
        "actual_candidate_c": True,
        "host_shims": True,
        "target_build": False,
        "hardware_validated": False,
        "variants": {}, "mutations": {},
    }
    failed = False
    ubsan = ["-fsanitize=undefined", "-fno-sanitize-recover=all"]
    with tempfile.TemporaryDirectory(prefix="pdma-c-host-") as temp:
        directory = Path(temp)
        ROOT = directory / "source"
        shutil.copytree(HERE, ROOT / "tests")
        (ROOT / "after").mkdir()
        for name in ("drv_pdma.c", "drv_pdma.h"):
            shutil.copy2(args.driver_dir / name, ROOT / "after" / name)
        for name, flags in [("plain", []), ("ubsan", ubsan),
                            ("ubsan_smp", [*ubsan, "-DRT_USING_SMP=1"]),
                            ("ubsan_builtin_tests_compiled", [*ubsan,
                                "-DRT_USING_MSH=1", "-DRT_PDMA_ENABLE_BUILTIN_TESTS=1"])]:
            result = compile_and_run(ROOT, directory / name, flags)
            report["variants"][name] = result
            ok = result["run"] is not None and result["run"]["returncode"] == 0
            failed |= not ok
            print(name, "PASS" if ok else "FAIL")
        if args.asan:
            result = compile_and_run(ROOT, directory / "asan", ["-fsanitize=address,undefined"])
            report["variants"]["asan"] = result
            ok = result["run"] is not None and result["run"]["returncode"] == 0
            failed |= not ok
            print("asan", "PASS" if ok else "FAIL")
        if args.mutations:
            source = (ROOT / "after/drv_pdma.c").read_text()
            mutations = {
                "old_unconditional_queue": [
                    ("pdma_completion_queued(chan) &&\n        pdma_dev.chan[chan].completion_count >=",
                     "pdma_dev.chan[chan].completion_count >="),
                    ("pdma_completion_queued(ch) && !pdma_completion_push(ch,",
                     "!pdma_completion_push(ch,")],
                "infer_from_active_waiter": [
                    ("pdma_completion_queued(ch) && !pdma_completion_push(ch,",
                     "pdma_dev.chan[ch].waiter_active && !pdma_completion_push(ch,")],
                "old_waiter_stops_new_generation": [
                    ("if (pdma_chan_active(chan) && quiesce &&\n            pdma_dev.chan[chan].generation == generation)",
                     "if (pdma_chan_active(chan) && quiesce)")],
                "registration_discards_queue_ownership": [
                    ("pdma_dev.chan[chan].callback_count != 0 ||\n        pdma_dev.chan[chan].completion_count != 0)",
                     "pdma_dev.chan[chan].callback_count != 0)")],
                "old_descriptor_self_link": [
                    ("(list + i + 1)", "(list + 1)")],
                "signed_channel7_masks": [
                    ("#define PTOUT_INT 0x01000000U", "#define PTOUT_INT 0x01000000"),
                    ("#define PALL_INT 0x01010101U", "#define PALL_INT 0x01010101")],
                "callback_mode_leaks_waiter_events": [
                    ("notify_waiters = pdma_completion_queued(ch);", "notify_waiters = RT_TRUE;")],
            }
            for name, changes in mutations.items():
                mutated = source
                for before, after in changes:
                    assert mutated.count(before) == 1, (name, before)
                    mutated = mutated.replace(before, after)
                root = directory / name
                shutil.copytree(ROOT / "after", root / "after")
                shutil.copytree(ROOT / "tests/include", root / "tests/include")
                for filename in ["host_shim.h", "test_pdma_host.c"]:
                    shutil.copy2(ROOT / "tests" / filename, root / "tests" / filename)
                (root / "after/drv_pdma.c").write_text(mutated)
                result = compile_and_run(root, directory / (name + ".bin"), ubsan)
                # A compiler failure is not a detected behavioral regression.
                killed = (result["compiled"]["returncode"] == 0 and
                          result["run"]["returncode"] not in (None, 0))
                result["mutation_detected"] = killed
                report["mutations"][name] = result
                failed |= not killed
                print(name, "DETECTED" if killed else "NOT_DETECTED")
    report["requested_checks_passed"] = not failed
    report["limitations"] = ["SMP variant uses sequential shims, not concurrent SMP proof.", "ASan is optional and must not be inferred from UBSan results."]
    if args.report is not None:
        with args.report.open("x") as output:
            output.write(json.dumps(report, indent=2) + "\n")
    return int(failed)


if __name__ == "__main__":
    raise SystemExit(main())
