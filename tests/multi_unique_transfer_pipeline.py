from pathlib import Path
import os
import re
import subprocess
import sys
import tempfile

compiler, root, *sanitized = sys.argv[1:]
cases = Path(root) / "tests/multi_unique_transfer"
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
    ("valid_forward", "43\n0\n", 0),
    ("valid_projection", "owned\n42\n1\n", 1),
    ("valid_separate_inputs", "42\n1\n", 0),
    ("valid_chain", "42\n1\n", 0),
    ("valid_generic", "42\n1\n", 0),
    ("valid_partial_drop", "42\n0\n", 0),
    ("valid_cfg", "21\n0\n", 0),
    ("valid_reborrow", "42\n1\n", 0),
    ("valid_optional", "7\n42\n1\n", 2),
    ("valid_mixed_input", "7\nmixed\n7\n42\n0\n", 1),
    ("valid_protocol", "2\n42\n", 1),
    ("valid_three", "42\n1\nhello!\n", 1),
    ("valid_result", "43\n0\n43\n0\n", 0),
    ("valid_protocol_output", "3\n42\n0\n", 1),
]
assert {p.stem for p in cases.glob("valid_*.isa")} == {name for name, _, _ in positive}
with tempfile.TemporaryDirectory() as directory:
    binary = Path(directory) / "program"
    for case, expected, allocations in positive:
        for opt in ["-O0", "-O2"]:
            args = [compiler, cases / (case + ".isa"), opt, "--runtime-test-counters", "-o", binary]
            if sanitized:
                args += ["--sanitize"]
            result = command(args)
            assert result.returncode == 0, (case, result.stderr)
            result = command([binary], env=environment)
            assert (result.returncode, result.stdout) == (0, expected), (case, result)
            counts = re.fullmatch(r"ISAC_RT allocations=(\d+) frees=(\d+)\n", result.stderr)
            assert (counts and int(counts[1]) == int(counts[2]) == allocations) or (
                allocations == 0 and not result.stderr
            ), (case, result.stderr)

codes = {
    "invalid_aggregate_reborrow": "E0312",
    "invalid_ambiguous": "E6003",
    "invalid_box": "E0611",
    "invalid_direct_multi": "E6003",
    "invalid_duplicate": "E6003",
    "invalid_flag_parent": "E0312",
    "invalid_hybrid": "E6003",
    "invalid_local": "E6003",
    "invalid_missing_source": "E6003",
    "invalid_mixed_output": "E6003",
    "invalid_number_parent": "E0312",
    "invalid_owned_storage": "E6003",
    "invalid_partial_drop": "E0312",
    "invalid_protocol_cleanup": "E0410",
    "invalid_receiver": "E6003",
    "invalid_vec": "E0611",
}
assert set(codes) == {p.stem for p in cases.glob("invalid_*.isa")}
for source in sorted(cases.glob("invalid_*.isa")):
    result = command([compiler, source, "--emit-mlir", "--color=never"])
    assert (
        result.returncode == 1
        and "error[" + codes[source.stem] + "]" in result.stderr
        and not result.stdout
    ), (source, result)

for stage in [
    "ast",
    "hir",
    "core",
    "regions",
    "region-constraints",
    "erased",
    "lir",
    "mlir",
    "llvm",
]:
    result = command([compiler, cases / "valid_forward.isa", "--emit-" + stage])
    assert result.returncode == 0, (stage, result.stderr)
    if stage in ["hir", "core"]:
        assert (
            "Unique borrowed container" in result.stdout and "reference.transfer" in result.stdout
        )
    if stage in ["core", "regions"]:
        assert "parent obligation" in result.stdout and result.stdout.count("parent.forward") == 2
    if stage in ["erased", "lir", "mlir", "llvm"]:
        for proof in [
            "reference.transfer",
            "parent.forward",
            "payload.region",
            "parent obligation",
            "field RegionArgs",
            "carrier instance",
            "RegionId",
            "LoanId",
        ]:
            assert proof not in result.stdout, (stage, proof)
