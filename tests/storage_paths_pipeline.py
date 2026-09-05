from pathlib import Path
import os
import re
import subprocess
import sys
import tempfile

compiler, root, *extra = sys.argv[1:]
root = Path(root)
expected = {
    "valid_fields": "old\nnew!\nsafe\ncallee\n",
    "valid_elements": "30\n10\n30\n10\n1\ndeep\n",
}
environment = dict(
    os.environ,
    ASAN_OPTIONS="detect_leaks=1:halt_on_error=1:detect_stack_use_after_return=1",
    UBSAN_OPTIONS="halt_on_error=1",
)


def command(args):
    return subprocess.run(list(map(str, args)), capture_output=True, text=True, timeout=90)


with tempfile.TemporaryDirectory() as directory:
    executable = Path(directory) / "program"
    for name, output in expected.items():
        source = root / "tests/storage_paths" / (name + ".isa")
        for optimization in ["-O0", "-O2"]:
            args = [compiler, source, optimization, "--runtime-test-counters", "-o", executable]
            if extra:
                args += ["--sanitize"]
            result = command(args)
            assert result.returncode == 0, (name, result.stderr)
            result = subprocess.run(
                [str(executable)], capture_output=True, text=True, timeout=30, env=environment
            )
            assert (result.returncode, result.stdout) == (0, output), (name, result)
            counts = re.fullmatch(r"ISAC_RT allocations=(\d+) frees=(\d+)\n", result.stderr)
            assert counts and counts[1] == counts[2], (name, result.stderr)
        for stage in ["core", "regions", "erased", "lir", "mlir", "llvm"]:
            result = command([compiler, source, "--emit-" + stage])
            assert result.returncode == 0, (name, stage, result.stderr)
            if stage in ["core", "regions"]:
                assert "storage identity" in result.stdout
                if name == "valid_elements":
                    assert "element bound" in result.stdout
                    assert "guard=Option::Some" in result.stdout
                    assert "guard=initialized-Box" in result.stdout
                    assert ".element[*].element[0].element[*]" in result.stdout
            else:
                for marker in ["storage identity", "storage epoch", "element bound"]:
                    assert marker not in result.stdout, (name, stage, marker)


for source in [
    "tests/aggregate_syntax/invalid_stored_reference.isa",
    "tests/containers/invalid_vec_generic_reference_element.isa",
    "tests/containers/invalid_box_generic_reference_element.isa",
    "tests/containers/invalid_vec_element_push_live.isa",
    "tests/containers/invalid_box_element_consume_live.isa",
    "tests/projection_storage/invalid_destroy_live.isa",
]:
    result = command([compiler, root / source, "--check", "--color=never"])
    assert result.returncode != 0 and "error[" in result.stderr, (source, result)
