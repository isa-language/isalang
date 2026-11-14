from pathlib import Path
import os
import re
import subprocess
import sys
import tempfile

compiler, root, *sanitized = sys.argv[1:]
prefix = '@import "std.async"\n@import "std.vec"\nasync fn leaf()->i32{return 7;}\n'
env = dict(
    os.environ,
    ASAN_OPTIONS="detect_leaks=1:halt_on_error=1:detect_stack_use_after_return=1",
    UBSAN_OPTIONS="halt_on_error=1",
)
cases = {
    "shared": (
        'async fn main(){let text=String::from("hello");let r=&text;let n=await leaf();print(r);print(n);text.push_str("!");print(text);}',
        "hello\n7\nhello!\n",
    ),
    "unique": (
        'async fn main(){let text=String::from("hello");let r=&mut text;let n=await leaf();r.push_str("!");print(text);print(n);}',
        "hello!\n7\n",
    ),
    "scalar": (
        "async fn main(){let data=1;let r=&mut data;let n=await leaf();r=r+n;print(data);}",
        "8\n",
    ),
    "shared_aliases": (
        'async fn main(){let text=String::from("same");let a=&text;let b=&text;let n=await leaf();print(a);print(b);}',
        "same\nsame\n",
    ),
    "independent_owners": (
        'async fn main(){let a=String::from("a");let b=String::from("b");let read=&a;let edit=&mut b;let n=await leaf();print(read);edit.push_str("!");print(b);}',
        "a\nb!\n",
    ),
    "multiple_awaits": (
        'async fn main(){let text=String::from("many");let r=&mut text;let a=await leaf();r.push_str("1");let b=await leaf();r.push_str("2");print(text);}',
        "many12\n",
    ),
    "lexical_shared": (
        'async fn main(){let text=String::from("lexical");borrow shared text as r{let n=await leaf();print(r);}text.push_str("!");print(text);}',
        "lexical\nlexical!\n",
    ),
    "lexical_unique": (
        "async fn main(){let data=1;borrow unique data as edit{let n=await leaf();edit=n;}print(data);}",
        "7\n",
    ),
    "early_return": (
        'async fn work(text:String,early:bool)->i32{let r=&text;if early{return 1;}let n=await leaf();print(r);return n;}fn main(){print(@block_on(work(String::from("early"),true)));print(@block_on(work(String::from("resumed"),false)));}',
        "1\nresumed\n7\n",
    ),
    "owned_record": (
        'struct Pair{left:String,right:String,}async fn main(){let pair=Pair{left:String::from("left"),right:String::from("right")};let view=&pair;let n=await leaf();print(view.left.as_str());print(view.right.as_str());}',
        "left\nright\n",
    ),
    "box": (
        "async fn main(){let data=Box<i32>::new(42);let view=&data;let n=await leaf();print(view.get());}",
        "42\n",
    ),
    "vec": (
        "async fn main(){let data=Vec<i32>{1,2};let view=&data;let n=await leaf();print(view.len());match view.get(1){Some(item)=>print(item),None=>{}}}",
        "2\n2\n",
    ),
    "never_started": (
        'async fn work(text:String){let r=&text;let n=await leaf();print(r);}fn main(){let future=work(String::from("never"));future.drop();print(1);}',
        "1\n",
    ),
    "staggered_aliases": (
        'async fn main(){let text=String::from("same");let a=&text;let b=&text;let first=await leaf();print(a);let second=await leaf();print(b);text.push_str("!");print(text);}',
        "same\nsame\nsame!\n",
    ),
    "continuation_cfg": (
        "async fn main(){let data=0;let edit=&mut data;let n=await leaf();for i in 0..3{edit=edit+1;}if n==7{print(edit);}else{print(0);}print(data);}",
        "3\n3\n",
    ),
}
negative = {
    "stored_carrier": (
        "async fn main(){let data=1;let view=Option::Some(&data);let n=await leaf();match view{Some(r)=>print(r),None=>{}}}",
        "borrow remains active across",
    ),
    "field": (
        "struct Data{value:i32,}async fn main(){let data=Data{value:1};let r=&data.value;let n=await leaf();print(r);}",
        "borrow remains active across",
    ),
    "element": (
        "async fn main(){let data:i32[2]=[1,2];let r=&data[0];let n=await leaf();print(r);}",
        "borrow remains active across",
    ),
    "reborrow": (
        "async fn main(){let data=1;let parent=&mut data;let child=&*parent;let n=await leaf();print(child);parent=2;}",
        "borrow remains active across",
    ),
    "overlap": (
        "async fn main(){let data=1;let r=&data;let w=&mut data;let n=await leaf();print(r);w=2;}",
        "error[",
    ),
    "owner_drop": (
        'async fn main(){let data=String::from("x");let r=&data;data.drop();let n=await leaf();print(r);}',
        "error[",
    ),
    "borrowed_operand": (
        "fn inspect(value:&i32,n:i32)->i32{return value+n;}async fn main(){let data=1;let r=&data;print(inspect(r,await leaf()));}",
        "borrowed expression value remains active across",
    ),
    "nested_boundary": (
        "async fn main(){let data=1;let r=&data;if true{let n=await leaf();print(r);}}",
        "borrow remains active across",
    ),
    "loop": (
        "async fn main(){let data=1;let r=&data;for i in 0..2{let n=await leaf();print(r);}}",
        "borrow remains active across",
    ),
    "external_capture": (
        "async fn work(value:&i32){let n=await leaf();print(value);}fn main(){}",
        "owned",
    ),
}


def run(args, **kw):
    return subprocess.run(list(map(str, args)), capture_output=True, text=True, timeout=90, **kw)


with tempfile.TemporaryDirectory() as directory:
    source, binary = Path(directory) / "program.isa", Path(directory) / "program"
    for opt in ["-O0", "-O2"]:
        for name, (text, expected) in cases.items():
            source.write_text(prefix + text)
            built = run(
                [
                    compiler,
                    source,
                    opt,
                    *(["--sanitize"] if sanitized else []),
                    "--runtime-test-counters",
                    "-o",
                    binary,
                ]
            )
            assert built.returncode == 0, (name, built.stderr)
            done = run([binary], env=env)
            assert done.returncode == 0 and done.stdout == expected, (name, done)
            balance = re.fullmatch(r"ISAC_RT allocations=(\d+) frees=(\d+)\n", done.stderr)
            assert balance and balance[1] == balance[2], (name, done)
    for name, (text, message) in negative.items():
        source.write_text(prefix + text)
        bad = run([compiler, source, "--check", "--color=never"])
        assert bad.returncode == 1 and message in bad.stderr and not bad.stdout, (name, bad)
    source.write_text(prefix + cases["unique"][0])
    for stage in ["async-core", "state-machine", "erased", "lir", "mlir", "llvm"]:
        dump = run([compiler, source, "--emit-" + stage])
        assert dump.returncode == 0, (stage, dump.stderr)
        if stage == "async-core":
            assert "reconstruct-borrow(" in dump.stdout
        else:
            assert "reconstruct-borrow(" not in dump.stdout
print(f"{2 * len(cases)} local suspension borrow native runs and {len(negative)} rejections passed")
