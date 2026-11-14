from pathlib import Path
import os
import re
import subprocess
import sys
import tempfile

compiler, root, *sanitized = sys.argv[1:]
cases = Path(root) / "tests/owned_unique_transfer"
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
    ("valid_forward", "43\n", 0),
    ("valid_projection", "owned\n42\n", 1),
    ("valid_mixed", "7\nmixed\n42\n", 1),
    ("valid_generic", "42\n", 0),
    ("valid_chain", "42\n", 0),
    ("valid_cfg", "21\n", 0),
    ("valid_optional", "42\n", 0),
    ("valid_absent", "7\n1\n", 2),
    ("valid_reborrow", "42\n", 0),
    ("valid_heap", "hello!\n", 1),
    ("valid_other_unique", "1\n42\n", 0),
    ("valid_protocol", "2\n42\n", 1),
    ("valid_discard_optional", "1\n", 0),
    ("valid_scope", "43\n", 0),
    ("valid_storage_parent", "42\n", 0),
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
    "invalid_owner_drop": "E0312",
    "invalid_parent_active": "E0312",
    "invalid_storage_parent": "E0312",
    "invalid_input_reuse": "E0301",
    "invalid_local": "E6003",
    "invalid_ambiguous": "E6003",
    "invalid_multiple": "E6003",
    "invalid_owned_storage": "E0204",
    "invalid_owned_storage_heap": "E6003",
    "invalid_optional_required": "E6003",
    "invalid_receiver": "E6003",
    "invalid_vec": "E0611",
    "invalid_box": "E0611",
    "invalid_protocol_cleanup": "E0410",
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
        assert "parent obligation" in result.stdout and "parent.forward" in result.stdout
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
