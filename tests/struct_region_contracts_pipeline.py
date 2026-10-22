from pathlib import Path
import os
import re
import subprocess
import sys
import tempfile

compiler, root, *sanitized = sys.argv[1:]
cases = Path(root) / "tests/struct_region_contracts"
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
    ("valid_direct", "42\n42\n43\n44\n", 0),
    ("valid_paths", "7\n8\n", 1),
    ("valid_optional", "0\n1\n2\n", 0),
    ("valid_methods", "3\n2\n2\n4\n", 0),
    ("valid_cfg", "10\n11\n12\n", 0),
    ("valid_multiple_origins", "10\n21\n12\n22\n", 0),
]
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
    "invalid_box": "E0611",
    "invalid_call_overlap": "E0312",
    "invalid_conditional_required": "E6003",
    "invalid_input_move": "E0301",
    "invalid_input_mutation": "E0311",
    "invalid_mixed_input": "E6003",
    "invalid_mixed_result": "E6003",
    "invalid_multiple_origins": "E0312",
    "invalid_owner_drop": "E0312",
    "invalid_owner_mutation": "E0312",
    "invalid_partial_input": "E0301",
    "invalid_partial_result": "E0301",
    "invalid_reference_parameter": "E6003",
    "invalid_return_call_local": "E6003",
    "invalid_return_local": "E6003",
    "invalid_return_parameter_storage": "E6003",
    "invalid_shared_receiver_mutation": "E0311",
    "invalid_shared_receiver_take": "E0311",
    "invalid_unique_receiver": "E6003",
    "invalid_vec": "E0611",
}
assert set(codes) == {source.stem for source in cases.glob("invalid_*.isa")}
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
    result = command([compiler, cases / "valid_paths.isa", "--emit-" + stage])
    assert result.returncode == 0, (stage, result.stderr)
    if stage in ["hir", "core"]:
        assert "reference.transfer" in result.stdout and "reference.input" in result.stdout
        assert "return.payload-source" in result.stdout and "field=#0" in result.stdout
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
