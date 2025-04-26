import pathlib, subprocess, sys, tempfile

compiler, root = sys.argv[1:]
root = pathlib.Path(root)


def run(args):
    return subprocess.run(args, capture_output=True, text=True, timeout=40)


with tempfile.TemporaryDirectory() as d:
    d = pathlib.Path(d)
    for source in [
        "examples/control-flow/commands.isa",
        "examples/control-flow/heap-loops.isa",
        "tests/stage3/valid_primitives.isa",
    ]:
        expected = None
        for level in ["-O0", "-O1", "-O2", "-O3", "-Os", "-Oz"]:
            exe = d / "program"
            r = run([compiler, str(root / source), level, "-o", str(exe)])
            assert r.returncode == 0, (level, r.stderr)
            r = run([str(exe)])
            assert r.returncode == 0, (level, r.stderr)
            if expected is None:
                expected = r.stdout
            assert r.stdout == expected, (source, level, r.stdout, expected)
    source = root / "examples/control-flow/optimization.isa"
    ir = []
    for level in ["-O0", "-O2"]:
        r = run([compiler, str(source), level, "--emit-llvm"])
        assert r.returncode == 0, r.stderr
        ir.append(r.stdout)
    assert "alloca" in ir[0] and "alloca" not in ir[1]
    assert len(ir[1]) < len(ir[0]), "optimization did not simplify example"
