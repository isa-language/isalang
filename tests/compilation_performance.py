import subprocess
import sys
import tempfile
from pathlib import Path

compiler = sys.argv[1]
with tempfile.TemporaryDirectory() as directory:
    source = Path(directory) / "large.isa"
    source.write_text(
        "fn main() { let values: u8[12000] = ["
        + ",".join(["0"] * 12000)
        + ']; @println("{}", values[11999]); }'
    )
    result = subprocess.run(
        [compiler, str(source), "--check", "--time-passes"],
        capture_output=True,
        text=True,
        timeout=30,
    )
    assert result.returncode == 0 and not result.stdout, result
    for phase in [
        "lexer",
        "imports/macros/parser",
        "HIR",
        "Core elaboration",
        "types/regions/resources",
    ]:
        assert "time: " + phase + " " in result.stderr, result.stderr

    source.write_text("fn main() { let values = [42; 12000]; print(values[11999]); }")
    binary = Path(directory) / "program"
    for opt in ["-O0", "-O2"]:
        result = subprocess.run(
            [compiler, str(source), opt, "--time-passes", "-o", str(binary)],
            capture_output=True,
            text=True,
            timeout=30,
        )
        assert result.returncode == 0 and "time: emission/native link " in result.stderr, result
        result = subprocess.run([str(binary)], capture_output=True, text=True, timeout=10)
        assert (result.returncode, result.stdout, result.stderr) == (0, "42\n", ""), result
