import re, subprocess, sys
from pathlib import Path

compiler, path = sys.argv[1:]
source = Path(path).read_text()
r = subprocess.run([compiler, path, "--check", "--color=never"], capture_output=True, text=True)
want = re.search(r"^// error: (E\d+)", source)
assert (
    (r.returncode == 1 and "error[" + want[1] + "]" in r.stderr) if want else r.returncode == 0
), (path, r.returncode, r.stderr)
