import pathlib, subprocess, sys, tempfile

compiler, root = sys.argv[1:]
root = pathlib.Path(root)
for source in sorted((root / "tests/diagnostics").glob("*.isa")):
    r = subprocess.run(
        [compiler, str(source), "--color=never"], capture_output=True, text=True, timeout=15
    )
    actual = r.stderr.replace(str(root) + "/", "")
    assert r.returncode != 0 and "\x1b" not in actual, source
    assert actual == source.with_suffix(".expected").read_text(), (source, actual)
source = root / "tests/diagnostics/ownership_use_after_move.isa"
r = subprocess.run(
    [compiler, str(source), "--color=always"], capture_output=True, text=True, timeout=15
)
assert "\x1b[" in r.stderr
with tempfile.TemporaryDirectory() as d:
    source = pathlib.Path(d) / "many.isa"
    source.write_text(
        "\n".join(f"fn f{i}() {{ print(missing); }}" for i in range(30))
        + "\nfn main()->i32{return 0;}"
    )
    r = subprocess.run(
        [compiler, str(source), "--color=never"], capture_output=True, text=True, timeout=15
    )
    assert (
        r.returncode != 0 and r.stderr.count("error[E0102]") == 20 and "error limit" in r.stderr
    ), r.stderr
