from pathlib import Path
import signal
import subprocess
import sys
import tempfile

compiler, root = sys.argv[1:3]
root = Path(root)
with tempfile.TemporaryDirectory() as temporary:
    tmp = Path(temporary)
    for name in ("valid_random", "valid_geometry", "valid_assert_lazy"):
        source = root / "tests/std_foundations" / (name + ".isa")
        for opt in ("-O0", "-O2"):
            executable = tmp / (name + opt)
            subprocess.run([compiler, str(source), opt, "-o", str(executable)], check=True)
            run = subprocess.run([str(executable)], capture_output=True, timeout=10)
            assert run.returncode == 0 and run.stdout == b"1\n" and not run.stderr, run
    for name, program, message in (
        ("assertion", '@assert(false, "value {}", 42);', b"value 42"),
        ("panic", '@panic("Ol\u00e1 {}", 7);', "Ol\u00e1 7".encode()),
        ("range", "let rng = Random::new(); rng.range(4, 4);", b"low < high"),
        ("clamp", "clamp_i32(0, 2, 1);", b"low <= high"),
    ):
        source = tmp / (name + ".isa")
        source.write_text(
            '@import "std.random"\n@import "std.math"\nfn main() -> i32 {'
            + program
            + " return 0; }\n"
        )
        executable = tmp / name
        subprocess.run([compiler, str(source), "-O2", "-o", str(executable)], check=True)

        def no_core():
            import resource

            resource.setrlimit(resource.RLIMIT_CORE, (0, 0))

        run = subprocess.run([str(executable)], capture_output=True, timeout=10, preexec_fn=no_core)
        assert run.returncode == -signal.SIGABRT, (name, run.returncode, run.stderr)
        assert b"isac runtime panic: " in run.stderr and message in run.stderr, run.stderr
    source = root / "tests/std_foundations/valid_random.isa"
    for emit in (
        "--emit-hir",
        "--emit-core",
        "--emit-erased",
        "--emit-lir",
        "--emit-mlir",
        "--emit-llvm",
    ):
        dump = subprocess.run(
            [compiler, str(source), emit], capture_output=True, text=True, check=True
        ).stdout
        assert dump
        if emit in ("--emit-erased", "--emit-lir", "--emit-llvm"):
            for proof in ("BorrowShared", "BorrowUnique", "RegionId", "quantity="):
                assert proof not in dump, (emit, proof)
