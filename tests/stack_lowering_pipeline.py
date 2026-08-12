import os
from pathlib import Path
import re
import resource
import signal
import subprocess
import sys
import tempfile

compiler, root, *extra = sys.argv[1:]
root = Path(root)
sanitized = bool(extra)
budget = 8 * 1024 * 1024 if sanitized else 1024 * 1024
env = dict(
    os.environ,
    ASAN_OPTIONS="detect_leaks=1:halt_on_error=1:detect_stack_use_after_return=1",
    UBSAN_OPTIONS="halt_on_error=1",
)


def limits():
    resource.setrlimit(resource.RLIMIT_CORE, (0, 0))
    _, hard = resource.getrlimit(resource.RLIMIT_STACK)
    assert hard == resource.RLIM_INFINITY or hard >= budget, "stack budget unavailable"
    resource.setrlimit(resource.RLIMIT_STACK, (budget, budget))


def compile_program(source, output, level):
    command = [compiler, str(source), level, "-o", str(output)]
    if sanitized:
        command.append("--sanitize")
    result = subprocess.run(command, capture_output=True, text=True, timeout=120)
    assert result.returncode == 0, (command, result.stderr)


def run(program):
    return subprocess.run(
        [str(program)], capture_output=True, text=True, env=env, preexec_fn=limits, timeout=90
    )


def entry_allocations_only(text):

    functions = re.findall(r"define [^\n]*@isa_\w+\([^\n]*\)[^\n]*\{\n(.*?)\n\}", text, re.S)
    assert functions, "no generated Isac functions"
    for body in functions:
        entry = True
        for line in body.splitlines():
            if re.match(r"^[\w.]+:", line):
                entry = False
            if re.search(r"\balloca\b", line):
                assert entry, ("stack allocation outside entry", line)


with tempfile.TemporaryDirectory() as directory:
    directory = Path(directory)
    program = directory / "program"
    source = root / "tests/stack_lowering/long_loops.isa"
    levels = ["-O0", "-O2"] if sanitized else ["-O0", "-O1", "-O2", "-O3", "-Os", "-Oz"]
    for level in levels:
        compile_program(source, program, level)
        result = run(program)
        assert result.returncode == 0 and result.stdout == "stack-ok\n" and not result.stderr, (
            level,
            result,
        )

    source = root / "docs/audit/2026-10-11-stack-overflow/tests/emulator/repro_stack_loop.isa"
    compile_program(source, program, "-O0")
    result = run(program)
    assert result.returncode == 0 and not result.stdout and not result.stderr, result

    for target in [
        "x86_64-unknown-linux-gnu",
        "i686-unknown-linux-gnu",
        "aarch64-unknown-linux-gnu",
    ]:
        result = subprocess.run(
            [compiler, str(source), "-O0", "--target=" + target, "--emit-llvm"],
            capture_output=True,
            text=True,
            timeout=60,
        )
        if result.returncode != 0 and "target unavailable" in result.stderr:
            continue
        assert result.returncode == 0, (target, result.stderr)
        entry_allocations_only(result.stdout)
        assert "@isac_rt_index_check" in result.stdout
        assert re.search(r"extractvalue .*?, 3, 0", result.stdout), (
            target,
            "length should be extracted directly from descriptor",
        )

    if not sanitized:
        result = subprocess.run(
            [compiler, str(root / "tests/stack_lowering/long_loops.isa"), "-O0", "--emit-llvm"],
            capture_output=True,
            text=True,
            timeout=60,
        )
        assert result.returncode == 0, result.stderr
        entry_allocations_only(result.stdout)

    for access in ["return values[index];", "values[index] = 1u8; return 0u8;"]:
        unique = access.startswith("values[")
        source = directory / "out-of-bounds.isa"
        source.write_text(
            "fn access(values: &"
            + ("mut " if unique else "")
            + "u8[], index: usize) -> u8 { "
            + access
            + " }\n"
            "fn main() { let values: u8[1] = [1u8]; "
            "let value = access(&" + ("mut " if unique else "") + "values, 1usize); }\n"
        )
        for level in ["-O0", "-O2"]:
            compile_program(source, program, level)
            result = run(program)
            assert (
                result.returncode == -signal.SIGABRT and "index out of bounds" in result.stderr
            ), (unique, level, result)
            assert "AddressSanitizer" not in result.stderr and "runtime error:" not in result.stderr
