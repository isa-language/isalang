from pathlib import Path
import os
import re
import subprocess
import sys
import tempfile

compiler, root, *sanitized = sys.argv[1:]
root = Path(root)
env = dict(
    os.environ,
    ASAN_OPTIONS="detect_leaks=1:halt_on_error=1:detect_stack_use_after_return=1",
    UBSAN_OPTIONS="halt_on_error=1",
)


def run(args, **kw):
    return subprocess.run(list(map(str, args)), capture_output=True, text=True, timeout=90, **kw)


with tempfile.TemporaryDirectory() as directory:
    source, binary = Path(directory) / "worker.isa", Path(directory) / "program"
    cases = [
        ("i8", "-128i8", "return x;", "-128"),
        ("i16", "-32768i16", "return x;", "-32768"),
        ("i32", "-2147483648i32", "return x;", "-2147483648"),
        ("i64", "-9223372036854775808i64", "return x;", "-9223372036854775808"),
        ("u8", "255u8", "return x;", "255"),
        ("u16", "65535u16", "return x;", "65535"),
        ("u32", "4294967295u32", "return x;", "4294967295"),
        ("u64", "18446744073709551615u64", "return x;", "18446744073709551615"),
        ("usize", "42usize", "return x + 1usize;", "43"),
        ("isize", "-42isize", "return x - 1isize;", "-43"),
        (
            "i32",
            "10",
            "let sum = 0; for let i = 0; i < x; i++ {sum = sum + helper(i);} return sum;",
            "145",
        ),
        ("i32", "7", "if x == 0 {return 1;} return worker(x - 1) * 2;", "128"),
    ]
    runs = 0
    for optimization in ["-O0", "-O2"]:
        for ty, value, body, expected in cases:
            source.write_text(
                f"fn helper(x:i32)->i32{{return 3*x+1;}}\nfn worker(x:{ty})->{ty}{{{body}}}\n"
                f'fn main(){{@println("{{}}",worker({value}));@println("{{}}",worker({value}));}}'
            )
            flags = ["--sanitize"] if sanitized else []
            built = run(
                [
                    compiler,
                    source,
                    "--thread-call=worker",
                    optimization,
                    *flags,
                    "--runtime-test-counters",
                    "-o",
                    binary,
                ]
            )
            assert built.returncode == 0, (ty, body, built.stderr)
            done = run([binary], env=env)
            runs += 1
            assert done.returncode == 0 and done.stdout == (expected + "\n") * 2, (ty, body, done)
            balance = re.fullmatch(r"ISAC_RT allocations=(\d+) frees=(\d+)\n", done.stderr)
            assert balance and balance[1] == balance[2], done

    for helper in [
        '@println("not scalar");',
        'let s=String::from("heap");print(s);',
        "let r=&x;print(r);",
    ]:
        source.write_text(
            "fn helper(x:i32)->i32{"
            + helper
            + "return x;}\nfn worker(x:i32)->i32{return helper(x);}\n"
            "fn main(){print(worker(1));}"
        )
        bad = run([compiler, source, "--thread-call=worker", "--check", "--color=never"])
        assert bad.returncode == 1 and "E8006" in bad.stderr, bad
    source.write_text(
        'extern "C" fn abs(x:i32)->i32;\nfn worker(x:i32)->i32{unsafe{return abs(x);}}'
        "fn main(){print(worker(1));}"
    )
    bad = run([compiler, source, "--thread-call=worker", "--check", "--color=never"])
    assert bad.returncode == 1 and "E8006" in bad.stderr, bad
    for stage in ["core", "erased", "lir", "mlir", "llvm"]:
        dump = run(
            [
                compiler,
                root / "examples/scoped_worker.isa",
                "--thread-call=calculate",
                "--emit-" + stage,
            ]
        )
        assert dump.returncode == 0, (stage, dump.stderr)
        if stage == "core":
            assert "thread.run" in dump.stdout and "scoped-join" in dump.stdout
        else:
            assert "thread.run" not in dump.stdout
            assert "isac_rt_thread_run_u64" in dump.stdout
            for proof in ["RegionId", "LoanId", "MemoryState", "BorrowProvenance"]:
                assert proof not in dump.stdout
print(f"{runs} joined-worker native runs; call-graph gates and 5 dumps passed")
