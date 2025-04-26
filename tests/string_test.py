import json, pathlib, subprocess, sys, tempfile

compiler, source = sys.argv[1:3]
source = pathlib.Path(source)
expected = json.loads((source.parent / "expectations.json").read_text())[source.stem]


def run(args):
    return subprocess.run(args, capture_output=True, text=True, timeout=30)


r = run([compiler, str(source), "--check", "--color=never"])
if expected["error"]:
    assert r.returncode != 0 and expected["error"] in r.stderr, (source, r.stderr)
    assert " --> " in r.stderr and "^" in r.stderr, (source, r.stderr)
else:
    assert r.returncode == 0, (source, r.stderr)
    with tempfile.TemporaryDirectory() as d:
        exe = str(pathlib.Path(d) / "program")
        flags = ["--sanitize"] if len(sys.argv) > 3 else []
        r = run([compiler, str(source), "-o", exe, "--runtime-test-counters", *flags])
        assert r.returncode == 0, (source, r.stderr)
        r = run([exe])
        assert r.returncode == 0 and r.stdout == expected["stdout"], (
            source,
            r.stdout,
            r.stderr,
            expected,
        )
        n = expected["allocations"]
        if n:
            assert f"ISAC_RT allocations={n} frees={n}" in r.stderr, (source, r.stderr)
        else:
            assert not r.stderr or "allocations=0 frees=0" in r.stderr, r.stderr
