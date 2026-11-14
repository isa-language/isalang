from pathlib import Path
import os
import subprocess
import sys
import tempfile

clang, root, *sanitized = sys.argv[1:]
root = Path(root)
with tempfile.TemporaryDirectory() as directory:
    binary = Path(directory) / "runtime-threads"
    command = [
        clang,
        "-std=c11",
        "-O1",
        "-Wall",
        "-Wextra",
        "-Werror",
        "-pthread",
        "-DISAC_RT_TESTING",
        "-I" + str(root / "runtime/include"),
        str(root / "tests/runtime_concurrency.c"),
        str(root / "runtime/src/isac_runtime.c"),
        str(root / "runtime/src/isac_allocator_libc.c"),
        "-lm",
        "-o",
        str(binary),
    ]
    if sanitized:
        command += ["-fsanitize=address,undefined"]
    built = subprocess.run(command, capture_output=True, text=True, timeout=60)
    assert built.returncode == 0, built.stderr
    environment = dict(
        os.environ,
        ASAN_OPTIONS="detect_leaks=1:halt_on_error=1:detect_stack_use_after_return=1",
        UBSAN_OPTIONS="halt_on_error=1",
    )
    result = subprocess.run(
        [str(binary)], capture_output=True, text=True, timeout=60, env=environment
    )
    assert (
        result.returncode == 0
        and not result.stdout
        and result.stderr == "ISAC_RT allocations=16000 frees=16000\n"
    ), result
