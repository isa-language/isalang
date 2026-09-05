from pathlib import Path
import os
import re
import subprocess
import sys
import tempfile

compiler, root, *extra = sys.argv[1:]
root = Path(root)
fixture = root / "tests/storage_epochs/valid_transfers.isa"
output = "2\n1\n6\n0\n42\n3\nabc!\n0\n" * 2
env = dict(
    os.environ,
    ASAN_OPTIONS="detect_leaks=1:halt_on_error=1:detect_stack_use_after_return=1",
    UBSAN_OPTIONS="halt_on_error=1",
)


def command(args):
    return subprocess.run(list(map(str, args)), capture_output=True, text=True, timeout=90)


for name in [
    "invalid_vec_element_push_live",
    "invalid_vec_element_pop_live",
    "invalid_vec_element_clear_live",
    "invalid_box_element_consume_live",
]:
    result = command(
        [compiler, root / "tests/containers" / (name + ".isa"), "--check", "--color=never"]
    )
    assert result.returncode != 0 and re.search(r"error\[E03\d+\]", result.stderr), result

with tempfile.TemporaryDirectory() as directory:
    executable = Path(directory) / "program"
    for optimization in ["-O0", "-O2"]:
        args = [compiler, fixture, optimization, "--runtime-test-counters", "-o", executable]
        if extra:
            args.append("--sanitize")
        result = command(args)
        assert result.returncode == 0, result.stderr
        result = subprocess.run(
            [str(executable)], capture_output=True, text=True, env=env, timeout=30
        )
        assert result.returncode == 0 and result.stdout == output, result
        counts = re.fullmatch(r"ISAC_RT allocations=(\d+) frees=(\d+)\n", result.stderr)
        assert counts and counts[1] == counts[2], result.stderr

for stage in ["core", "regions", "erased", "lir", "mlir", "llvm"]:
    result = command([compiler, fixture, "--emit-" + stage])
    assert result.returncode == 0, result.stderr
    markers = ["storage identity #S", "storage epoch #E", "storage transition"]
    if stage in ["core", "regions"]:
        assert all(marker in result.stdout for marker in markers)
        assert "unknown-mutable-call" in result.stdout
        assert "may-reallocate" in result.stdout
        assert "invalidate-elements" in result.stdout
        assert "release-backing-storage" in result.stdout
    else:
        assert all(marker not in result.stdout for marker in markers)
