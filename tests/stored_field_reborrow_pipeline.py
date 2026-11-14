from pathlib import Path
import os
import re
import subprocess
import sys
import tempfile

compiler, root, *sanitized = sys.argv[1:]
cases = Path(root) / "tests/stored_field_reborrow"
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
    ("valid_unique", "44\n", 0),
    ("valid_shared", "1\n1\n42\n", 0),
    ("valid_nested", "42\n", 1),
    ("valid_heap", "hello!\nworld\n", 1),
    ("valid_restore", "42\n42\n", 0),
    ("valid_early", "1\n2\n42\n", 2),
    ("valid_chain", "42\n", 0),
    ("valid_cfg", "2\n", 3),
    ("valid_lexical", "1\n42\n", 0),
    ("valid_calls", "42\n43\n", 0),
    ("valid_return", "42\n", 0),
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
    "invalid_drop": "E0312",
    "invalid_field_drop": "E0312",
    "invalid_lexical_escape": "E0312",
    "invalid_move": "E0312",
    "invalid_parent": "E0312",
    "invalid_replace": "E0312",
    "invalid_return": "E6003",
    "invalid_scope": "E0312",
    "invalid_shared_parent": "E0311",
    "invalid_shared_unique": "E0312",
    "invalid_shared_write": "E0311",
    "invalid_siblings": "E0312",
    "invalid_take": "E0312",
    "invalid_unique_overlap": "E0312",
    "invalid_unique_shared": "E0312",
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
    result = command([compiler, cases / "valid_shared.isa", "--emit-" + stage])
    assert result.returncode == 0, (stage, result.stderr)
    if stage in ["core", "regions"]:
        assert "parent obligation" in result.stdout and "keeps input alive" in result.stdout
    if stage == "core":
        assert "stored-field-reborrow [retain field; suspend storage parent]" in result.stdout
    if stage in ["erased", "lir", "mlir", "llvm"]:
        for proof in [
            "stored-field-reborrow",
            "parent obligation",
            "parent summary",
            "field RegionArgs",
            "carrier instance",
            "RegionId",
            "LoanId",
        ]:
            assert proof not in result.stdout, (stage, proof)
