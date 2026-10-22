from pathlib import Path
import os
import re
import subprocess
import sys
import tempfile

compiler, root, *sanitized = sys.argv[1:]
root = Path(root)
cases = root / "tests/local_carrier_fields"
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
        ("valid_fields", "7\n10\n20\n30\n30\nstored\n0\n"),
        ("valid_replace", "3\n2\n4\n5\n"),
        ("valid_paths", "1\n43\n50\n50\n62\n62\n62\n"),
        ("valid_multiple_origins", "21\n11\n"),
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
        result = command([compiler, cases / "valid_fields.isa", "--emit-" + stage])
        assert result.returncode == 0, (stage, result.stderr)
        for marker in ["field RegionArgs", "carrier instance"]:
            assert (marker in result.stdout) == (stage in ["core", "regions"]), (stage, marker)

for source in sorted(cases.glob("invalid_*.isa")):
    result = command([compiler, source, "--emit-mlir", "--color=never"])
    assert result.returncode == 1 and "error[" in result.stderr and not result.stdout, (
        source,
        result,
    )


for case in [
    "references/invalid_reference_field",
    "containers/invalid_vec_generic_reference_element",
    "containers/invalid_box_generic_reference_element",
    "containers/invalid_stored_reference_nested",
    "position_contracts/invalid_owned_payload_result",
]:
    result = command([compiler, root / "tests" / (case + ".isa"), "--check", "--color=never"])
    assert result.returncode == 1 and "error[" in result.stderr, (case, result)
