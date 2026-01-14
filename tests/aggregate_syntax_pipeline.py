from pathlib import Path
import json, os, re, resource, subprocess, sys, tempfile

compiler, root, *extra = sys.argv[1:]
cases = Path(root) / "tests/aggregate_syntax"
expected = json.loads((cases / "expectations.json").read_text())
assert {p.stem for p in cases.glob("*.isa")} == set(expected)
env = {
    **os.environ,
    "ASAN_OPTIONS": "detect_leaks=1:halt_on_error=1:detect_stack_use_after_return=1",
    "UBSAN_OPTIONS": "halt_on_error=1",
}
for name, expectation in expected.items():
    if not expectation.get("invalid"):
        continue
    r = subprocess.run(
        [compiler, str(cases / (name + ".isa")), "--check"],
        capture_output=True,
        text=True,
        timeout=30,
    )
    assert r.returncode != 0 and "error[E9001]" not in r.stderr, (name, r.stderr)
    assert expectation["error"] in r.stderr, (name, r.stderr)

with tempfile.TemporaryDirectory() as directory:
    program = str(Path(directory) / "program")
    for name, expectation in expected.items():
        if expectation.get("invalid"):
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
            r = subprocess.run(
                [program],
                capture_output=True,
                text=True,
                env=env,
                timeout=20,
                preexec_fn=lambda: resource.setrlimit(resource.RLIMIT_CORE, (0, 0)),
            )
            if expectation.get("panic"):
                assert r.returncode != 0 and expectation["panic"] in r.stderr, (name, r)
                assert (
                    "ERROR: AddressSanitizer" not in r.stderr and "runtime error:" not in r.stderr
                ), r.stderr
                continue
            assert r.returncode == 0 and r.stdout == expectation["stdout"], (name, optimization, r)
            if expectation.get("heap"):
                counts = re.fullmatch(r"ISAC_RT allocations=(\d+) frees=(\d+)\n", r.stderr)
                assert counts and counts[1] == counts[2] and int(counts[1]) > 0, (name, r.stderr)
            else:
                assert r.stderr == "", (name, r.stderr)

for name in [
    "valid_structs",
    "valid_iteration",
    "valid_matrices",
    "valid_string_conversions",
    "valid_classic_for",
    "valid_recursive_iteration",
    "valid_iteration_views",
    "valid_iteration_protocol",
]:
    for stage in ["ast", "hir", "core", "regions", "erased", "lir", "mlir", "llvm"]:
        r = subprocess.run(
            [compiler, str(cases / (name + ".isa")), "--emit-" + stage],
            capture_output=True,
            text=True,
            timeout=30,
        )
        assert r.returncode == 0, (name, stage, r.stderr)
        if stage == "hir" and name == "valid_structs":
            assert "struct.init" in r.stdout and "field=#" in r.stdout, r.stdout
        if stage == "hir" and name == "valid_classic_for":
            assert "latch" in r.stdout and "$postfix" in r.stdout, r.stdout
        if stage == "hir" and name == "valid_iteration_views":
            assert "view-for rank=2" in r.stdout and "borrow<T" in r.stdout, r.stdout
        if stage == "core" and name == "valid_iteration":
            assert "borrow" in r.stdout and "loop" in r.stdout, r.stdout
        if stage == "core" and name == "valid_string_conversions":
            assert "string.from" in r.stdout, r.stdout
        if stage in ["erased", "lir", "mlir", "llvm"]:
            for proof in [
                "LoanId",
                "RegionId",
                "quantity=",
                "return.region",
                "struct.init",
                "view-for",
                "@iteration",
            ]:
                assert proof not in r.stdout, (stage, proof)
