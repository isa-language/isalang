import json, os, subprocess, sys, tempfile
from pathlib import Path

compiler, source = sys.argv[1:3]
source = Path(source)
expected = json.loads(source.with_name("expectations.json").read_text())[source.stem]
sanitized = len(sys.argv) > 3
checked = subprocess.run(
    [compiler, str(source), "--check", "--color=never"], capture_output=True, text=True
)
if expected["code"]:
    assert checked.returncode == 1 and "error[" + expected["code"] + "]" in checked.stderr, (
        checked.stderr
    )
    if source.stem in {
        "invalid_macro_ownership",
        "invalid_macro_expansion_syntax",
        "invalid_macro_protocol",
    }:
        assert "in expansion of @" in checked.stderr, checked.stderr
else:
    assert checked.returncode == 0, checked.stderr
    with tempfile.TemporaryDirectory() as directory:
        executable = Path(directory) / "program"
        command = [compiler, str(source), "-o", str(executable)]
        if sanitized:
            command += ["--sanitize"]
        built = subprocess.run(command, capture_output=True, text=True)
        assert built.returncode == 0, built.stderr
        run = subprocess.run(
            [str(executable)],
            input=bytes.fromhex(expected["stdin_hex"])
            if "stdin_hex" in expected
            else expected["stdin"].encode(),
            capture_output=True,
            timeout=15,
            env={
                **os.environ,
                "ASAN_OPTIONS": "detect_leaks=1:halt_on_error=1",
                "UBSAN_OPTIONS": "halt_on_error=1",
            },
        )
        assert run.returncode == 0, (run.returncode, run.stdout, run.stderr)
        assert run.stdout.decode() == expected["stdout"], (run.stdout[:4096], expected["stdout"])
        assert not run.stderr, run.stderr
