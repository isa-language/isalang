import argparse
from pathlib import Path
import subprocess

parser = argparse.ArgumentParser()
parser.add_argument("--compiler", default="build/isac")
parser.add_argument("--source", default="examples/heap/box.isa")
parser.add_argument("--out", default="docs/dumps/heap_move")
args = parser.parse_args()
out = Path(args.out)
out.mkdir(parents=True, exist_ok=True)
source = Path(args.source)
(out / "source.isa").write_text(source.read_text())
for stage, suffix in [
    ("ast", "ast"),
    ("hir", "hir"),
    ("core", "core"),
    ("erased", "erased"),
    ("lir", "lir"),
    ("mlir", "mlir"),
    ("std-mlir", "std.mlir"),
    ("llvm-dialect", "llvm.mlir"),
    ("llvm", "ll"),
]:
    result = subprocess.run(
        [args.compiler, str(source), "--emit-" + stage], capture_output=True, text=True, check=True
    )
    (out / ("program." + suffix)).write_text(result.stdout)
