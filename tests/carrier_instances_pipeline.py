from pathlib import Path
import os
import re
import subprocess
import sys
import tempfile

compiler, root, *sanitized = sys.argv[1:]
root = Path(root)
source = root / "tests/carrier_instances/valid_lifecycle.isa"
environment = dict(
    os.environ,
    ASAN_OPTIONS="detect_leaks=1:halt_on_error=1:detect_stack_use_after_return=1",
    UBSAN_OPTIONS="halt_on_error=1",
)


def command(args, **kwargs):
    return subprocess.run(
        list(map(str, args)), capture_output=True, text=True, timeout=60, **kwargs
    )


with tempfile.TemporaryDirectory() as directory:
    binary = Path(directory) / "program"
    for opt in ["-O0", "-O2"]:
        args = [compiler, source, opt, "--runtime-test-counters", "-o", binary]
        if sanitized:
            args += ["--sanitize"]
        result = command(args)
        assert result.returncode == 0, result.stderr
        result = command([binary], env=environment)
        assert (result.returncode, result.stdout) == (
            0,
            "10\n20\n0\ncarrier\n0\n30\n20\n21\n2\n",
        ), result
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
        for marker in [
            "carrier instance #C",
            "carrier transition",
            "carrier end",
            "carrier referent",
        ]:
            assert (marker in result.stdout) == (stage in ["core", "regions"]), (stage, marker)


for case in [
    "aggregate_syntax/invalid_stored_reference",
    "containers/invalid_vec_generic_reference_element",
    "containers/invalid_box_generic_reference_element",
    "containers/invalid_stored_reference_nested",
    "containers/invalid_box_element_consume_live",
    "references/invalid_payload_region_local_escape",
    "position_contracts/invalid_owned_payload_result",
]:
    result = command([compiler, root / "tests" / (case + ".isa"), "--check", "--color=never"])
    assert result.returncode == 1 and "error[" in result.stderr, (case, result)
