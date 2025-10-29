from pathlib import Path
import subprocess, sys, tempfile, os

compiler, root, *extra = sys.argv[1:]
source = Path(root) / "tests/containers/valid_owned_field_impl_cleanup.isa"
for stage in ["ast", "hir", "core", "erased", "lir", "mlir", "llvm"]:
    out = subprocess.run([compiler, str(source), "--emit-" + stage], capture_output=True, text=True)
    assert out.returncode == 0, out.stderr
    if stage == "core":
        assert "field.store" in out.stdout and "field.take" in out.stdout
    if stage in ["erased", "lir", "mlir", "llvm"]:
        assert "field.take" not in out.stdout
with tempfile.TemporaryDirectory() as directory:
    for opt in ["-O0", "-O2"]:
        exe = Path(directory) / ("program" + opt)
        args = [compiler, str(source), opt, "--runtime-test-counters", "-o", str(exe)]
        if extra:
            args += ["--sanitize"]
        built = subprocess.run(args, capture_output=True, text=True)
        assert built.returncode == 0, built.stderr
        run = subprocess.run(
            [str(exe)],
            capture_output=True,
            text=True,
            env={
                **os.environ,
                "ASAN_OPTIONS": "detect_leaks=1:halt_on_error=1",
                "UBSAN_OPTIONS": "halt_on_error=1",
            },
        )
        assert (run.returncode, run.stdout, run.stderr) == (
            0,
            "new\n",
            "ISAC_RT allocations=3 frees=3\n",
        ), run


for name, count, output in [
    ("valid_nested_owned_take", 2, "b\n"),
    ("valid_nested_owned_drop", 2, ""),
    ("valid_nested_owned_replace", 4, ""),
    ("valid_nested_owned_parameter", 2, "a\n"),
]:
    source = Path(root) / "tests/containers" / (name + ".isa")
    with tempfile.TemporaryDirectory() as directory:
        for opt in ["-O0", "-O2"]:
            exe = Path(directory) / ("nested" + opt)
            args = [compiler, str(source), opt, "--runtime-test-counters", "-o", str(exe)]
            if extra:
                args += ["--sanitize"]
            built = subprocess.run(args, capture_output=True, text=True)
            assert built.returncode == 0, built.stderr
            run = subprocess.run(
                [str(exe)],
                capture_output=True,
                text=True,
                env={
                    **os.environ,
                    "ASAN_OPTIONS": "detect_leaks=1:halt_on_error=1",
                    "UBSAN_OPTIONS": "halt_on_error=1",
                },
            )
            assert (run.returncode, run.stdout, run.stderr) == (
                0,
                output,
                f"ISAC_RT allocations={count} frees={count}\n",
            ), run
