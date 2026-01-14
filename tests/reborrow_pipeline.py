from pathlib import Path
import os
import subprocess
import sys
import tempfile

compiler, root = sys.argv[1:3]
root = Path(root)
flags = ["--sanitize"] if len(sys.argv) > 3 else []
env = {
    **os.environ,
    "ASAN_OPTIONS": "detect_leaks=1:halt_on_error=1",
    "UBSAN_OPTIONS": "halt_on_error=1",
}
cases = {
    "valid_direct": "10\n10\n14\n15\n",
    "valid_edges": "1\n2\n1\n3\n4\n5\n",
    "valid_heap": "a\nabc\n",
    "valid_carrier": "10\n20\n",
    "valid_protocol": "42\n7\n",
}
with tempfile.TemporaryDirectory() as directory:
    for name, expected in cases.items():
        source = root / "tests/reborrow" / (name + ".isa")
        for opt in ["-O0", "-O2"]:
            binary = Path(directory) / name
            built = subprocess.run(
                [compiler, str(source), opt, *flags, "--runtime-test-counters", "-o", str(binary)],
                capture_output=True,
                text=True,
            )
            assert built.returncode == 0, built.stderr
            run = subprocess.run([str(binary)], capture_output=True, text=True, env=env, timeout=15)
            assert (run.returncode, run.stdout) == (0, expected), run
            assert run.stderr == (
                "ISAC_RT allocations=1 frees=1\n"
                if name in {"valid_heap", "valid_protocol"}
                else ""
            ), run.stderr
    for source in sorted((root / "tests/reborrow").glob("invalid_*.isa")):
        run = subprocess.run(
            [compiler, str(source), "--check", "--color=never"], capture_output=True, text=True
        )
        code = "E0310" if source.stem == "invalid_protocol_transition" else "E0312"
        assert run.returncode == 1 and "error[" + code + "]" in run.stderr, run.stderr
        if source.stem in {
            "invalid_shared_live",
            "invalid_unique_parent_read",
            "invalid_loop_live",
        }:
            for label in [
                "derived borrow created here",
                "derived reference last used here",
                "incompatible parent access requested here",
                "= note:",
                "= help:",
            ]:
                assert label in run.stderr, run.stderr
        if source.stem == "invalid_loop_live":
            assert "live on a loop backedge" in run.stderr, run.stderr
        if source.stem == "invalid_owner_drop":
            assert "print(reader)" in run.stderr and "reference last used" in run.stderr, run.stderr
    source = root / "tests/reborrow/valid_direct.isa"
    for stage in ["core", "regions", "region-constraints", "erased", "lir", "mlir", "llvm"]:
        run = subprocess.run(
            [compiler, str(source), "--emit-" + stage], capture_output=True, text=True
        )
        assert run.returncode == 0, run.stderr
        if stage == "core":
            assert "[CFG last-use]" in run.stdout
        elif stage == "regions":
            assert "loan #L" in run.stdout and "Unique" in run.stdout and "Shared" in run.stdout
        elif stage == "region-constraints":
            assert run.stdout.count("reason=Reborrow") >= 4, run.stdout
        else:
            for proof in ["loan #L", "reason=Reborrow", "[CFG last-use]"]:
                assert proof not in run.stdout, run.stdout
