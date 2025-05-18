import subprocess, sys, tempfile
from pathlib import Path

compiler, root = sys.argv[1:3]
root = Path(root)
source = root / "examples/macros_demo.isa"


def emit(stage):
    r = subprocess.run([compiler, str(source), "--emit-" + stage], capture_output=True, text=True)
    assert r.returncode == 0, r.stderr
    return r.stdout


expanded = emit("expanded")
assert "expansion #" in expanded and "temp#" in expanded
assert "append_display" in expanded and "@ println" not in expanded
hir = emit("hir")
core = emit("core")
assert "effects={ IO Allocation }" in hir and "effects={ IO Allocation }" in core
assert "memory=Shared" in core and "memory=Unique" in core
for stage in ["erased", "lir", "mlir", "llvm"]:
    text = emit(stage)
    assert "MemoryState" not in text and "ProtocolState" not in text and "quantity=" not in text
    assert "isac_rt_string_" in text
assert "live_in={" in emit("liveness")
with tempfile.TemporaryDirectory() as d:
    path = Path(d) / "counter.isa"
    path.write_text('fn main()->i32 { let s=@format("{}",42); @println("{}",s); return 0; }')
    binary = Path(d) / "counter"
    for optimization in ["-O0", "-O2", "-O3", "-Os", "-Oz"]:
        built = subprocess.run(
            [compiler, str(path), optimization, "-o", str(binary), "--runtime-test-counters"],
            capture_output=True,
            text=True,
        )
        assert built.returncode == 0, built.stderr
        r = subprocess.run([str(binary)], capture_output=True, text=True)
        assert r.returncode == 0 and r.stdout == "42\n", (r.stdout, r.stderr)
        assert "allocations=2 frees=2" in r.stderr, r.stderr
    obj = Path(d) / "macro32.o"
    r = subprocess.run(
        [compiler, str(source), "--target=i686-unknown-linux-gnu", "--emit-obj", "-o", str(obj)],
        capture_output=True,
        text=True,
    )
    assert r.returncode == 0 and obj.read_bytes().startswith(b"\x7fELF"), r.stderr
