import subprocess
import sys
import tempfile
from pathlib import Path

compiler, source, expected = sys.argv[1:4]
with tempfile.TemporaryDirectory(prefix="isac-test-") as d:
    exe = Path(d) / "program with spaces"
    r = subprocess.run([compiler, source, "-o", str(exe)], capture_output=True, text=True)
    assert r.returncode == 0, (r.stdout, r.stderr)
    r = subprocess.run([str(exe)], capture_output=True, text=True)
    assert r.returncode == 0 and r.stdout == expected.replace("|", "\n"), (
        r.returncode,
        r.stdout,
        r.stderr,
    )
