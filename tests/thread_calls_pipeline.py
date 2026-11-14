from pathlib import Path
import os
import re
import subprocess
import sys
import tempfile

compiler, root, *sanitized = sys.argv[1:]
prefix = '@import "std.vec"\n@import "std.box"\n'
env = dict(
    os.environ,
    ASAN_OPTIONS="detect_leaks=1:halt_on_error=1:detect_stack_use_after_return=1",
    UBSAN_OPTIONS="halt_on_error=1",
)
cases = {
    "scalars": (
        "fn work(a:f32,b:f64,n:i8)->f64{return (a as f64)+b+(n as f64);}fn main(){print(@thread(work(1.5f32,2.5,-2i8)));}",
        "2\n",
    ),
    "no_args": ("fn work()->i32{return 7;}fn main(){print(@thread(work()));}", "7\n"),
    "unit": ("fn work(){}fn main(){@thread(work());print(1);}", "1\n"),
    "string": (
        'fn work(text:String)->String{text.push_str("!");return text.move();}fn main(){let text=String::from("owned");let result=@thread(work(text.move()));print(result);}',
        "owned!\n",
    ),
    "vec": (
        'fn work(values:Vec<String>)->Vec<String>{values.push(String::from("worker"));return values.move();}fn main(){let values=Vec<String>{"caller"};let result=@thread(work(values.move()));print(result.len());let text=result.pop().unwrap();print(text);}',
        "2\nworker\n",
    ),
    "box": (
        'fn work(value:Box<String>)->Box<String>{return value.move();}fn main(){let value=Box<String>::new(String::from("box"));let result=@thread(work(value.move()));let text=result.into_inner();print(text);}',
        "box\n",
    ),
    "record": (
        'struct Data{text:String,code:i32,}fn work()->Data{return Data{text:String::from("record"),code:9};}fn forward()->Data{return @thread(work());}fn main(){let result=forward();print(result.text.as_str());print(result.code);}',
        "record\n9\n",
    ),
    "enum": (
        'enum Value{Text(String),None,}fn work()->Value{return Value::Text(String::from("enum"));}fn main(){let result=@thread(work());match result{Value::Text(text)=>print(text),Value::None=>{},}}',
        "enum\n",
    ),
    "shared": (
        "fn work(a:&i32[],b:&i32[])->i32{return a[0]+b[1];}fn main(){let data:i32[2]=[1,2];print(@thread(work(&data,&data)));print(data[0]);}",
        "3\n1\n",
    ),
    "unique": (
        "fn work(data:&mut i32[]){data[0]=data[0]+10;}fn main(){let data:i32[2]=[1,2];@thread(work(&mut data));print(data[0]);}",
        "11\n",
    ),
    "reference_heap": (
        'fn work(text:&mut String)->usize{text.push_str("!");return text.len();}fn main(){let text=String::from("borrowed");print(@thread(work(&mut text)));print(text);}',
        "9\nborrowed!\n",
    ),
    "reference_struct": (
        "struct Data{value:i32,}fn work(data:&mut Data){data.value=data.value+1;}fn main(){let data=Data{value:4};@thread(work(&mut data));print(data.value);}",
        "5\n",
    ),
    "str_view": (
        'fn work(text:str)->usize{let owned=String::from(text);return owned.len();}fn main(){let text=String::from("view");print(@thread(work(text.as_str())));text.push_str("!");print(text);}',
        "4\nview!\n",
    ),
    "last_use": (
        "fn inspect(data:&i32[])->i32{return data[0];}fn work(data:&mut i32[]){data[0]=7;}fn main(){let data:i32[1]=[1];let r:&i32[]=&data;print(inspect(r));@thread(work(&mut data));print(data[0]);}",
        "1\n7\n",
    ),
}
negative = {
    "io": ("fn work()->i32{print(1);return 1;}fn main(){@thread(work());}", "unproved"),
    "external": (
        'extern "C" fn abs(x:i32)->i32;fn work()->i32{unsafe{return abs(-1);}}fn main(){@thread(work());}',
        "unproved",
    ),
    "borrow_overlap": (
        "fn work(a:&i32[],b:&mut i32[]){b[0]=a[0];}fn main(){let a:i32[1]=[1];@thread(work(&a,&mut a));}",
        "error[",
    ),
    "double_unique": (
        "fn work(a:&mut i32[],b:&mut i32[]){}fn main(){let a:i32[1]=[1];@thread(work(&mut a,&mut a));}",
        "error[",
    ),
    "after_move": (
        'fn work(text:String){}fn main(){let text=String::from("x");@thread(work(text.move()));print(text);}',
        "moved",
    ),
    "nested": (
        "fn child()->i32{return 1;}fn work()->i32{return @thread(child());}fn main(){@thread(work());}",
        "unproved",
    ),
    "expression": ("fn main(){@thread(1+2);}", "function call"),
    "borrow_return": (
        "fn work(data:&i32)->&i32{return data;}fn main(){let data=1;@thread(work(&data));}",
        "function call",
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
            assert (not done.stderr) or (balance and balance[1] == balance[2]), (name, done)
    for name, (text, message) in negative.items():
        source.write_text(prefix + text)
        bad = run([compiler, source, "--check", "--color=never"])
        assert bad.returncode == 1 and not bad.stdout and message in bad.stderr, (name, bad)
    for stage in ["core", "regions", "erased", "lir", "mlir", "llvm"]:
        dump = run([compiler, Path(root) / "examples/thread_owned.isa", "--emit-" + stage])
        assert dump.returncode == 0, (stage, dump.stderr)
        if stage == "core":
            assert "thread.call" in dump.stdout and "caller-owned noescape" in dump.stdout
        if stage in ["erased", "lir", "mlir", "llvm"]:
            assert "isac_rt_thread_call" in dump.stdout and "thread.call" not in dump.stdout
            for proof in ["LoanId", "RegionId", "BorrowProvenance"]:
                assert proof not in dump.stdout
print(f"{2 * len(cases)} scoped thread native runs, {len(negative)} rejections and 6 dumps passed")
