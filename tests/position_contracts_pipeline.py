from pathlib import Path
import os
import re
import subprocess
import sys
import tempfile

compiler, root, *sanitized = sys.argv[1:]
root = Path(root)
source = root / "tests/position_contracts/valid_positions.isa"
env = dict(
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
    for opt in ["-O0", "-O2"]:
        args = [compiler, source, opt, "--runtime-test-counters", "-o", binary]
        if sanitized:
            args += ["--sanitize"]
        result = command(args)
        assert result.returncode == 0, result.stderr
        result = command([binary], env=env)
        assert (result.returncode, result.stdout) == (0, "10\ntext!\ntext!\n0\n20\n20\n90\n"), (
            result
        )
        counters = re.fullmatch(r"ISAC_RT allocations=(\d+) frees=(\d+)\n", result.stderr)
        assert counters and counters[1] == counters[2], result.stderr
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
        result = command([compiler, source, "--emit-" + stage])
        assert result.returncode == 0, (stage, result.stderr)
        if stage in ["hir", "core"]:
            assert (
                "reference.transfer result=" in result.stdout and "reference.input" in result.stdout
            )
        if stage in ["regions", "core"]:
            assert "path=#V0.payload[0]" in result.stdout and "path=#V0.payload[1]" in result.stdout
        if stage in ["erased", "lir", "mlir", "llvm"]:
            for marker in ["reference.transfer", "reference.input", "RegionArgs", "carrier.path"]:
                assert marker not in result.stdout, (stage, marker)

for case, diagnostic in [
    ("invalid_local_escape", "E6003"),
    ("invalid_live_origin", "E0312"),
    ("invalid_equal_origin_overlap", "E0312"),
    ("invalid_owned_payload_result", "E6003"),
    ("invalid_missing_position_origin", "E6003"),
]:
    result = command(
        [compiler, root / "tests/position_contracts" / (case + ".isa"), "--check", "--color=never"]
    )
    assert result.returncode == 1 and f"error[{diagnostic}]" in result.stderr, (case, result)


for case in [
    "aggregate_syntax/invalid_stored_reference",
    "containers/invalid_vec_generic_reference_element",
    "containers/invalid_box_generic_reference_element",
    "containers/invalid_stored_reference_nested",
    "containers/invalid_vec_element_push_live",
    "references/invalid_payload_region_local_escape",
]:
    path = root / "tests" / (case + ".isa")
    assert path.exists(), path
    result = command([compiler, path, "--check", "--color=never"])
    assert result.returncode == 1 and "error[" in result.stderr, (case, result)
