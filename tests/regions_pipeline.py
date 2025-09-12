from pathlib import Path
import os
import subprocess
import sys
import tempfile

compiler, root = sys.argv[1:3]
root = Path(root)
flags = ["--sanitize"] if len(sys.argv) > 3 else []
environment = {
    **os.environ,
    "ASAN_OPTIONS": "detect_leaks=1:halt_on_error=1",
    "UBSAN_OPTIONS": "halt_on_error=1",
}
with tempfile.TemporaryDirectory() as directory:
    for fixture, expected in [
        ("valid_cfg_branch_last_use", "10\n20\n10\n30\n"),
        ("valid_cfg_shadowing", "1\n9\n"),
        ("valid_cfg_loop_last_use", "0\n1\n2\n2\n"),
    ]:
        source = root / "tests/references" / (fixture + ".isa")
        for opt in ["-O0", "-O2"]:
            binary = Path(directory) / "program"
            built = subprocess.run(
                [compiler, str(source), opt, *flags, "-o", str(binary)],
                capture_output=True,
                text=True,
            )
            assert built.returncode == 0, built.stderr
            result = subprocess.run([str(binary)], capture_output=True, text=True, env=environment)
            assert (result.returncode, result.stdout, result.stderr) == (0, expected, ""), result
    source = root / "tests/references/valid_cfg_branch_last_use.isa"
    for stage in ["regions", "core", "erased", "lir", "mlir", "llvm"]:
        result = subprocess.run(
            [compiler, str(source), "--emit-" + stage], capture_output=True, text=True
        )
        assert result.returncode == 0, result.stderr
        if stage in ["regions", "core"]:
            assert "loan #L" in result.stdout and "liveIn={" in result.stdout
        else:
            assert "loan #L" not in result.stdout and "liveIn={" not in result.stdout
        if stage == "core":
            assert "[CFG last-use]" in result.stdout
