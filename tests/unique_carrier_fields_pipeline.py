from pathlib import Path
import os
import re
import subprocess
import sys
import tempfile

compiler, root, *sanitized = sys.argv[1:]
cases = Path(root) / "tests/unique_carrier_fields"
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
    ("valid_store", "42\n2\n", 1),
    ("valid_replace", "3\n4\n", 0),
    ("valid_nested", "42\n", 1),
    ("valid_mixed", "7\n8\n42\n", 1),
    ("valid_cleanup", "1\n2\n42\n", 2),
    ("valid_loop", "2\n2\n", 0),
    ("valid_heap", "2\n0\n3\n", 2),
    ("valid_temporary", "42\n", 1),
    ("valid_siblings", "3\n4\n", 0),
    ("valid_restore", "42\n", 0),
    ("valid_branch", "42\n", 0),
    ("valid_pruned_loop", "42\n", 0),
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
    "invalid_parent_read": "E0312",
    "invalid_parent_write": "E0312",
    "invalid_second_loan": "E0312",
    "invalid_owner_drop": "E0312",
    "invalid_alias_after_take": "E0312",
    "invalid_alias_after_match": "E0312",
    "invalid_double_take": "E0301",
    "invalid_implicit_copy": "E0302",
    "invalid_missing_restore": "E0310",
    "invalid_parameter": "E6003",
    "invalid_result": "E6003",
    "invalid_direct_reference": "E0312",
    "invalid_construct_unique": "E0312",
    "invalid_vec": "E0611",
    "invalid_box": "E0611",
    "invalid_reuse_replacement": "E0312",
    "invalid_lexical_parent_end": "E0312",
    "invalid_local_owner_end": "E0312",
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
    result = command([compiler, cases / "valid_replace.isa", "--emit-" + stage])
    assert result.returncode == 0, (stage, result.stderr)
    if stage in ["core", "regions"]:
        assert "parent obligation" in result.stdout and "keeps input alive" in result.stdout
    if stage in ["erased", "lir", "mlir", "llvm"]:
        for proof in [
            "parent obligation",
            "parent summary",
            "field RegionArgs",
            "carrier instance",
            "RegionId",
            "LoanId",
        ]:
            assert proof not in result.stdout, (stage, proof)
