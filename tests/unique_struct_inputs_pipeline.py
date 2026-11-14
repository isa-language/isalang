from pathlib import Path
import os
import re
import subprocess
import sys
import tempfile

compiler, root, *sanitized = sys.argv[1:]
cases = Path(root) / "tests/unique_struct_inputs"
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
    ("valid_direct", "42\n", 0),
    ("valid_nested", "owned\n42\n", 1),
    ("valid_transfer", "43\n", 0),
    ("valid_mixed", "7\nmixed\n42\n", 1),
    ("valid_fields", "10\n20\n", 0),
    ("valid_enum", "42\n", 0),
    ("valid_cfg", "10\n21\n", 2),
    ("valid_shared_child", "1\n42\n", 0),
    ("valid_unique_child", "42\n", 0),
    ("valid_impl", "42\n", 0),
    ("valid_heap", "hello!\n", 1),
    ("valid_shared_result", "7\n43\n", 0),
    ("valid_protocol", "3\n42\n", 1),
    ("valid_array", "42\n", 0),
    ("valid_vec_referent", "42\n1\n", 2),
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
    "invalid_box": "E0611",
    "invalid_vec": "E0611",
    "invalid_child_drop": "E0312",
    "invalid_consumed": "E0301",
    "invalid_missing_field": "E0301",
    "invalid_owner_argument": "E0312",
    "invalid_receiver": "E6003",
    "invalid_reference_parameter": "E6003",
    "invalid_scope": "E0312",
    "invalid_shared_child_write": "E0312",
    "invalid_shared_escape": "E6003",
    "invalid_take_live": "E0312",
    "invalid_unique_result": "E6003",
    "invalid_unique_origin_result": "E6003",
    "invalid_protocol_cleanup": "E0410",
    "invalid_protocol_input": "E0403",
    "invalid_vec_referent_invalidation": "E0312",
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
    result = command([compiler, cases / "valid_mixed.isa", "--emit-" + stage])
    assert result.returncode == 0, (stage, result.stderr)
    if stage in ["hir", "core"]:
        assert (
            "Unique caller-owned noescape" in result.stdout
            and "Shared caller-owned noescape" in result.stdout
        )
    if stage in ["core", "regions"]:
        assert "parent obligation" in result.stdout and "keeps input alive" in result.stdout
    if stage == "core":
        assert "stored-field-reborrow [retain field; suspend storage parent]" in result.stdout
    if stage in ["erased", "lir", "mlir", "llvm"]:
        for proof in [
            "payload.region",
            "stored-field-reborrow",
            "parent obligation",
            "parent summary",
            "field RegionArgs",
            "carrier instance",
            "RegionId",
            "LoanId",
        ]:
            assert proof not in result.stdout, (stage, proof)
