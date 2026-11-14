import subprocess
import resource
import sys

binary = sys.argv[1]
resource.setrlimit(resource.RLIMIT_CORE, (0, 0))
done = subprocess.run([binary], capture_output=True, text=True, timeout=30)
assert done.returncode == 0, done
for mode, reason in [
    ("1", "thread creation failed"),
    ("2", "thread join failed"),
    ("3", "null scalar thread worker"),
]:
    done = subprocess.run([binary, mode], capture_output=True, text=True, timeout=30)
    assert done.returncode != 0 and reason in done.stderr, done
for mode, reason in [
    ("4", "thread creation failed"),
    ("5", "thread join failed"),
    ("6", "null scoped thread callback/context"),
    ("7", "null scoped thread callback/context"),
]:
    done = subprocess.run([binary, mode], capture_output=True, text=True, timeout=30)
    assert done.returncode != 0 and reason in done.stderr, done
