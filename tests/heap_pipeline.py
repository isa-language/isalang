from pathlib import Path
import re
import subprocess
import sys
import tempfile

compiler, root = sys.argv[1:3]
root = Path(root)


def emit(source, mode):
    p = subprocess.run([compiler, str(source), "--emit-" + mode], capture_output=True, text=True)
    assert p.returncode == 0, (source, mode, p.stderr)
    return p.stdout


box = root / "examples/heap/box.isa"
buffer = root / "examples/heap/buffer.isa"
assert "BoxI32" in emit(box, "ast")
hir = emit(box, "hir")
assert "DropGlue=FreeAllocation" in hir and "extern=C" in hir
core = emit(box, "core")
assert "heap.alloc" in core and "source=Dead" in core and "DropGlue(" in core
for source in (box, buffer):
    for mode in ("erased", "lir", "mlir", "std-mlir", "llvm-dialect", "llvm"):
        text = emit(source, mode)
        assert not re.search(
            r"\b(Own|Shared|Unique|Dead|Ready|Released|Quantity|RawPtr|BufferState)\b|protocol\.transition|resource\.phi",
            text,
        ), (source, mode)
        assert not re.search(r"\b(malloc|calloc|free)\s*\(", text), (mode, "direct libc dependency")
main_erased = re.split(r"fn isa_\d+_main\n", emit(box, "erased"), maxsplit=1)[1]
assert main_erased.count("heap.alloc") == 1 and main_erased.count("free ") == 1
assert "move" not in main_erased and "borrow" not in main_erased
main_llvm = re.split(r"define i32 @isa_\d+_main\(\)", emit(box, "llvm"), maxsplit=1)[1]
assert main_llvm.count("call ptr @isac_rt_alloc(") == 1
assert main_llvm.count("call void @isac_rt_free(") == 1
assert "isac_rt_print_i32" in main_llvm
buffer_llvm = emit(buffer, "llvm")
main_buffer = re.split(r"define i32 @isa_\d+_main\(\)", buffer_llvm, maxsplit=1)[1]
assert "call void @isac_rt_free(" not in main_buffer, "Released wrapper was freed twice"
assert "allocation=absent" in emit(buffer, "core")
assert '"isac.free"' in emit(box, "mlir")

with tempfile.TemporaryDirectory() as directory:
    for name in (
        "invalid_heap_use_after_move",
        "invalid_heap_double_drop",
        "invalid_buffer_after_release",
    ):
        source = root / "tests/heap" / (name + ".isa")
        r = subprocess.run([compiler, str(source), "--emit-mlir"], capture_output=True, text=True)
        assert r.returncode == 1 and not r.stdout, (name, r.stdout, r.stderr)
        output = Path(directory) / name
        r = subprocess.run(
            [compiler, str(source), "-o", str(output)], capture_output=True, text=True
        )
        assert r.returncode == 1 and not output.exists(), name
