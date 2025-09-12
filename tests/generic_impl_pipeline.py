from pathlib import Path
import os
import subprocess
import sys
import tempfile

compiler, root = sys.argv[1:3]
root = Path(root)
flags = ["--sanitize"] if len(sys.argv) > 3 else []
environment = {
    **os.environ,
    "ASAN_OPTIONS": "detect_leaks=1:halt_on_error=1",
    "UBSAN_OPTIONS": "halt_on_error=1",
}


def build(source, output, opt, counters=False):
    command = [compiler, str(source), opt, *flags, "-o", str(output)]
    if counters:
        command.append("--runtime-test-counters")
    result = subprocess.run(command, capture_output=True, text=True)
    assert result.returncode == 0, result.stderr


with tempfile.TemporaryDirectory() as directory:
    directory = Path(directory)
    for fixture, expected in [
        ("valid_generic_struct_return", "31\n1\n"),
        ("valid_option_unwrap", "1\n42\nhello\n17\n"),
        ("valid_heap_function_return", "returned\nother\n"),
    ]:
        for opt in ["-O0", "-O2"]:
            binary = directory / "program"
            build(root / "tests/containers" / (fixture + ".isa"), binary, opt)
            result = subprocess.run([str(binary)], capture_output=True, text=True, env=environment)
            assert (result.returncode, result.stdout, result.stderr) == (0, expected, ""), result
    source = directory / "none.isa"
    source.write_text("fn main() { let empty: Option<i32> = Option::None; print(empty.unwrap()); }")
    for opt in ["-O0", "-O2"]:
        binary = directory / "none"
        build(source, binary, opt)
        result = subprocess.run([str(binary)], capture_output=True, text=True, env=environment)
        assert result.returncode != 0 and "called Option::unwrap() on None" in result.stderr, result
        assert (
            "ERROR: AddressSanitizer" not in result.stderr and "runtime error:" not in result.stderr
        )
    source.write_text(
        'fn main() { let value=Option::Some(String::from("one")); let text=value.unwrap(); print(text); }'
    )
    binary = directory / "counted"
    build(source, binary, "-O2", True)
    result = subprocess.run([str(binary)], capture_output=True, text=True, env=environment)
    assert result.returncode == 0 and result.stdout == "one\n", result
    assert "allocations=1 frees=1" in result.stderr, result
    source = root / "tests/containers/valid_generic_struct_return.isa"
    for stage in ["ast", "hir", "core", "erased", "lir", "mlir", "llvm"]:
        result = subprocess.run(
            [compiler, str(source), "--emit-" + stage], capture_output=True, text=True
        )
        assert result.returncode == 0 and result.stdout.strip(), result.stderr
        if stage == "hir":
            assert "generic struct #G" in result.stdout and "#TP" in result.stdout
            assert "Pair<i32,bool>::make" in result.stdout
