from pathlib import Path
import os
import subprocess
import sys
import tempfile

compiler, root = sys.argv[1:3]
root = Path(root)
sanitized = len(sys.argv) > 3
flags = ["--sanitize"] if sanitized else []
environment = {
    **os.environ,
    "ASAN_OPTIONS": "detect_leaks=1:halt_on_error=1",
    "UBSAN_OPTIONS": "halt_on_error=1",
}


def run(args):
    result = subprocess.run(args, capture_output=True, text=True, timeout=60)
    assert result.returncode == 0, (args, result.stderr)
    return result.stdout


with tempfile.TemporaryDirectory() as directory:
    directory = Path(directory)
    for fixture, expected in [
        ("valid_shared_slice", "6\n9\n3\n0\n2\n"),
        ("valid_mut_slice", "7\n42\n"),
        ("valid_string_char_at", "a\ná\n😀\nmiddle\nend\noutside\nempty\n"),
        ("valid_string_find_from", "1\n8\n3\n99\n99\n99\n"),
    ]:
        source = root / "tests/containers" / (fixture + ".isa")
        for opt in ["-O0", "-O2"]:
            executable = directory / fixture
            run([compiler, str(source), opt, *flags, "-o", str(executable)])
            result = subprocess.run(
                [str(executable)], capture_output=True, text=True, env=environment
            )
            assert (result.returncode, result.stdout, result.stderr) == (0, expected, ""), result

    for operation in ["print(values[index]);", "values[index] = 9;"]:
        source = directory / "bounds.isa"
        source.write_text(
            "fn access(values: &mut i32[], index: usize) { "
            + operation
            + " } fn main() { let data = [1,2]; access(&mut data,2); }"
        )
        for opt in ["-O0", "-O2"]:
            executable = directory / "bounds"
            run([compiler, str(source), opt, *flags, "-o", str(executable)])
            result = subprocess.run(
                [str(executable)], capture_output=True, text=True, env=environment
            )
            assert result.returncode != 0 and "index out of bounds" in result.stderr, result
            assert (
                "ERROR: AddressSanitizer" not in result.stderr
                and "runtime error:" not in result.stderr
            )

    source = root / "tests/containers/valid_shared_slice.isa"
    for stage in ["ast", "hir", "core", "erased", "lir", "mlir", "llvm"]:
        dump = run([compiler, str(source), "--emit-" + stage])
        assert dump.strip()
        if stage == "core":
            assert "borrow.shared" in dump and "region.contract" in dump
        if stage == "erased":
            assert "slice.view" in dump and "index.check" in dump
        if stage == "mlir":
            assert "isac.view" in dump and "isac.index_check" in dump
        if stage == "llvm":
            assert "isac_rt_index_check" in dump
