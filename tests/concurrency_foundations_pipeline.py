from pathlib import Path
import os
import re
import subprocess
import sys
import tempfile

compiler, root, *sanitized = sys.argv[1:]
cases = Path(root) / "tests/concurrency_foundations"
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
    ("valid_structural", "30\n4\n", 0),
    ("valid_borrowed_state", "7\n1\n2\n21\n7\n1\n2\n21\n", 0),
    ("valid_loop_cleanup", "1\n1\n20\n1\n1\n2\n", 0),
    ("valid_runtime_contracts", "1\n3\n", 5),
    ("valid_reference_results", "1\n20\n", 0),
]
assert {p.stem for p in cases.glob("valid_*.isa")} == {name for name, _, _ in positive}
with tempfile.TemporaryDirectory() as directory:
    binary = Path(directory) / "program"
    for case, output, allocations in positive:
        report = command([compiler, cases / (case + ".isa"), "--emit-concurrency"])
        assert report.returncode == 0, (case, report.stderr)
        assert "queries only; execution requires checked contracts" in report.stdout
        assert "semantic CFG" in report.stdout and "loan-demands=" in report.stdout
        assert "Send: structural" in report.stdout and "Sync: structural" in report.stdout
        assert (
            "allocator thread contract" in report.stdout
            and "region bound required" in report.stdout
        )
        if case == "valid_borrowed_state":
            assert "sparse; not empty" in report.stdout
        for optimization in ["-O0", "-O2"]:
            args = [
                compiler,
                cases / (case + ".isa"),
                optimization,
                "--runtime-test-counters",
                "-o",
                binary,
            ]
            if sanitized:
                args += ["--sanitize"]
            build = command(args)
            assert build.returncode == 0, (case, build.stderr)
            result = command([binary], env=environment)
            assert (result.returncode, result.stdout) == (0, output), (case, result)
            counts = re.fullmatch(r"ISAC_RT allocations=(\d+) frees=(\d+)\n", result.stderr)
            assert (counts and int(counts[1]) == int(counts[2]) == allocations) or (
                allocations == 0 and not result.stderr
            ), (case, result.stderr)
codes = {
    "invalid_async": "E8006",
    "invalid_await": "E8006",
    "invalid_postfix_await": "E8006",
    "invalid_live_borrow": "E0312",
    "invalid_owner_escape": "E6003",
}
assert set(codes) == {p.stem for p in cases.glob("invalid_*.isa")}
for name, code in codes.items():
    result = command([compiler, cases / (name + ".isa"), "--emit-concurrency", "--color=never"])
    assert (
        result.returncode == 1 and "error[" + code + "]" in result.stderr and not result.stdout
    ), (name, result)
for stage in ["core", "regions", "erased", "lir", "mlir", "llvm"]:
    result = command([compiler, cases / "valid_reference_results.isa", "--emit-" + stage])
    assert result.returncode == 0, (stage, result.stderr)
    assert "Concurrency foundations" not in result.stdout
    if stage in ["core", "regions"]:
        assert "region" in result.stdout.lower()
    else:
        for proof in [
            "loan-demands=",
            "Send: structural",
            "Sync: structural",
            "reference.transfer",
            "parent.forward",
            "RegionId",
            "LoanId",
        ]:
            assert proof not in result.stdout, (stage, proof)
