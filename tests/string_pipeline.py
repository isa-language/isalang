import pathlib, subprocess, sys, tempfile

compiler, root = sys.argv[1:]
root = pathlib.Path(root)
source = root / "tests/strings/valid_string_move.isa"


def emit(stage, *flags):
    r = subprocess.run(
        [compiler, str(source), "--emit-" + stage, *flags],
        capture_output=True,
        text=True,
        timeout=30,
    )
    assert r.returncode == 0, (stage, r.stderr)
    return r.stdout


for stage in ["ast", "hir", "core", "erased", "lir", "mlir", "llvm"]:
    text = emit(stage)
    if stage == "core":
        assert "move" in text and "source=Dead" in text and "static_str" in text, text
    if stage in ["erased", "lir", "mlir", "llvm"]:
        assert "source=Dead" not in text and "borrow.shared" not in text and "quantity=" not in text
llvm = emit("llvm")
main = llvm.split("define i32 @main()", 1)[1]
assert main.count("call void @isac_rt_string_from") == 1
assert main.count("call void @isac_rt_string_drop") == 1
assert 'private constant [7 x i8] c"Isalang"' in llvm
assert "malloc" not in main and "memcpy" not in main
for level in ["-O0", "-O2", "-O3", "-Os", "-Oz"]:
    with tempfile.TemporaryDirectory() as d:
        exe = str(pathlib.Path(d) / "demo")
        r = subprocess.run(
            [compiler, str(root / "examples/string_demo.isa"), level, "-o", exe],
            capture_output=True,
            text=True,
            timeout=30,
        )
        assert r.returncode == 0, r.stderr
        r = subprocess.run([exe], capture_output=True, text=True, timeout=15)
        assert r.returncode == 0 and r.stdout == "Olá, Isalang\nOlá, Isalang\nOlá, Isalang!\n", (
            level,
            r.stdout,
            r.stderr,
        )
