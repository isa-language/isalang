from pathlib import Path
import os
import re
import subprocess
import sys
import tempfile

compiler, root, *sanitized = sys.argv[1:]
cases = Path(root) / "tests/direct_reference_fields"
environment = dict(
    os.environ,
    ASAN_OPTIONS="detect_leaks=1:halt_on_error=1:detect_stack_use_after_return=1",
    UBSAN_OPTIONS="halt_on_error=1",
)


def command(args, **options):
    return subprocess.run(
        list(map(str, args)), capture_output=True, text=True, timeout=60, **options
    )


with tempfile.TemporaryDirectory() as directory:
    binary = Path(directory) / "program"
    for case, expected in [
        ("valid_direct", "42\n7\nshared\n6\n42\n43\n43\n44\n45\n"),
        ("valid_replace", "1\n2\n4\n3\n5\n"),
        ("valid_return", "9\n9\n10\n"),
        ("valid_cfg", "10\n10\n11\n20\n21\n22\n"),
        ("valid_projections", "7\n3\n3\n8\n7\n9\n10\n"),
    ]:
        for opt in ["-O0", "-O2"]:
            args = [compiler, cases / (case + ".isa"), opt, "--runtime-test-counters", "-o", binary]
            if sanitized:
                args += ["--sanitize"]
            result = command(args)
            assert result.returncode == 0, (case, result.stderr)
            result = command([binary], env=environment)
            assert (result.returncode, result.stdout) == (0, expected), (case, result)
            counts = re.fullmatch(r"ISAC_RT allocations=(\d+) frees=(\d+)\n", result.stderr)
            assert (counts and counts[1] == counts[2]) or not result.stderr, (case, result.stderr)

codes = {
    "invalid_alias_after_field_drop": "E0312",
    "invalid_alias_after_replace": "E0312",
    "invalid_alias_after_struct_drop": "E0312",
    "invalid_consumed_field": "E0301",
    "invalid_local_scope": "E0312",
    "invalid_mutate_referent": "E0311",
    "invalid_nested_box": "E0611",
    "invalid_nested_vec": "E0611",
    "invalid_owner_drop": "E0312",
    "invalid_owner_mutation": "E0312",
    "invalid_return_hidden_origin": "E6003",
    "invalid_return_local": "E6003",
    "invalid_shared_store": "E0311",
    "invalid_shared_take": "E0311",
    "invalid_struct_parameter": "E6003",
    "invalid_struct_result": "E6003",
    "invalid_uninitialized": "E0301",
    "invalid_unique_argument": "E0201",
    "invalid_unique_field": "E0312",
    "invalid_unique_generic": "E0312",
    "invalid_unique_missing_restore": "E0310",
}
assert set(codes) == {source.stem for source in cases.glob("invalid_*.isa")}
for source in sorted(cases.glob("invalid_*.isa")):
    result = command([compiler, source, "--emit-mlir", "--color=never"])
    assert (
        result.returncode == 1
        and "error[" + codes[source.stem] + "]" in result.stderr
        and not result.stdout
    ), (source, result)
    if source.stem in ["invalid_nested_vec", "invalid_nested_box"]:
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
    result = command([compiler, cases / "valid_direct.isa", "--emit-" + stage])
    assert result.returncode == 0, (stage, result.stderr)
    for marker in ["field RegionArgs", "carrier instance"]:
        assert (marker in result.stdout) == (stage in ["core", "regions"]), (stage, marker)
    if stage == "core":
        assert "reference.field=#" in result.stdout and "stored-reference" in result.stdout
    if stage in ["erased", "lir", "mlir", "llvm"]:
        for proof in ["carrier.path", "reference.field=#", "RegionId", "LoanId"]:
            assert proof not in result.stdout, (stage, proof)
