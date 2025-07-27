from pathlib import Path
import subprocess
import sys
import tempfile

compiler, root = sys.argv[1:]
source = Path(root) / "tests/containers/valid_generic_instances.isa"


def emit(kind):
    result = subprocess.run(
        [compiler, str(source), "--emit-" + kind], capture_output=True, text=True
    )
    assert result.returncode == 0, result.stderr
    return result.stdout


assert "EnumDecl Option<T>" in emit("ast")
hir = emit("hir")
instances = [line for line in hir.splitlines() if line.startswith("instance #I")]
assert len(instances) == 2, instances
assert all("#TP" in line and "type=#T" in line for line in instances), instances
assert "generic enum #G" in hir
for kind in ["core", "erased", "lir", "mlir", "llvm"]:
    assert emit(kind).strip(), kind

with tempfile.TemporaryDirectory() as directory:
    for opt in ["-O0", "-O2"]:
        executable = Path(directory) / ("generic" + opt)
        result = subprocess.run(
            [compiler, str(source), opt, "-o", str(executable)], capture_output=True, text=True
        )
        assert result.returncode == 0, result.stderr
        result = subprocess.run([str(executable)], capture_output=True, text=True)
        assert (result.returncode, result.stdout, result.stderr) == (0, "1\n", ""), result


source = Path(root) / "tests/containers/valid_main_unit.isa"
llvm = emit("llvm")
assert "define i32 @main(i32" in llvm and "ptr" in llvm
assert "define void @isa_" in llvm and "_main()" in llvm

source = Path(root) / "tests/containers/valid_linear_nested_payload.isa"
assert "payload.take" in emit("core")
assert "payload.take" not in emit("erased")
for kind in ["ast", "hir", "core", "erased", "lir", "mlir", "llvm"]:
    assert emit(kind).strip()
with tempfile.TemporaryDirectory() as directory:
    for opt in ["-O0", "-O2"]:
        executable = Path(directory) / ("payload" + opt)
        result = subprocess.run(
            [compiler, str(source), opt, "-o", str(executable)], capture_output=True, text=True
        )
        assert result.returncode == 0, result.stderr
        result = subprocess.run([str(executable)], capture_output=True, text=True)
        assert (result.returncode, result.stdout, result.stderr) == (0, "hello\n", ""), result
