import os
from pathlib import Path
import re
import subprocess
import sys
import tempfile

compiler, root = sys.argv[1:3]
sanitized = len(sys.argv) > 3
root = Path(root)
environment = {
    **os.environ,
    "ASAN_OPTIONS": "detect_leaks=1:halt_on_error=1",
    "UBSAN_OPTIONS": "halt_on_error=1",
}


def command(args):
    result = subprocess.run(args, capture_output=True, text=True, timeout=60)
    assert result.returncode == 0, (args, result.stderr)
    return result.stdout


with tempfile.TemporaryDirectory() as directory:
    directory = Path(directory)
    source = root / "examples/env/arguments.isa"
    for opt in ["-O0", "-O2"]:
        executable = directory / "arguments"
        flags = ["--sanitize"] if sanitized else []
        command([compiler, str(source), opt, *flags, "-o", str(executable)])
        for args, code, output in [
            ([], 1, "missing argument\n"),
            (["Olá"], 0, "argument = Olá\nowned = Olá\n"),
            ([""], 0, "argument = \nowned = \n"),
        ]:
            result = subprocess.run(
                [str(executable), *args], capture_output=True, text=True, env=environment
            )
            assert (result.returncode, result.stdout, result.stderr) == (code, output, ""), result
        result = subprocess.run(
            [os.fsencode(executable), b"\xff"], capture_output=True, env=environment
        )
        assert result.returncode != 0 and not result.stdout, result
        assert b"command-line argument is not valid UTF-8" in result.stderr, result.stderr

    count = directory / "count.isa"
    count.write_text(
        '@import "std.env" fn main() { print(argument_count()); print(argument(0)); print(argument(3)); }'
    )
    executable = directory / "count"
    command([compiler, str(count), *flags, "-o", str(executable)])
    result = subprocess.run(
        [str(executable), "one", "two", "three"], capture_output=True, text=True, env=environment
    )
    assert (result.returncode, result.stdout, result.stderr) == (
        0,
        f"4\n{executable}\nthree\n",
        "",
    ), result
    result = subprocess.run([str(executable)], capture_output=True, text=True, env=environment)
    assert result.returncode != 0 and "argument index out of bounds" in result.stderr, result

    executable = directory / "counters"
    counter_source = directory / "counters.isa"
    counter_source.write_text(
        '@import "std.env" fn main() { let copy = String::from(argument(1)); print(copy); copy.clear(); print(argument(1)); }'
    )
    command(
        [compiler, str(counter_source), "--runtime-test-counters", *flags, "-o", str(executable)]
    )
    result = subprocess.run(
        [str(executable), "hello"], capture_output=True, text=True, env=environment
    )
    assert result.returncode == 0 and result.stdout == "hello\nhello\n", result
    assert "allocations=4 frees=4" in result.stderr, result.stderr

    for stage in ["ast", "hir", "core", "erased", "lir", "mlir", "llvm"]:
        text = command([compiler, str(source), "--emit-" + stage])
        assert text.strip(), stage
        if stage == "erased":
            entry = text.split("fn main\n")[1]
            assert (
                entry.index("isac_rt_env_init")
                < entry.index("_main")
                < entry.index("isac_rt_env_destroy")
            ), entry
        if stage == "llvm":
            assert re.search(r"define i32 @main\(i32 .*ptr ", text), text
