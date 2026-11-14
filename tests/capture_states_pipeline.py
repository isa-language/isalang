from pathlib import Path
import os
import re
import subprocess
import sys
import tempfile

compiler, root, *sanitized = sys.argv[1:]
root = Path(root)
cases = root / "tests/capture_states"
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
    ("valid_owned_states", "1\n3\n", 10),
    ("valid_protocol_states", "42\n", 1),
    ("valid_span_contracts", "1\n42\n10\n42\n", 2),
    ("valid_loop_edges", "", 4),
    ("valid_payload_states", "payload\n", 1),
    ("valid_stored_reference_gate", "1\n20\n", 0),
    ("valid_protocol_entries", "0\n0\n", 0),
]
assert {p.stem for p in cases.glob("*.isa")} == {name for name, _, _ in positive}
with tempfile.TemporaryDirectory() as directory:
    binary = Path(directory) / "program"
    for name, output, allocations in positive:
        report = command([compiler, cases / (name + ".isa"), "--emit-captures"])
        assert report.returncode == 0, (name, report.stderr)
        assert "queries only; execution requires checked contracts" in report.stdout
        assert "initialized " in report.stdout and "terminal" in report.stdout
        if name == "valid_protocol_states":
            assert "E0410:" in report.stdout and "cancellation-cleanup=checked" in report.stdout
        if name == "valid_span_contracts":
            assert "Span contract: borrowed slice; Shared" in report.stdout
            assert "Span contract: borrowed slice; Unique" in report.stdout
            assert "frame/whole-result/storage contracts pending" in report.stdout
            assert "borrowed resource requires a frame region contract" in report.stdout
        for optimization in ["-O0", "-O2"]:
            args = [
                compiler,
                cases / (name + ".isa"),
                optimization,
                "--runtime-test-counters",
                "-o",
                binary,
            ]
            if sanitized:
                args += ["--sanitize"]
            build = command(args)
            assert build.returncode == 0, (name, build.stderr)
            result = command([binary], env=environment)
            assert (result.returncode, result.stdout) == (0, output), (name, result)
            counts = re.fullmatch(r"ISAC_RT allocations=(\d+) frees=(\d+)\n", result.stderr)
            assert (counts and int(counts[1]) == int(counts[2]) == allocations) or (
                allocations == 0 and not result.stderr
            ), (name, result.stderr)

negative = [
    ("tests/concurrency_foundations/invalid_async.isa", "E8006"),
    ("tests/concurrency_foundations/invalid_await.isa", "E8006"),
    ("tests/concurrency_foundations/invalid_live_borrow.isa", "E0312"),
    ("tests/slice_access/invalid_slice_growth.isa", "E0312"),
    ("tests/slice_access/invalid_return_slice_local.isa", "E6003"),
]
for name, code in negative:
    result = command([compiler, root / name, "--emit-captures", "--color=never"])
    assert (
        result.returncode == 1 and "error[" + code + "]" in result.stderr and not result.stdout
    ), (name, result)
for stage in ["core", "regions", "erased", "lir", "mlir", "llvm"]:
    result = command([compiler, cases / "valid_owned_states.isa", "--emit-" + stage])
    assert result.returncode == 0, (stage, result.stderr)
    for proof in ["Capture states:", "cancellation-cleanup=", "execution-live=", "Span contract:"]:
        assert proof not in result.stdout, (stage, proof)
