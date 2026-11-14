from pathlib import Path
import os
import re
import subprocess
import sys
import tempfile

compiler, root, *sanitized = sys.argv[1:]
cases = Path(root) / "tests/cfg_unique_routing"
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
    ("valid_while", "21\n12\n", 0),
    ("valid_classic_continue", "13\n22\n", 0),
    ("valid_nested_loop", "11\n22\n", 0),
    ("valid_loop_return", "11\n22\n", 0),
    ("valid_break_scope", "13\n22\n", 0),
    ("valid_match_bool", "11\n22\n", 0),
    ("valid_match_expression", "21\n12\n", 0),
    ("valid_match_option", "10\n21\n7\n10\n21\n", 0),
    ("valid_match_result", "10\n22\n7\n1\n22\n", 0),
    ("valid_match_loop", "11\n22\n", 0),
    ("valid_generic", "10\n20\n", 0),
    ("valid_heap_cleanup", "a?\nb!\n", 5),
    ("valid_single_scrutinee", "7\n11\n22\n", 0),
    ("valid_forward_chain", "11\n22\n", 0),
    ("valid_field_positions", "7\n8\n20\n10\n", 0),
    ("valid_short_circuit", "7\n2\n2\n", 0),
    ("valid_protocol_cleanup", "3\n1\n20\n", 1),
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
    "invalid_backedge_swap": "E6003",
    "invalid_box": "E0611",
    "invalid_break_routes": "E6003",
    "invalid_continue_latch": "E6003",
    "invalid_duplicate": "E6003",
    "invalid_loop_local": "E6003",
    "invalid_match_local": "E6003",
    "invalid_match_routes": "E6003",
    "invalid_mixed": "E6003",
    "invalid_owner": "E0312",
    "invalid_parent": "E0312",
    "invalid_partial": "E6003",
    "invalid_protocol_cleanup": "E0410",
    "invalid_receiver": "E6003",
    "invalid_vec": "E0611",
    "invalid_zero_iteration": "E6003",
}
assert set(codes) == {p.stem for p in cases.glob("invalid_*.isa")}
for source in sorted(cases.glob("invalid_*.isa")):
    result = command([compiler, source, "--emit-mlir", "--color=never"])
    assert (
        result.returncode == 1
        and "error[" + codes[source.stem] + "]" in result.stderr
        and not result.stdout
    ), (source, result)
    if source.stem == "invalid_continue_latch":
        assert "cannot infer one exclusive input origin" in result.stderr, result.stderr

for case in ["valid_classic_continue", "valid_match_option", "valid_field_positions"]:
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
                "Unique borrowed container" in result.stdout
                and "reference.transfer" in result.stdout
            )
            assert (
                "reference.routing inferred-from-typed-hir-cfg; verify-effective-returns"
                in result.stdout
            )
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
