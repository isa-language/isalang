from pathlib import Path
import os
import re
import subprocess
import sys
import tempfile

compiler, root, *extra = sys.argv[1:]
cases = Path(root) / "tests/projection_storage"
expected = {
    "valid_nested": "3\nold\n8\n4\n4\n3\nold\n8\n",
    "valid_parameter": "6\n3\n3\n",
    "valid_branch": "4\n4\n",
    "valid_protocol": "4\n",
    "valid_nested_assignment": "3\nnew\n",
}
errors = {
    "invalid_absent": "E0301",
    "invalid_destroy_live": "E0312",
    "invalid_return_local": "E6003",
}
assert {p.stem for p in cases.glob("*.isa")} == set(expected) | set(errors)
env = dict(
    os.environ,
    ASAN_OPTIONS="detect_leaks=1:halt_on_error=1:detect_stack_use_after_return=1",
    UBSAN_OPTIONS="halt_on_error=1",
)


def command(args):
    return subprocess.run(list(map(str, args)), capture_output=True, text=True, timeout=90)


for name, code in errors.items():
    result = command([compiler, cases / (name + ".isa"), "--check", "--color=never"])
    assert result.returncode != 0 and code in result.stderr, (name, result)

with tempfile.TemporaryDirectory() as directory:
    executable = Path(directory) / "program"
    for name, output in expected.items():
        for optimization in ["-O0", "-O2"]:
            args = [
                compiler,
                cases / (name + ".isa"),
                optimization,
                "--runtime-test-counters",
                "-o",
                executable,
            ]
            if extra:
                args.append("--sanitize")
            result = command(args)
            assert result.returncode == 0, (name, result.stderr)
            result = subprocess.run(
                [str(executable)], capture_output=True, text=True, env=env, timeout=30
            )
            assert result.returncode == 0 and result.stdout == output, (name, optimization, result)
            counts = re.fullmatch(r"ISAC_RT allocations=(\d+) frees=(\d+)\n", result.stderr)
            assert counts and counts[1] == counts[2], (name, result.stderr)
    for name in ["valid_nested", "valid_parameter", "valid_nested_assignment"]:
        for stage in ["core", "regions", "region-constraints", "erased", "lir", "mlir", "llvm"]:
            result = command([compiler, cases / (name + ".isa"), "--emit-" + stage])
            assert result.returncode == 0, (name, stage, result.stderr)
            if stage == "regions":
                assert "projection storage bound" in result.stdout
            elif stage in ["core", "region-constraints"]:
                assert "ProjectionStorage" in result.stdout
            else:
                assert "ProjectionStorage" not in result.stdout
                assert "projection storage bound" not in result.stdout
