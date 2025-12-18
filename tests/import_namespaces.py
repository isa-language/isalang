from pathlib import Path
import os, subprocess, sys, tempfile

compiler, root, *sanitized = sys.argv[1:]
env = {
    **os.environ,
    "ASAN_OPTIONS": "detect_leaks=1:halt_on_error=1:detect_stack_use_after_return=1",
    "UBSAN_OPTIONS": "halt_on_error=1",
}
with tempfile.TemporaryDirectory(prefix="isac modules ") as directory:
    base = Path(directory)

    def write(name, text):
        path = base / name
        path.write_text(text)
        return path

    def run(path, *args, code=0, contains=None):
        r = subprocess.run([compiler, str(path), *args], capture_output=True, text=True, env=env)
        assert r.returncode == code, (path, r.returncode, r.stderr)
        if contains:
            assert contains in r.stderr, (path, r.stderr)
        return r.stdout

    for name, value in [("left", 10), ("right", 20)]:
        write(
            name + ".isa",
            f"""const OFFSET:i32={value};
struct Data{{value:i32,}} struct Holder<T>{{value:T,}}
enum Choice<T>{{Some(T),None,}}
impl Data{{fn get(shared self)->i32{{return self.value;}}}}
impl<T> Holder<T>{{fn set(unique self,value:T){{self.value=value.move();}}}}
fn answer()->i32{{let d=Data::new();d.value=OFFSET;return d.get();}}
@macro answer{{()=>{{answer()}}}}
@macro nested{{()=>{{@answer()}}}}
@macro identity{{($x:expr)=>{{$x}}}}
""",
        )
    source = write(
        "main.isa",
        """@import("left.isa" as left,"right.isa" as right)
fn main(){
 let h=left::Holder<i32>::new();h.set(7);
 let c:left::Choice<i32> =left::Choice::Some(3);
 let number=match c {left::Choice::Some(v)=>v,left::Choice::None=>0,};
 let answer=42;
 @println("{} {} {} {} {} {} {} {}",left::answer(),right::answer(),@left::answer(),@right::nested(),
 @left::identity(right::answer()),left::OFFSET+right::OFFSET,number,h.value);
}""",
    )
    run(source, "--check")
    for level in ["-O0", "-O2"]:
        exe = base / "program"
        args = [level, "-o", str(exe)] + (["--sanitize"] if sanitized else [])
        run(source, *args)
        r = subprocess.run([str(exe)], capture_output=True, text=True, env=env)
        assert (r.returncode, r.stdout, r.stderr) == (0, "10 20 10 20 20 30 3 7\n", ""), r
    graph = run(source, "--emit-imports")
    assert "as left qualified" in graph and "as right qualified" in graph, graph
    run(
        write("ambiguous.isa", '@import("left.isa","right.isa") fn main(){answer();}'),
        "--check",
        "--color=never",
        code=1,
        contains="[E0113]",
    )
    run(
        write("ambiguous_macro.isa", '@import("left.isa","right.isa") fn main(){@answer();}'),
        "--check",
        "--color=never",
        code=1,
        contains="[E0113]",
    )
    run(
        write("qualified_only.isa", '@import "left.isa" as left fn main(){answer();}'),
        "--check",
        code=1,
        contains="unknown function",
    )
    run(
        write("missing.isa", '@import "left.isa" as left fn main(){left::absent();}'),
        "--check",
        code=1,
        contains="has no item",
    )
    run(
        write(
            "shadow.isa",
            '@import("left.isa","right.isa") fn main(){let OFFSET=1;@println("{}",OFFSET);}',
        ),
        "--check",
    )

    write("foreign.isa", 'extern "C" fn abs(x:i32)->i32;')
    foreign = write(
        "foreign-main.isa",
        '@import "foreign.isa" as c fn main(){unsafe{@println("{}",c::abs(-42));}}',
    )
    run(foreign, "--check")
    bridge = run(foreign, "--emit-c-bridge")
    assert " abs(" in bridge and "module$1$abs(" not in bridge, bridge
    exe = base / "foreign"
    run(foreign, "-o", str(exe), *(["--sanitize"] if sanitized else []))
    r = subprocess.run([str(exe)], capture_output=True, text=True, env=env)
    assert (r.returncode, r.stdout, r.stderr) == (0, "42\n", ""), r
