from pathlib import Path
import os
import re
import subprocess
import sys
import tempfile

compiler, root, *sanitized = sys.argv[1:]
cases = Path(root) / "tests/mixed_result_contracts"
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
    ("valid_identity", "1\n10\n20\n", 0),
    ("valid_selection", "4\n10\n21\n30\n40\n", 0),
    ("valid_shared_copy", "1\n1\n10\n20\n", 0),
    ("valid_shared_survives_copy", "1\n10\n20\n", 0),
    ("valid_nested", "owned\n1\n10\n20\n", 1),
    ("valid_generic", "1\n10\n20\n", 0),
    ("valid_loop_match", "3\n10\n20\n30\n", 0),
    ("valid_optional", "3\n10\n20\n30\n3\n7\n10\n30\n", 0),
    ("valid_chain", "3\n10\n20\n30\n", 0),
    ("valid_partial_drop", "10\n20\n", 0),
    ("valid_heap", "a\na?\nb!\n", 3),
    ("valid_protocol", "3\n1\n10\n20\n", 1),
    ("valid_shared_copy_inferred", "2\n2\n10\n20\n30\n", 0),
    ("valid_optional_shared", "1\n7\n10\n20\n", 0),
    ("valid_preserved_receivers", "1\n1\n10\n20\n", 0),
    ("valid_conditional_alternative", "1\n30\n1\n30\n", 0),
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
    "invalid_box_storage": "E0611",
    "invalid_vec_storage": "E0611",
    "invalid_direct_composition": "E6003",
    "invalid_copy_mixed_wrapper": "E0302",
    "invalid_enum_joint_presence": "E6003",
    "invalid_independent_presence": "E6003",
    "invalid_duplicate_unique": "E6003",
    "invalid_local_shared": "E6003",
    "invalid_local_unique": "E6003",
    "invalid_owner_drop": "E0312",
    "invalid_protocol_cleanup": "E0410",
    "invalid_receiver_replacement": "E0310",
    "invalid_receiver_result": "E6003",
    "invalid_required_presence": "E6003",
    "invalid_shared_copy_live": "E0312",
    "invalid_shared_mutation": "E0312",
    "invalid_shared_reborrow_parent": "E0312",
    "invalid_shared_routes": "E6003",
    "invalid_unique_parent": "E0312",
    "invalid_unique_routes": "E6003",
}
assert set(codes) == {p.stem for p in cases.glob("invalid_*.isa")}
for source in sorted(cases.glob("invalid_*.isa")):
    result = command([compiler, source, "--emit-mlir", "--color=never"])
    assert (
        result.returncode == 1
        and "error[" + codes[source.stem] + "]" in result.stderr
        and not result.stdout
    ), (source, result)

for case in ["valid_selection", "valid_optional", "valid_shared_copy_inferred"]:
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
        result = command([compiler, cases / (case + ".isa"), "--emit-" + stage])
        assert result.returncode == 0, (case, stage, result.stderr)
        if stage in ["hir", "core"]:
            assert (
                "Mixed borrowed container" in result.stdout
                and "reference.transfer" in result.stdout
            )
            assert "capability=Shared" in result.stdout and "capability=Unique" in result.stdout
        if stage in ["erased", "lir", "mlir", "llvm"]:
            for proof in [
                "reference.routing",
                "reference.transfer",
                "parent.forward",
                "payload.region",
                "parent obligation",
                "field RegionArgs",
                "carrier instance",
                "RegionId",
                "LoanId",
            ]:
                assert proof not in result.stdout, (case, stage, proof)
