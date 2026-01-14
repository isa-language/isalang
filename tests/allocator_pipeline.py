from pathlib import Path
import os
import re
import resource
import subprocess
import sys
import tempfile

compiler, root, clang = sys.argv[1:4]
root = Path(root)
sanitized = len(sys.argv) > 4
cflags = (
    ["-fsanitize=address,undefined", "-fno-sanitize-recover=all", "-fno-omit-frame-pointer"]
    if sanitized
    else []
)
env = {
    **os.environ,
    "ASAN_OPTIONS": "detect_leaks=1:halt_on_error=1",
    "UBSAN_OPTIONS": "halt_on_error=1",
}


def no_core():
    resource.setrlimit(resource.RLIMIT_CORE, (0, 0))


def build(arguments):
    result = subprocess.run([str(a) for a in arguments], capture_output=True, text=True)
    assert result.returncode == 0, result.stderr


def counters(result):
    assert result.returncode == 0, result
    match = re.fullmatch(r"ISAC_RT allocations=(\d+) frees=(\d+)\n", result.stderr)
    assert match and int(match[1]) > 0 and match[1] == match[2], result.stderr


provider = [
    root / "tests/allocator_provider.c",
    root / "runtime/src/isac_runtime.c",
    "-I",
    root / "runtime/include",
    "-DISAC_RT_TESTING",
]
with tempfile.TemporaryDirectory() as directory:
    directory = Path(directory)
    for opt in ["-O0", "-O2"]:
        binary = directory / "runtime"
        build(
            [
                clang,
                "-std=c11",
                "-Wall",
                "-Wextra",
                "-Werror",
                opt,
                *cflags,
                root / "tests/allocator_test.c",
                *provider,
                "-o",
                binary,
            ]
        )
        counters(subprocess.run([binary], capture_output=True, text=True, env=env, timeout=15))
        for failure in ["alloc", "zeroed", "realloc"]:
            result = subprocess.run(
                [binary, failure],
                capture_output=True,
                text=True,
                env=env,
                preexec_fn=no_core,
                timeout=15,
            )
            assert (
                result.returncode < 0 and "isac runtime panic: allocation failed" in result.stderr
            ), result
            assert (
                "AddressSanitizer" not in result.stderr and "runtime error:" not in result.stderr
            ), result.stderr
        obj = directory / ("program.ll" if sanitized else "program.o")
        build(
            [
                compiler,
                root / "tests/allocator_program.isa",
                opt,
                *(["--sanitize"] if sanitized else []),
                "--emit-llvm" if sanitized else "--emit-obj",
                "-o",
                obj,
            ]
        )
        binary = directory / "program"
        build([clang, opt, *cflags, obj, *provider, "-o", binary])
        result = subprocess.run(
            [binary, "one", "two"], capture_output=True, text=True, env=env, timeout=15
        )
        counters(result)
        assert result.stdout == "a\nb\nc\nbox\nargs=3\n", result.stdout
