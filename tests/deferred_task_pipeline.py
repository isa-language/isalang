from pathlib import Path
import os
import re
import resource
import subprocess
import sys
import tempfile

api, root, *sanitized = sys.argv[1:]
resource.setrlimit(resource.RLIMIT_CORE, (0, 0))
compiler = Path(api).with_name("isac")
env = dict(
    os.environ,
    ASAN_OPTIONS="detect_leaks=1:halt_on_error=1:detect_stack_use_after_return=1",
    UBSAN_OPTIONS="halt_on_error=1",
)
cases = {
    ("work", "normal"): "begin 1\ncheckpoint one\ncheckpoint two\nxABC\n",
    ("work", "repeat"): "begin 1\ncheckpoint one\ncheckpoint two\nxABC\n",
    ("work", "early"): "begin 0\nxA\n",
    (
        "work",
        "interleave",
    ): "begin 1\nbegin 2\ncheckpoint one\ncheckpoint one\ncheckpoint two\ncheckpoint two\nxABC\nyABC\n",
    ("work", "cancel-ready"): "",
    ("work", "cancel-repeat"): "",
    ("work", "cancel-one"): "begin 1\n",
    ("work", "cancel-two"): "begin 1\ncheckpoint one\n",
    ("work", "cancel-completed"): "begin 1\ncheckpoint one\ncheckpoint two\n",
    ("work", "drop-completed"): "begin 1\ncheckpoint one\ncheckpoint two\n",
    ("empty", "normal"): "empty body\n7\n",
    ("empty", "cancel-ready"): "",
    ("unit", "normal"): "unit body\n",
    ("unit", "cancel-ready"): "",
}


def run(args, **kw):
    return subprocess.run(list(map(str, args)), capture_output=True, text=True, timeout=90, **kw)


with tempfile.TemporaryDirectory() as directory:
    binary = Path(directory) / "program"
    for opt in ["O0", "O2"]:
        for (name, mode), expected in cases.items():
            built = run(
                [
                    api,
                    Path(root) / "tests",
                    name,
                    mode,
                    binary,
                    opt,
                    "sanitized" if sanitized else "normal",
                ]
            )
            assert built.returncode == 0, (name, mode, built.stderr)
            done = run([binary], env=env)
            assert done.returncode == 0 and done.stdout == expected, (name, mode, done)
            balance = re.fullmatch(r"ISAC_RT allocations=(\d+) frees=(\d+)\n", done.stderr)
            if (name, mode) in [("empty", "cancel-ready"), ("unit", "cancel-ready")]:
                assert not done.stderr, done
            else:
                assert balance and balance[1] == balance[2], (name, mode, done)
        built = run(
            [
                api,
                Path(root) / "tests",
                "work",
                "take-ready",
                binary,
                opt,
                "sanitized" if sanitized else "normal",
            ]
        )
        assert built.returncode == 0, built
        done = run([binary], env=env)
        assert (
            done.returncode != 0
            and not done.stdout
            and "frame result is not completed" in done.stderr
        ), done
        assert "AddressSanitizer" not in done.stderr and "runtime error:" not in done.stderr, done
        flags = ["--defer-call=work", "--defer-before=checkpoint_one,checkpoint_two"]
        built = run(
            [
                compiler,
                Path(root) / "tests/deferred_task.isa",
                *flags,
                "-" + opt,
                *(["--sanitize"] if sanitized else []),
                "--runtime-test-counters",
                "-o",
                binary,
            ]
        )
        assert built.returncode == 0, built
        done = run([binary], env=env)
        assert done.returncode == 0 and done.stdout == cases["work", "normal"], done
        balance = re.fullmatch(r"ISAC_RT allocations=(\d+) frees=(\d+)\n", done.stderr)
        assert balance and balance[1] == balance[2], done
    for stage in ["core", "regions", "region-constraints", "erased", "lir", "mlir", "llvm"]:
        dump = run(
            [compiler, Path(root) / "tests/deferred_task.isa", *flags, "--emit-" + stage, "-O0"]
        )
        assert dump.returncode == 0, (stage, dump.stderr)
        if stage == "core":
            assert "__frame_state_new_" in dump.stdout and "__frame_segment_" in dump.stdout
        if stage in ["erased", "lir", "mlir", "llvm"]:
            for proof in ["RegionId", "LoanId", "MemoryState", "BorrowProvenance"]:
                assert proof not in dump.stdout, (stage, proof)
print(f"{2 * (len(cases) + 2)} deferred task native runs and 7 dumps passed")
