from pathlib import Path
import os, subprocess, sys, tempfile

compiler, root, *sanitized = sys.argv[1:]
compiler, root = str(Path(compiler).resolve()), Path(root).resolve()
with tempfile.TemporaryDirectory() as directory:
    base = Path(directory)
    (base / "helpers").mkdir()

    def write(name, source):
        path = base / name
        path.write_text(source)
        return path

    def invoke(path, *options):
        return subprocess.run(
            [compiler, str(path), *options], cwd="/", capture_output=True, text=True, timeout=30
        )

    def check(name, source, code=None, contains=None):
        path = write(name, source)
        result = invoke(path, "--check", "--color=never")
        assert (result.returncode == 0) == (code is None), (name, result)
        if code:
            assert f"[{code}]" in result.stderr, (name, result.stderr)
        if contains:
            assert contains in result.stderr, (name, result.stderr)
        return path

    write(
        "helpers/math.isa",
        "fn twice(x:i32)->i32{return x*2;} @macro doubled { ($x:expr) => { twice($x) } }",
    )
    write("helpers/transitive.isa", '@import "math.isa" fn answer()->i32{return @doubled(21);}')
    path = check(
        "main.isa",
        '@import("helpers/transitive.isa", "helpers/math.isa", '
        '"helpers/../helpers/math.isa",) fn main(){@println("{}",answer());}',
    )
    graph = invoke(path, "--emit-imports")
    assert graph.returncode == 0, graph.stderr

    assert graph.stdout.count(str(base / "helpers/math.isa")) == 1, graph.stdout
    (base / "math-link.isa").symlink_to(base / "helpers/math.isa")
    check("dedup.isa", '@import("math-link.isa","helpers/math.isa") fn main(){print(twice(1));}')
    expanded = invoke(path, "--emit-expanded")
    assert (
        expanded.returncode == 0
        and "expansion #" in expanded.stdout
        and "twice#" in expanded.stdout
    ), expanded

    write("hygiene.isa", "@macro local { ($x:expr) => { {let temp=$x; temp} } }")
    hygienic = check(
        "hygienic.isa",
        '@import "hygiene.isa" fn main(){let temp=100; '
        'let value=@local(20); @println("{} {}",temp,value);}',
    )
    write("consume.isa", "@macro consume { ($x:expr) => { $x.move() } }")
    check(
        "ownership.isa",
        '@import "consume.isa" fn main(){let a=String::from("a"); let b=@consume(a); print(a);}',
        "E0301",
    )

    generated = check(
        "generated.isa",
        '@macro load { () => { @import("helpers/math.isa") } } '
        "@load() fn main(){print(@doubled(21));}",
    )
    write("helpers/generated.isa", '@macro load_next { () => { @import("helpers/math.isa") } }')
    check(
        "imported-generated.isa",
        '@import "helpers/generated.isa" @load_next() fn main(){print(@doubled(1));}',
    )

    write("cycle-a.isa", '@import "cycle-b.isa"')
    write("cycle-b.isa", '@import "cycle-a.isa"')
    check("cycle-main.isa", '@import "cycle-a.isa" fn main(){}', "E0111", "cycle-b.isa")
    write(
        "generated-cycle.isa",
        '@macro load_cycle { () => { @import("generated-cycle.isa") } } @load_cycle()',
    )
    check("generated-cycle-main.isa", '@import "generated-cycle.isa" fn main(){}', "E0111")
    for name, statement in [
        ("dynamic", "@import(path)"),
        ("empty", "@import()"),
        ("number", "@import(12)"),
        ("separator", '@import("std.box" "std.vec")'),
        ("bundled-path", '@import("std../box")'),
        ("extension", '@import("helpers/math")'),
    ]:
        check(name + ".isa", statement + " fn main(){}", "E0110")
    check("nested.isa", 'fn main(){@import("std.box");}', "E0110", "module-scope")
    check(
        "nested-generated.isa",
        '@macro load { () => { @import("std.box") } } fn main(){@load();}',
        "E0110",
        "module-scope",
    )
    check("missing.isa", '@import("absent.isa") fn main(){}', "E0112", "missing.isa:1:1")
    check("reserved.isa", "@macro import { () => {} } fn main(){}", "E0110")

    for name, source in [
        ("ffi", 'extern "C" fn injected()->i32;'),
        ("intrinsic", '@intrinsic "box.generic.new" fn injected(x:i32)->i32;'),
        ("representation", '@representation "generic_box" struct Injected<T>{value:T,}'),
    ]:
        write(name + ".isa", source)
        result = invoke(
            write("untrusted-" + name + ".isa", '@import("' + name + '.isa") fn main(){}'),
            "--check",
        )
        assert result.returncode != 0, (name, result)
        assert str(base / (name + ".isa")) in result.stderr, result.stderr
    check(
        "mixed-trust.isa",
        '@import("std.box", "' + str(root / "lib/std/box.isa") + '") fn main(){}',
        "E0110",
        "trusted",
    )

    env = {
        **os.environ,
        "ASAN_OPTIONS": "detect_leaks=1:halt_on_error=1:detect_stack_use_after_return=1",
        "UBSAN_OPTIONS": "halt_on_error=1",
    }
    for source, output in [(path, "42\n"), (hygienic, "100 20\n"), (generated, "42\n")]:
        for optimization in ["-O0", "-O2"]:
            executable = base / "program"
            options = [optimization, "-o", str(executable)]
            if sanitized:
                options += ["--sanitize"]
            built = invoke(source, *options)
            assert built.returncode == 0, built.stderr
            run = subprocess.run(
                [str(executable)], capture_output=True, text=True, env=env, timeout=15
            )
            assert (run.returncode, run.stdout, run.stderr) == (0, output, ""), run
