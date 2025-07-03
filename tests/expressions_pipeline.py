import subprocess, sys, tempfile
from pathlib import Path

compiler, root = sys.argv[1:3]
root = Path(root)


def run(args):
    result = subprocess.run([compiler, *map(str, args)], capture_output=True, text=True)
    assert result.returncode == 0, result.stderr
    return result.stdout


with tempfile.TemporaryDirectory() as directory:
    tmp = Path(directory)
    source = tmp / "temporary.isa"
    source.write_text(
        'fn main()->i32 { print(String::from("42").as_str().parse_i32().value());return 0;}'
    )
    core = run([source, "--emit-core"])
    assert "$temporary" in core and "scope.end" in core
    for emit in ("ast", "hir", "core", "erased", "lir", "mlir", "llvm"):
        assert run([source, "--emit-" + emit])
    for optimization in ("-O0", "-O1", "-O2", "-O3", "-Os", "-Oz"):
        executable = tmp / "program"
        run([source, optimization, "-o", executable, "--runtime-test-counters"])
        result = subprocess.run([str(executable)], capture_output=True, text=True, timeout=10)
        assert result.returncode == 0 and result.stdout == "42\n", result
        assert "allocations=2 frees=2" in result.stderr, result.stderr
    source = root / "tests/expressions/valid_match_enum_payload_expression.isa"
    assert "refine.variant" in run([source, "--emit-core"])
    assert "refine.variant" not in run([source, "--emit-erased"])
    source = tmp / "size.isa"
    source.write_text(
        "const MAX:usize=4294967295usize;const WRAP:usize=MAX+1usize;fn main()->i32{print(WRAP);return 0;}"
    )
    for triple, expected in [
        ("i686-unknown-linux-gnu", "0"),
        ("x86_64-unknown-linux-gnu", "4294967296"),
    ]:
        hir = run([source, "--emit-hir", "--target=" + triple])
        line = next(line for line in hir.splitlines() if " WRAP " in line)
        assert line.endswith(" " + expected), line
