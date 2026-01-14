from pathlib import Path
import json, os, re, subprocess, sys, tempfile

compiler, root, *extra = sys.argv[1:]
cases = Path(root) / "tests/exclusive_results"
expected = json.loads((cases / "expectations.json").read_text())
assert {p.stem for p in cases.glob("*.isa")} == set(expected)
env = {
    **os.environ,
    "ASAN_OPTIONS": "detect_leaks=1:halt_on_error=1:detect_stack_use_after_return=1",
    "UBSAN_OPTIONS": "halt_on_error=1",
}
for name, expectation in expected.items():
    if not name.startswith("invalid_"):
        continue
    r = subprocess.run(
        [compiler, str(cases / (name + ".isa")), "--check"],
        capture_output=True,
        text=True,
        timeout=30,
    )
    assert r.returncode != 0 and "error[E9001]" not in r.stderr, (name, r.stderr)
    assert expectation["error"] in r.stderr, (name, r.stderr)
    if name == "invalid_return_temporary":
        assert "temporary owner created here" in r.stderr and "$temporary" not in r.stderr, r.stderr

with tempfile.TemporaryDirectory() as directory:
    program = str(Path(directory) / "program")
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
                program,
            ]
            if extra:
                cmd += ["--sanitize"]
            r = subprocess.run(cmd, capture_output=True, text=True, timeout=90)
            assert r.returncode == 0, (name, optimization, r.stderr)
            r = subprocess.run([program], capture_output=True, text=True, env=env, timeout=20)
            assert r.returncode == 0 and r.stdout == expectation["stdout"], (name, optimization, r)
            if expectation.get("heap"):
                counts = re.fullmatch(r"ISAC_RT allocations=(\d+) frees=(\d+)\n", r.stderr)
                assert counts and counts[1] == counts[2] and int(counts[1]) > 0, (name, r.stderr)
            else:
                assert r.stderr == "", (name, r.stderr)

for name in ["valid_slice_result", "valid_generic_method_result"]:
    for stage in ["hir", "core", "regions", "region-constraints", "erased", "lir", "mlir", "llvm"]:
        r = subprocess.run(
            [compiler, str(cases / (name + ".isa")), "--emit-" + stage],
            capture_output=True,
            text=True,
            timeout=30,
        )
        assert r.returncode == 0, (name, stage, r.stderr)
        if stage in ["hir", "core"]:
            assert "return.region" in r.stdout and "Unique borrowed container" in r.stdout, r.stdout
        if stage == "regions":
            assert "keeps input alive" in r.stdout, r.stdout
        if stage == "region-constraints":
            assert "reason=FunctionResult" in r.stdout and " <= " in r.stdout, r.stdout
        if stage in ["erased", "lir", "mlir", "llvm"]:
            for proof in [
                "LoanId",
                "RegionId",
                "quantity=",
                "return.region",
                "payload.inspect",
                "keeps input alive",
            ]:
                assert proof not in r.stdout, (stage, proof)
