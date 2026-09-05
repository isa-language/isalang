from pathlib import Path
import resource
import subprocess
import sys
import tempfile
import time

with tempfile.TemporaryDirectory() as directory:
    source = Path(directory) / "slots.isa"
    source.write_text(
        '@import "std.vec"\nfn main() { let values = Vec<i32>::new();\n'
        + "values.push(1);\n" * 512
        + "values.get(511); }\n"
    )
    start = time.monotonic()
    result = subprocess.run(
        [sys.argv[1], str(source), "--check"], capture_output=True, text=True, timeout=30
    )
    assert result.returncode == 0 and not result.stdout and not result.stderr, result
    peak = resource.getrusage(resource.RUSAGE_CHILDREN).ru_maxrss
    if sys.platform.startswith("linux"):
        assert peak < 192 * 1024, f"slot analysis retained excessive state: {peak} KiB"
    print(f"512 pushes: {time.monotonic() - start:.3f}s; peak child RSS={peak}")
