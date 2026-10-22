from pathlib import Path
import os
import re
import subprocess
import sys
import tempfile

compiler, root, *sanitized = sys.argv[1:]
cases = Path(root) / "tests/mixed_region_contracts"
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
    ("valid_transfer", "42\n5\n43\n", 1),
    ("valid_nested", "7\n8\n", 3),
    ("valid_optional", "0\n1\n2\n", 2),
    ("valid_shared_method", "3\n2\n2\n4\n", 4),
    ("valid_cfg", "10\n11\n11\n11\n12\n", 3),
    ("valid_protocol", "3\n42\n43\n", 1),
    ("valid_owned_containers", "42\n2\n43\n", 8),
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
            assert counts and int(counts[1]) == int(counts[2]) == allocations, (case, result.stderr)

codes = {
    "invalid_alias_then_consume": "E0312",
    "invalid_box": "E0611",
    "invalid_call_drop_origin": "E0312",
    "invalid_local_result": "E6003",
    "invalid_owned_origin": "E6003",
    "invalid_partial_input": "E0301",
    "invalid_partial_result": "E0301",
    "invalid_protocol_cleanup": "E0410",
    "invalid_protocol_input": "E0403",
    "invalid_protocol_result": "E0403",
    "invalid_result_drop": "E0312",
    "invalid_return_alias_into_owner": "E6003",
    "invalid_shared_owned_origin": "E6003",
    "invalid_shared_replacement": "E0311",
    "invalid_use_after_call": "E0301",
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
    if source.stem in ["invalid_box", "invalid_vec"]:
        assert "reference-free owned elements" in result.stderr, (source, result.stderr)

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
    result = command([compiler, cases / "valid_nested.isa", "--emit-" + stage])
    assert result.returncode == 0, (stage, result.stderr)
    if stage in ["hir", "core"]:
        assert "reference.transfer" in result.stdout and "reference.input" in result.stdout
    if stage in ["core", "regions"]:
        assert "field RegionArgs" in result.stdout and "carrier instance" in result.stdout
    if stage in ["erased", "lir", "mlir", "llvm"]:
        for proof in [
            "reference.transfer",
            "reference.input",
            "field RegionArgs",
            "carrier instance",
            "RegionId",
            "LoanId",
        ]:
            assert proof not in result.stdout, (stage, proof)
