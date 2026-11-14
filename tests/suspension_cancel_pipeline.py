from pathlib import Path
import os
import re
import subprocess
import sys
import tempfile

driver, *sanitized = sys.argv[1:]
env = dict(
    os.environ,
    ASAN_OPTIONS="detect_leaks=1:halt_on_error=1:detect_stack_use_after_return=1",
    UBSAN_OPTIONS="halt_on_error=1",
)
expected = ["", "", "7\n", "7\n7\nowned12\n42\n"]
with tempfile.TemporaryDirectory() as folder:
    binary = Path(folder) / "program"
    for opt in ["O0", "O2"]:
        for phase, output in enumerate(expected):
            built = subprocess.run(
                [driver, str(phase), str(binary), opt, "sanitized" if sanitized else "normal"],
                capture_output=True,
                text=True,
                timeout=90,
            )
            assert built.returncode == 0, (phase, built.stderr)
            done = subprocess.run([binary], capture_output=True, text=True, env=env, timeout=15)
            assert done.returncode == 0 and done.stdout == output, (phase, done)
            balance = re.fullmatch(r"ISAC_RT allocations=(\d+) frees=(\d+)\n", done.stderr)
            assert balance and balance[1] == balance[2], (phase, done)
print("8 cancellation runs passed, including both movable continuation states")
