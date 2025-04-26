import os
from pathlib import Path
import resource
import subprocess
import sys
import tempfile

compiler = sys.argv[1]


def disable_core():
    resource.setrlimit(resource.RLIMIT_CORE, (0, 0))


cases = [
    ("let b=Buffer::new(2); print(b.read(2)); b.release();", "buffer index out of bounds"),
    ("let b=Buffer::new(2); b.write(2,1); b.release();", "buffer index out of bounds"),
    (
        "let b=Buffer::new(18446744073709551615usize); b.release();",
        "buffer allocation size overflow",
    ),
]
with tempfile.TemporaryDirectory() as directory:
    source = Path(directory) / "panic.isa"
    output = Path(directory) / "panic"
    for body, expected in cases:
        source.write_text('@import "std.buffer"\nfn main() -> i32 { ' + body + " return 0; }")
        r = subprocess.run(
            [compiler, str(source), "-o", str(output)], capture_output=True, text=True
        )
        assert r.returncode == 0, r.stderr
        r = subprocess.run([str(output)], preexec_fn=disable_core, capture_output=True, text=True)
        assert r.returncode != 0 and "isac runtime panic: " + expected in r.stderr, (
            r.returncode,
            r.stderr,
        )

if len(sys.argv) > 2:
    r = subprocess.run(
        [sys.argv[2], "oom"], preexec_fn=disable_core, capture_output=True, text=True
    )
    assert r.returncode != 0 and "isac runtime panic: allocation failed" in r.stderr, (
        r.returncode,
        r.stderr,
    )
