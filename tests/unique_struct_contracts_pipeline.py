from pathlib import Path
import os
import re
import subprocess
import sys
import tempfile

compiler, root, *sanitized = sys.argv[1:]
cases = Path(root) / "tests/unique_struct_contracts"
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
    ("valid_result", "42\n", 0),
    ("valid_nested_result", "42\n", 1),
    ("valid_enum_result", "42\n", 0),
    ("valid_receivers", "1\n42\n42\n", 0),
    ("valid_receiver_mixed", "7\n10\n6\n10\n20\n", 1),
    ("valid_method_result", "42\n", 0),
    ("valid_protocol_result", "2\n42\n", 1),
    ("valid_cfg", "21\n", 0),
    ("valid_parent", "43\n", 0),
    ("valid_receiver_alias", "42\n42\n", 0),
    ("valid_receiver_child", "42\n", 0),
    ("valid_nested_receiver", "42\n42\n", 1),
    ("valid_heap_result", "hello!\nhello!\n", 1),
    ("valid_shared_fields_receiver", "42\n43\n", 1),
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
    "invalid_local_result": "E6003",
    "invalid_multiple_results": "E6003",
    "invalid_receiver_change": "E0310",
    "invalid_receiver_extraction": "E0310",
    "invalid_receiver_shared_mutation": "E0311",
    "invalid_receiver_branch": "E0310",
    "invalid_receiver_local": "E0312",
    "invalid_receiver_return": "E6003",
    "invalid_two_origins": "E6003",
    "invalid_temporary_result": "E0204",
    "invalid_parent_active": "E0312",
    "invalid_owner_drop": "E0312",
    "invalid_vec": "E0611",
    "invalid_box": "E0611",
    "invalid_receiver_child_escape": "E0312",
    "invalid_owned_input_result": "E6003",
    "invalid_mixed_result": "E6003",
    "invalid_protocol_cleanup": "E0410",
    "invalid_shared_field_replacement": "E0310",
    "invalid_receiver_region_change": "E0310",
}
assert set(codes) == {p.stem for p in cases.glob("invalid_*.isa")}
for source in sorted(cases.glob("invalid_*.isa")):
    result = command([compiler, source, "--emit-mlir", "--color=never"])
    assert (
        result.returncode == 1
        and "error[" + codes[source.stem] + "]" in result.stderr
        and not result.stdout
    ), (source, result)
    if source.stem == "invalid_receiver_region_change":
        assert "stored reference region arguments" in result.stderr, result.stderr

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
    result = command([compiler, cases / "valid_result.isa", "--emit-" + stage])
    assert result.returncode == 0, (stage, result.stderr)
    if stage in ["hir", "core"]:
        assert (
            "Unique borrowed container" in result.stdout and "reference.transfer" in result.stdout
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

for stage in ["hir", "core"]:
    result = command([compiler, cases / "valid_receivers.isa", "--emit-" + stage])
    assert result.returncode == 0 and "reference.preserve" in result.stdout, (stage, result)
