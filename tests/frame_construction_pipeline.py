from pathlib import Path
import os
import re
import subprocess
import sys
import tempfile

compiler, root, *sanitized = sys.argv[1:]
root = Path(root)
environment = dict(
    os.environ,
    ASAN_OPTIONS="detect_leaks=1:halt_on_error=1:detect_stack_use_after_return=1",
    UBSAN_OPTIONS="halt_on_error=1",
)


def command(args, **options):
    return subprocess.run(
        list(map(str, args)), capture_output=True, text=True, timeout=60, **options
    )


positive = [
    ("valid_owned", "one\ntwo\n11\none\ntwo\n10\n", 4),
    ("valid_protocol", "ready\n", 2),
    ("valid_loop", "", 4),
    ("valid_copy", "40\n", 0),
]
cases = root / "tests/frame_construction"
assert {p.stem for p in cases.glob("*.isa")} == {p[0] for p in positive}
with tempfile.TemporaryDirectory() as directory:
    binary = Path(directory) / "program"
    for name, output, count in positive:
        report = command([compiler, cases / (name + ".isa"), "--emit-frames"])
        assert report.returncode == 0, (name, report.stderr)
        for marker in [
            "verification only; async/coroutines/threads are not implemented",
            "constructor:",
            "cleanup prefix=0",
            "seal",
            "finish",
            "sealed lifecycle: ready -> restore -> deliver -> finish",
            "sealed lifecycle: ready -> destroy -> finish",
        ]:
            assert marker in report.stdout, (name, marker)
        if name == "valid_owned":
            for marker in ["acquire.move", "acquire.copy", "drop.staged", "drop.slot"]:
                assert marker in report.stdout, (name, marker)
        for opt in ["-O0", "-O2"]:
            args = [compiler, cases / (name + ".isa"), opt, "--runtime-test-counters", "-o", binary]
            if sanitized:
                args += ["--sanitize"]
            built = command(args)
            assert built.returncode == 0, (name, built.stderr)
            result = command([binary], env=environment)
            assert (result.returncode, result.stdout) == (0, output), (name, result)
            counts = re.fullmatch(r"ISAC_RT allocations=(\d+) frees=(\d+)\n", result.stderr)
            assert (counts and int(counts[1]) == int(counts[2]) == count) or (
                count == 0 and not result.stderr
            ), (name, result.stderr)

for name in [
    "valid_owned_states",
    "valid_payload_states",
    "valid_span_contracts",
    "valid_stored_reference_gate",
]:
    report = command([compiler, root / "tests/capture_states" / (name + ".isa"), "--emit-frames"])
    assert report.returncode == 0 and "blocked" in report.stdout, (name, report.stderr)
    if name == "valid_owned_states":
        assert "projection transfer contracts" in report.stdout
    if name in ["valid_span_contracts", "valid_stored_reference_gate"]:
        assert "frame region contract" in report.stdout
for name, code in [
    ("invalid_async", "E8006"),
    ("invalid_await", "E8006"),
    ("invalid_live_borrow", "E0312"),
]:
    result = command(
        [
            compiler,
            root / "tests/concurrency_foundations" / (name + ".isa"),
            "--emit-frames",
            "--color=never",
        ]
    )
    assert (
        result.returncode == 1 and "error[" + code + "]" in result.stderr and not result.stdout
    ), (name, result)
for stage in ["core", "erased", "lir", "mlir", "llvm"]:
    report = command([compiler, cases / "valid_owned.isa", "--emit-" + stage])
    assert report.returncode == 0, (stage, report.stderr)
    for marker in ["Typed frame IR:", "acquire.move", "drop.staged", "cleanup prefix="]:
        assert marker not in report.stdout, (stage, marker)
