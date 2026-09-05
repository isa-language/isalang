from pathlib import Path
import os
import re
import subprocess
import sys
import tempfile

compiler, root, *sanitized = sys.argv[1:]
root = Path(root)
source = root / "tests/storage_slots/valid_slots.isa"
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
    for opt in ["-O0", "-O2"]:
        args = [compiler, source, opt, "--runtime-test-counters", "-o", binary]
        if sanitized:
            args += ["--sanitize"]
        result = command(args)
        assert result.returncode == 0, result.stderr
        result = command([binary], env=environment)
        assert (result.returncode, result.stdout) == (0, "30\n20\n50\n9\n80\n90\n91\n1\n202\n"), (
            result
        )
        counts = re.fullmatch(r"ISAC_RT allocations=(\d+) frees=(\d+)\n", result.stderr)
        assert counts and counts[1] == counts[2], result.stderr
    for stage in ["ast", "hir", "core", "regions", "erased", "lir", "mlir", "llvm"]:
        result = command([compiler, source, "--emit-" + stage])
        assert result.returncode == 0, (stage, result.stderr)
        markers = [
            "slot identity #C",
            "slot transition",
            "slot invalidation",
            "slot=.element[0] initialized=yes",
        ]
        for marker in markers:
            assert (marker in result.stdout) == (stage in ["core", "regions"]), (stage, marker)


for case in [
    "aggregate_syntax/invalid_stored_reference",
    "containers/invalid_vec_generic_reference_element",
    "containers/invalid_box_generic_reference_element",
    "containers/invalid_vec_element_push_live",
    "containers/invalid_vec_element_shared_mutation",
    "containers/invalid_box_element_consume_live",
]:
    path = root / "tests" / (case + ".isa")
    assert path.exists(), path
    result = command([compiler, path, "--check", "--color=never"])
    assert result.returncode != 0 and "error[" in result.stderr, (case, result)
