from pathlib import Path
import json, os, re, resource, subprocess, sys, tempfile

compiler, root, *extra = sys.argv[1:]
cases = Path(root) / "tests/container_mutation"
expected = json.loads((cases / "expectations.json").read_text())
env = {
    **os.environ,
    "ASAN_OPTIONS": "detect_leaks=1:halt_on_error=1:detect_stack_use_after_return=1",
    "UBSAN_OPTIONS": "halt_on_error=1",
}
for name, expectation in expected.items():
    if name.startswith("invalid_"):
        r = subprocess.run(
            [compiler, str(cases / (name + ".isa")), "--check"],
            capture_output=True,
            text=True,
            timeout=30,
        )
        assert r.returncode != 0 and "error[E9001]" not in r.stderr, (name, r.stderr)
        assert expectation["error"] in r.stderr, (name, r.stderr)
with tempfile.TemporaryDirectory() as directory:
    directory = Path(directory)
    for name, expectation in expected.items():
        if not name.startswith("valid_"):
            continue
        for optimization in ["-O0", "-O2"]:
            cmd = [
                compiler,
                str(cases / (name + ".isa")),
                optimization,
                "--runtime-test-counters",
                "-o",
                str(directory / "program"),
            ]
            if extra:
                cmd += ["--sanitize"]
            r = subprocess.run(cmd, capture_output=True, text=True, timeout=90)
            assert r.returncode == 0, (name, optimization, r.stderr)
            r = subprocess.run(
                [str(directory / "program")], capture_output=True, text=True, env=env, timeout=20
            )
            assert r.returncode == 0 and r.stdout == expectation["stdout"], (name, optimization, r)
            counts = re.fullmatch(r"ISAC_RT allocations=(\d+) frees=(\d+)\n", r.stderr)
            assert counts and counts[1] == counts[2], (name, r.stderr)
    source = directory / "bounds.isa"
    source.write_text(
        '@import "std.vec" fn main(){let v=Vec<i32>::new();let s=v.as_slice();print(s[0]);}'
    )
    cmd = [compiler, str(source), "-o", str(directory / "bounds")]
    if extra:
        cmd += ["--sanitize"]
    r = subprocess.run(cmd, capture_output=True, text=True)
    assert r.returncode == 0, r.stderr
    r = subprocess.run(
        [str(directory / "bounds")],
        capture_output=True,
        text=True,
        env=env,
        preexec_fn=lambda: resource.setrlimit(resource.RLIMIT_CORE, (0, 0)),
    )
    assert r.returncode != 0 and "isac runtime panic:" in r.stderr, r.stderr
    assert "ERROR: AddressSanitizer" not in r.stderr and "runtime error:" not in r.stderr, r.stderr
for name in [
    "valid_vec_get_mut",
    "valid_box_get_mut",
    "valid_vec_slices",
    "valid_vec_mut_endpoints",
    "valid_unique_option_queries",
]:
    for stage in ["hir", "core", "regions", "erased", "lir", "mlir", "llvm"]:
        r = subprocess.run(
            [compiler, str(cases / (name + ".isa")), "--emit-" + stage],
            capture_output=True,
            text=True,
            timeout=30,
        )
        assert r.returncode == 0, (name, stage, r.stderr)
        if stage == "core":
            assert "borrow_unique" in r.stdout or "borrow.unique" in r.stdout, r.stdout
        if name == "valid_unique_option_queries" and stage in ["hir", "core"]:
            assert "payload.inspect" in r.stdout and "tag-only noescape" in r.stdout, r.stdout
        if stage in ["erased", "lir", "mlir", "llvm"]:
            for proof in ["LoanId", "RegionId", "quantity=", "container.call", "payload.inspect"]:
                assert proof not in r.stdout, (stage, proof)
            if name == "valid_vec_slices":
                assert "isac_rt_vec_data" in r.stdout, r.stdout
