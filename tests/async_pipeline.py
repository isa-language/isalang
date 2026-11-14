from pathlib import Path
import os
import re
import resource
import subprocess
import sys
import tempfile

compiler, root, *sanitized = sys.argv[1:]
resource.setrlimit(resource.RLIMIT_CORE, (0, 0))
prefix = '@import "std.async"\n'
env = dict(
    os.environ,
    ASAN_OPTIONS="detect_leaks=1:halt_on_error=1:detect_stack_use_after_return=1",
    UBSAN_OPTIONS="halt_on_error=1",
)
cases = {
    "partial_owner": (
        'struct Pair{a:String,b:String,}async fn leaf(){}async fn main(){let data=Pair{a:String::from("a"),b:String::from("b")};let first=data.a.move();await leaf();print(first);data.a=String::from("c");print(data.a.as_str());print(data.b.as_str());}',
        "a\nc\nb\n",
        0,
    ),
    "thread_then_await": (
        'fn append(text:&mut String){text.push_str("!");}async fn task()->i32{return 7;}async fn main(){let text=String::from("joined");@thread(append(&mut text));let value=await task();@println("{} {}",text,value);}',
        "joined! 7\n",
        0,
    ),
    "discard": ("async fn task(){print(99);}fn main(){task();print(1);}", "1\n", 0),
    "lazy_drop": (
        'async fn task(text:String){print(text);}fn main(){let future=task(String::from("never"));@println("created");future.drop();@println("done");}',
        "created\ndone\n",
        0,
    ),
    "block_on": (
        "async fn task(x:i32)->i32{return x+1;}fn main(){print(@block_on(task(41)));}",
        "42\n",
        0,
    ),
    "async_main": (
        "async fn task()->i32{return 20;}async fn main()->i32{return await task();}",
        "",
        20,
    ),
    "same_producer": (
        "async fn task(x:i32)->i32{print(x);return x+1;}async fn main(){let a=await task(1);let b=await task(a);print(b);}",
        "1\n2\n3\n",
        0,
    ),
    "owned_result": (
        'async fn task(text:String)->String{let n=await number();text.push_str("!");print(n);return text.move();}async fn number()->i32{return 7;}fn main(){let text=@block_on(task(String::from("owned")));print(text);}',
        "7\nowned!\n",
        0,
    ),
    "borrow_ends": (
        'async fn task()->i32{return 1;}async fn main(){let text=String::from("before");let r=&text;print(r);let n=await task();text.push_str("!");print(text);print(n);}',
        "before\nbefore!\n1\n",
        0,
    ),
    "branch": (
        'async fn task()->i32{return 2;}async fn main(){let text=String::from("x");if true{let value=await task();print(value);}print(text);}',
        "2\nx\n",
        0,
    ),
    "early": (
        "async fn task(x:i32)->i32{if x==0{return 10;}let a=await number();return a;}async fn number()->i32{return 20;}fn main(){print(@block_on(task(0)));print(@block_on(task(1)));}",
        "10\n20\n",
        0,
    ),
    "result": (
        "async fn task()->Result<i32,i32>{let value=await number();return Result::Ok(value);}async fn number()->i32{return 5;}async fn main(){let result=await task();match result{Result::Ok(x)=>print(x),Result::Err(e)=>print(e),}}",
        "5\n",
        0,
    ),
    "heap_carrier": (
        "async fn number()->i32{return 1;}async fn task(value:Box<i32>)->i32{let n=await number();return value.into_inner()+n;}fn main(){let carrier=Box<i32>::new(2);print(@block_on(task(carrier.move())));}",
        "3\n",
        0,
    ),
}
negative = {
    "borrow_future": (
        "async fn task(){}fn main(){let future=task();@block_on(&future);}",
        "owned Future",
    ),
    "await_outside": (
        "async fn task()->i32{return 1;}fn main(){let x=await task();}",
        "`await` can only be used inside",
    ),
    "not_future": ("async fn main(){let x=await 10;}", "requires a Future"),
    "shared_across": (
        'async fn task(){}async fn main(){let text=String::from("x");let r=Option::Some(&text);await task();match r{Some(view)=>print(view),None=>{}}}',
        "borrow remains active across",
    ),
    "unique_across": (
        'struct View{value:&mut String,}async fn task(){}async fn main(){let text=String::from("x");let w=View{value:&mut text};await task();let edit=&mut w.value;edit.push_str("!");}',
        "borrow remains active across",
    ),
    "moved_future": (
        "async fn task(){}fn main(){let a=task();let b=a.move();@block_on(a);b.drop();}",
        "moved",
    ),
    "twice": (
        "async fn task(){}fn main(){let future=task();@block_on(future);@block_on(future);}",
        "moved",
    ),
    "borrow_input": ("async fn task(value:&i32)->i32{return 1;}fn main(){}", "owned"),
    "loop_cut": (
        "async fn task(){}async fn main(){for i in 0..2 {await task();}}",
        "edge contract",
    ),
    "recursion": (
        "async fn task(){let future=task();future.drop();}fn main(){@block_on(task());}",
        "recursive async",
    ),
    "private_storage": (
        "async fn task()->i32{return 1;}fn main(){let future=task();future.into_inner();}",
        "Future storage is private",
    ),
}


def command(args, **kw):
    return subprocess.run(list(map(str, args)), capture_output=True, text=True, timeout=90, **kw)


with tempfile.TemporaryDirectory() as directory:
    path, binary = Path(directory) / "program.isa", Path(directory) / "program"
    for opt in ["-O0", "-O2"]:
        for name, (text, output, code) in cases.items():
            path.write_text(prefix + text)
            built = command(
                [
                    compiler,
                    path,
                    opt,
                    *(["--sanitize"] if sanitized else []),
                    "--runtime-test-counters",
                    "-o",
                    binary,
                ]
            )
            assert built.returncode == 0, (name, built.stderr)
            done = command([binary], env=env)
            assert done.returncode == code and done.stdout == output, (name, done)
            balance = re.fullmatch(r"ISAC_RT allocations=(\d+) frees=(\d+)\n", done.stderr)
            assert balance and balance[1] == balance[2], (name, done)
    for name, (text, message) in negative.items():
        path.write_text(prefix + text)
        bad = command([compiler, path, "--check", "--color=never"])
        assert bad.returncode == 1 and message in bad.stderr and not bad.stdout, (name, bad)
    for stage in ["ast", "hir", "async-core", "state-machine", "erased", "lir", "mlir", "llvm"]:
        dump = command([compiler, Path(root) / "examples/async_local.isa", "--emit-" + stage])
        assert dump.returncode == 0, (stage, dump.stderr)
        if stage == "async-core":
            assert (
                "future.create" in dump.stdout
                and "= await " in dump.stdout
                and "kind=Async" in dump.stdout
            )
        if stage == "state-machine":
            assert (
                "__await_site_" in dump.stdout
                and "__future_create_" in dump.stdout
                and "kind=Async" not in dump.stdout
            )
        if stage in ["erased", "lir", "mlir", "llvm"]:
            for proof in ["future.create", "LoanId", "RegionId", "BorrowProvenance", "kind=Async"]:
                assert proof not in dump.stdout, (stage, proof)
print(f"{len(cases) * 2} async native runs, {len(negative)} rejection cases and 8 dumps passed")
