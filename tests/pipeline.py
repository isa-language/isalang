import re
import subprocess
import sys
import tempfile
from pathlib import Path

compiler, root = sys.argv[1:3]
root = Path(root)
source = root / "examples/file.isa"


def emit(mode, path=source):
    r = subprocess.run([compiler, str(path), mode], capture_output=True, text=True)
    assert r.returncode == 0, (mode, r.stderr)
    return r.stdout


assert "protocol" in emit("--emit-tokens")
assert "AST Program" in emit("--emit-ast")
assert "impl File" in emit("--emit-ast")
hir = emit("--emit-hir")
assert "transition=#S1 -> #S1" in hir and "#O" in hir and "#v" in hir
core = emit("--emit-core")
assert "File<Own | Open> -> 1 File<Own | Closed>" in core
assert "witness:0" in core and "consume %" in core and "rebind #" in core
branch = emit("--emit-core", root / "tests/cases/valid_branch.isa")
assert "resource.phi" in branch
for mode in (
    "--emit-erased",
    "--emit-lir",
    "--emit-mlir",
    "--emit-std-mlir",
    "--emit-llvm-dialect",
    "--emit-llvm",
):
    text = emit(mode)
    assert not re.search(
        r"FileState|ProtocolState|MemoryState|Quantity|\bOwn\b|\bShared\b|\bUnique\b|protocol\.transition|resource\.phi",
        text,
    ), (mode, text)
    assert "File__close" in text, (mode, "runtime method effect was lost")
assert "branch" in emit("--emit-lir", root / "tests/cases/valid_branch.isa")
assert '"isac.call"' in emit("--emit-mlir")
standard = emit("--emit-std-mlir")
assert "memref.alloca" in standard and "isac." not in standard
assert "llvm.func" in emit("--emit-llvm-dialect")
assert "define i32 @main" in emit("--emit-llvm")

with tempfile.TemporaryDirectory() as d:
    exe = Path(d) / "must-not-exist"
    r = subprocess.run(
        [compiler, str(root / "tests/cases/invalid_protocol_transition.isa"), "-o", str(exe)],
        capture_output=True,
        text=True,
    )
    assert r.returncode == 1 and not exe.exists()
    for word in ("E0401", "resource:", "current state:", "Closed", "available in:", "Open"):
        assert word in r.stderr, r.stderr
    r = subprocess.run([compiler, str(source), "--invalid-option"], capture_output=True, text=True)
    assert r.returncode == 1 and "unknown option" in r.stderr
