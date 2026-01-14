from pathlib import Path
import os, re, resource, subprocess, sys, tempfile

compiler, root, *extra = sys.argv[1:]
root = Path(root)
cases = root / "tests/std_expansion"
env = {
    **os.environ,
    "ASAN_OPTIONS": "detect_leaks=1:halt_on_error=1:detect_stack_use_after_return=1",
    "UBSAN_OPTIONS": "halt_on_error=1",
    "ISAC_STD_TEST_VAR": "environment",
    "ISAC_STD_INVALID_UTF8": os.fsdecode(b"\xff"),
}
env.pop("ISAC_STD_TEST_MISSING", None)
expected = {
    "matrix": ("owned\nMatriz ok\n", 12),
    "sequences": ("second\nsecond\nfirst\nfifo\ncell\nsequences ok\n", 31),
    "maps": ("value\nvalue\nreplacement\nreplacement\nnegative\nnegative\nmaps ok\n", 1034),
    "ordering": ("ordering ok\n", 27),
    "aggregate_results": ("nested\nitem\nsuffix\nitem\nresult\nerror\n", 17),
    "systems": (
        "folder/Olá.txt\nOlá.txt\nOlá, filesystem!\n\n0\n1\nargument;literal\nenvironment\nsystems ok\n",
        None,
    ),
}
invalid = {
    "invalid_borrowed_map_mutation": ("E0312", "shared borrows"),
    "invalid_borrowed_matrix_drop": ("E0312", "shared borrows"),
    "invalid_sequence_use_after_move": ("E0301", "moved value"),
    "invalid_linear_fill": ("E0302", "cannot be copied"),
    "invalid_linear_sort": ("E0102", "compare"),
    "invalid_uninitialized_return": ("E0301", "uninitialized or moved"),
    "invalid_partial_return": ("E0301", "uninitialized or moved"),
    "invalid_recursive_generic": ("E0204", "recursive generic field"),
    "invalid_protocol_return": ("E0403", "initial"),
    "invalid_protocol_storage": ("E0611", "element"),
    "invalid_hash_key": ("E0102", "hash"),
}
for name, diagnostic in invalid.items():
    r = subprocess.run(
        [compiler, str(cases / (name + ".isa")), "--check"], capture_output=True, text=True
    )
    assert r.returncode != 0 and all(part in r.stderr for part in diagnostic), (name, r)
for stage in ["ast", "hir", "core", "erased", "lir", "mlir", "llvm"]:
    r = subprocess.run(
        [compiler, str(cases / "aggregate_results.isa"), "--emit-" + stage],
        capture_output=True,
        text=True,
    )
    assert r.returncode == 0, (stage, r.stderr)
    if stage == "hir":
        assert "Item<String>" in r.stdout
    if stage == "core":
        assert "container.call" in r.stdout
    if stage in ["erased", "lir", "mlir", "llvm"]:
        assert (
            "container.call" not in r.stdout
            and "RegionId" not in r.stdout
            and "LoanId" not in r.stdout
        )
        assert "isac_rt_vec_remove_slot" in r.stdout
with tempfile.TemporaryDirectory() as directory:
    directory = Path(directory)
    for name, (stdout, count) in expected.items():
        for optimization in ["-O0", "-O2"]:
            executable = directory / "program"
            cmd = [
                compiler,
                str(cases / (name + ".isa")),
                optimization,
                "--runtime-test-counters",
                "-o",
                str(executable),
            ]
            if extra:
                cmd += ["--sanitize"]
            r = subprocess.run(cmd, capture_output=True, text=True, timeout=90)
            assert r.returncode == 0, (name, optimization, r.stderr)
            run_args = []
            if name == "systems":
                invalid_file = directory / "invalid.bin"
                invalid_file.write_bytes(b"\xff\xfe")
                run_args = [str(directory / "text.txt"), str(invalid_file)]
            r = subprocess.run(
                [str(executable), *run_args], env=env, capture_output=True, text=True, timeout=30
            )
            assert r.returncode == 0 and r.stdout == stdout, (name, optimization, r)
            counters = re.fullmatch(r"ISAC_RT allocations=(\d+) frees=(\d+)\n", r.stderr)
            assert counters and int(counters[1]) == int(counters[2]), (name, r.stderr)
            if count is not None:
                assert int(counters[1]) == count, (name, r.stderr)
            if name == "systems":
                (directory / "text.txt.dir").rmdir()

    failures = [
        (
            "Matriz dimensions overflow",
            '@import "std.matriz" fn main(){let x=Matriz<i32>::filled(0usize-1,2,0);}',
        ),
        (
            "Matriz shape does not match storage",
            '@import "std.matriz" fn main(){let v=Vec<i32>::new();let x=Matriz<i32>::from_vec(1,1,v.move());}',
        ),
        ("index out of bounds", '@import "std.vec" fn main(){let v=Vec<i32>::new();v.remove(0);}'),
        (
            "unordered comparison",
            '@import "std.collections" fn main(){let h=BinaryHeap<f64>::create();h.push(0.0/0.0);}',
        ),
        ("duration overflow", '@import "std.time" fn main(){let d=Duration::from_millis(0u64-1);}'),
    ]
    for message, source in failures:
        path = directory / "failure.isa"
        path.write_text(source)
        cmd = [compiler, str(path), "-o", str(directory / "failure")]
        if extra:
            cmd += ["--sanitize"]
        r = subprocess.run(cmd, capture_output=True, text=True)
        assert r.returncode == 0, (message, r.stderr)
        r = subprocess.run(
            [str(directory / "failure")],
            env=env,
            capture_output=True,
            text=True,
            preexec_fn=lambda: resource.setrlimit(resource.RLIMIT_CORE, (0, 0)),
        )
        assert r.returncode != 0 and ("isac runtime panic: " + message) in r.stderr, (
            message,
            r.stderr,
        )
        assert "ERROR: AddressSanitizer" not in r.stderr and "runtime error:" not in r.stderr, (
            r.stderr
        )
