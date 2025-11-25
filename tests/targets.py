import pathlib, subprocess, sys, tempfile, struct, re

compiler, root = sys.argv[1:]
root = pathlib.Path(root)


def run(*args):
    return subprocess.run([compiler, *map(str, args)], capture_output=True, text=True, timeout=40)


with tempfile.TemporaryDirectory() as d:
    d = pathlib.Path(d)
    source = d / "main.isa"
    source.write_text(
        "fn word(x:usize)->usize{return x;} fn main()->i32{print(word(42));return 0;}"
    )
    host = run("--print-target")
    assert host.returncode == 0, host.stderr
    for triple in [host.stdout.strip(), "x86_64-unknown-linux-gnu", "i686-unknown-linux-gnu"]:
        r = run(source, "--target=" + triple, "--emit-llvm")
        assert r.returncode == 0, r.stderr
        assert (
            'target triple = "' + triple + '"' in r.stdout and 'target datalayout = "' in r.stdout
        )
        width = "i32" if triple.startswith("i686") else "i64"
        assert re.search(r"@isa_\d+_word\(" + width + r"\b", r.stdout), r.stdout
        obj = d / (triple + ".o")
        r = run(source, "--target=" + triple, "--emit-obj", "-o", obj)
        assert r.returncode == 0, r.stderr
        data = obj.read_bytes()
        assert data[:4] == b"\x7fELF"
        assert struct.unpack_from("<H", data, 18)[0] == (3 if triple.startswith("i686") else 62)
    r = run(source, "--target=aarch64-unknown-linux-gnu", "--emit-obj", "-o", d / "arm.o")
    if r.returncode == 0:
        assert struct.unpack_from("<H", (d / "arm.o").read_bytes(), 18)[0] == 183
    else:
        assert "target unavailable" in r.stderr, r.stderr
        print("AArch64 backend unavailable: cross-object test skipped")
    r = run(source, "--cpu=native", "--emit-llvm")
    assert r.returncode == 0 and "target-cpu" in r.stdout, r.stderr
    r = run(source, "--cpu=x86-64-v3", "--features=+avx2,+fma", "--emit-llvm")
    assert r.returncode == 0 and "+avx2,+fma" in r.stdout, r.stderr
    r = run(source, "--cpu=not-a-real-cpu", "--check")
    assert r.returncode != 0 and "invalid CPU" in r.stderr, r.stderr
    r = run(source, "--target=i686-unknown-linux-gnu", "-o", d / "cross")
    assert r.returncode != 0 and "cross-linking" in r.stderr, r.stderr
    r = run(source, "--emit-asm", "-o", d / "main.s")
    assert r.returncode == 0 and "main" in (d / "main.s").read_text(), r.stderr
    source.write_text("fn main()->i32{let n=4294967296usize;return 0;}")
    r = run(source, "--target=i686-unknown-linux-gnu", "--check")
    assert r.returncode != 0 and "E0202" in r.stderr, r.stderr
