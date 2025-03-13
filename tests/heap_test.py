import json
import os
from pathlib import Path
import re
import subprocess
import sys
import tempfile

compiler, case, mode = sys.argv[1:4]
case = Path(case)
expected = json.loads((case.parent / "expectations.json").read_text())[case.stem]
if mode == "check":
    r = subprocess.run([compiler, str(case), "--check"], capture_output=True, text=True)
    if expected["code"]:
        assert r.returncode == 1 and f"error[{expected['code']}]" in r.stderr, (
            case,
            r.returncode,
            r.stderr,
        )
        if case.stem == "invalid_heap_use_after_move":
            assert "resource was moved to:\n    b" in r.stderr, r.stderr
        if case.stem == "invalid_buffer_after_release":
            assert "Released" in r.stderr and "Ready" in r.stderr, r.stderr
    else:
        assert r.returncode == 0, (case, r.stderr)
else:
    with tempfile.TemporaryDirectory(prefix="isac-heap-") as directory:
        output = Path(directory) / "native program"
        args = [compiler, str(case), "-o", str(output), "--runtime-test-counters"]
        if mode == "sanitized":
            args.append("--sanitize")
        r = subprocess.run(args, capture_output=True, text=True)
        assert r.returncode == 0, (case, r.stdout, r.stderr)
        env = os.environ.copy()
        env["ASAN_OPTIONS"] = "detect_leaks=1:halt_on_error=1"
        env["UBSAN_OPTIONS"] = "halt_on_error=1:print_stacktrace=1"
        r = subprocess.run([str(output)], env=env, capture_output=True, text=True)
        assert r.returncode == 0, (case, r.returncode, r.stdout, r.stderr)
        assert r.stdout == expected["stdout"], (case, r.stdout, expected["stdout"])
        counts = re.findall(r"ISAC_RT allocations=(\d+) frees=(\d+)", r.stderr)
        want = expected["allocations"]
        assert counts == ([(str(want), str(want))] if want else []), (case, r.stderr)
        assert "ERROR:" not in r.stderr and "runtime error:" not in r.stderr, r.stderr
