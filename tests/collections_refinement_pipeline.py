from pathlib import Path
import os, re, resource, subprocess, sys, tempfile

compiler, root, *extra = sys.argv[1:]
cases = Path(root) / "tests/collections_refinement"
env = {
    **os.environ,
    "ASAN_OPTIONS": "detect_leaks=1:halt_on_error=1:detect_stack_use_after_return=1",
    "UBSAN_OPTIONS": "halt_on_error=1",
}
invalid = {
    "invalid_query_loan_release": ("E0312", "shared borrows"),
    "invalid_as_ref_owner_drop": ("E0312", "shared borrows"),
    "invalid_deque_live_front": ("E0312", "shared borrows"),
    "invalid_owned_key_move": ("E0301", "moved value"),
    "invalid_mutable_comparison": ("E0311", "shared borrow"),
}
for name, diagnostic in invalid.items():
    r = subprocess.run(
        [compiler, str(cases / (name + ".isa")), "--check"], capture_output=True, text=True
    )
    assert r.returncode != 0 and all(x in r.stderr for x in diagnostic), (name, r.stderr)
expected = {
    "owned_keys": "owned keys ok\n",
    "owned_ordering": "owned ordering ok\n",
    "borrowed_queries": "borrowed queries ok\n",
    "circular_deque": "circular deque ok\n",
}
with tempfile.TemporaryDirectory() as directory:
    directory = Path(directory)
    for name, output in expected.items():
        for optimization in ["-O0", "-O2"]:
            exe = directory / "program"
            cmd = [
                compiler,
                str(cases / (name + ".isa")),
                optimization,
                "--runtime-test-counters",
                "-o",
                str(exe),
            ]
            if extra:
                cmd += ["--sanitize"]
            r = subprocess.run(cmd, capture_output=True, text=True, timeout=90)
            assert r.returncode == 0, (name, optimization, r.stderr)
            r = subprocess.run([str(exe)], capture_output=True, text=True, env=env, timeout=30)
            assert r.returncode == 0 and r.stdout == output, (name, optimization, r)
            counts = re.fullmatch(r"ISAC_RT allocations=(\d+) frees=(\d+)\n", r.stderr)
            assert counts and counts[1] == counts[2], (name, r.stderr)
    for source in [
        '@import "std.algorithms" fn main(){let v=Vec<f64>::new();v.push(0.0/0.0);Algorithms<f64>::heap_sort(&mut v);}',
        '@import "std.collections" fn main(){let m=BTreeMap<f32,i32>::create();m.insert((0.0/0.0) as f32,1);}',
    ]:
        path = directory / "nan.isa"
        path.write_text(source)
        cmd = [compiler, str(path), "-o", str(directory / "nan")]
        if extra:
            cmd += ["--sanitize"]
        r = subprocess.run(cmd, capture_output=True, text=True)
        assert r.returncode == 0, r.stderr
        r = subprocess.run(
            [str(directory / "nan")],
            capture_output=True,
            text=True,
            env=env,
            preexec_fn=lambda: resource.setrlimit(resource.RLIMIT_CORE, (0, 0)),
        )
        assert r.returncode != 0 and "unordered comparison" in r.stderr, r.stderr
        assert "ERROR: AddressSanitizer" not in r.stderr and "runtime error:" not in r.stderr, (
            r.stderr
        )
for stage in ["hir", "core", "regions", "erased", "lir", "mlir", "llvm"]:
    r = subprocess.run(
        [compiler, str(cases / "borrowed_queries.isa"), "--emit-" + stage],
        capture_output=True,
        text=True,
    )
    assert r.returncode == 0, (stage, r.stderr)
    if stage == "hir":
        assert "payload.region" in r.stdout and "Option<&i32>" in r.stdout
    if stage in ["erased", "lir", "mlir", "llvm"]:
        assert (
            "payload.region" not in r.stdout
            and "LoanId" not in r.stdout
            and "RegionId" not in r.stdout
        )
