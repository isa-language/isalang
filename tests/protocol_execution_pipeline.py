from pathlib import Path
import os
import re
import subprocess
import sys
import tempfile

compiler, root, *sanitized = sys.argv[1:]
prefix = '@import "std.async"\n@import "std.vec"\n'
closed = """protocol State{initial Idle;final Idle;state Idle{begin(self)->Busy;}state Busy{finish(self)->Idle;}}
struct Token:State{value:i32,}impl Token{fn begin(self){self.value=41;}fn finish(self){self.value=42;}}
"""
opened = """protocol State{initial Busy;final Idle;state Busy{finish(self)->Idle;}state Idle{}}
struct Token:State{value:i32,}impl Token{fn finish(self){self.value=42;}}
"""
cases = {
    "thread_closed": (
        closed
        + "fn work(value:Token)->i32{value.begin();value.finish();return value.value;}fn main(){print(@thread(work(Token::new())));}",
        "42\n",
    ),
    "thread_initial_nonfinal": (
        opened
        + "fn work(value:Token)->i32{value.finish();return value.value;}fn main(){print(@thread(work(Token::new())));}",
        "42\n",
    ),
    "thread_nested_owned": (
        closed
        + 'struct Job{token:Token,text:String,}fn work(job:Job)->String{let token=job.token.move();token.begin();token.finish();return job.text.move();}fn main(){let result=@thread(work(Job{token:Token::new(),text:String::from("owned")}));print(result);}',
        "owned\n",
    ),
    "thread_containers": (
        closed
        + "fn work(values:Vec<Box<Token>>)->usize{return values.len();}fn main(){let values=Vec<Box<Token>>{};values.push(Box<Token>::new(Token::new()));print(@thread(work(values.move())));}",
        "1\n",
    ),
    "async_input": (
        closed
        + "async fn leaf()->i32{return 1;}async fn task(value:Token)->i32{value.begin();value.finish();let n=await leaf();return value.value+n;}fn main(){print(@block_on(task(Token::new())));}",
        "43\n",
    ),
    "async_local": (
        closed
        + "async fn leaf(){}async fn main(){let value=Token::new();value.begin();value.finish();await leaf();print(value.value);}",
        "42\n",
    ),
    "async_multiple": (
        closed
        + "async fn leaf(){}async fn main(){let value=Token::new();value.begin();value.finish();await leaf();value.begin();value.finish();await leaf();print(value.value);}",
        "42\n",
    ),
    "async_initial_drop": (
        closed
        + "async fn task(value:Token){value.begin();value.finish();print(99);}fn main(){let task=task(Token::new());task.drop();print(1);}",
        "1\n",
    ),
    "async_owned_containers": (
        closed
        + "async fn leaf(){}async fn task(values:Vec<Box<Token>>)->usize{await leaf();return values.len();}fn main(){let values=Vec<Box<Token>>{};values.push(Box<Token>::new(Token::new()));print(@block_on(task(values.move())));}",
        "1\n",
    ),
    "owned_field_presence": (
        closed
        + "struct Job{token:Token,}fn main(){let job=Job{token:Token::new()};let token=job.token.move();token.begin();token.finish();job.token=token.move();job.drop();print(1);}",
        "1\n",
    ),
}
negative = {
    "async_initial_nonfinal": (
        opened + "async fn task(value:Token){value.finish();}fn main(){}",
        "droppable initial state",
    ),
    "async_busy_checkpoint": (
        closed
        + "async fn leaf(){}async fn main(){let value=Token::new();value.begin();await leaf();value.finish();}",
        "non-final",
    ),
    "borrowed_protocol_checkpoint": (
        closed
        + "async fn leaf(){}async fn main(){let value=Token::new();let view=&value;await leaf();print(view.value);}",
        "borrow remains active across",
    ),
    "busy_worker_return": (
        opened + "fn work(value:Token){}fn main(){@thread(work(Token::new()));}",
        "non-final",
    ),
    "nested_busy_drop": (
        closed
        + "struct Job{token:Token,}fn main(){let job=Job{token:Token::new()};let token=job.token.move();token.begin();job.token=token.move();job.drop();}",
        "error[",
    ),
    "uninitialized_protocol_field": (
        closed
        + "struct Job{token:Token,}fn work(value:Job){}fn main(){let job=Job::new();@thread(work(job.move()));}",
        "uninitialized",
    ),
    "opaque_native": (
        '@import "std.buffer"\nfn work(value:Buffer){value.release();}fn main(){@thread(work(Buffer::new(2)));}',
        "thread transfer contract",
    ),
    "unproved_method_effect": (
        "protocol State{initial Idle;final Idle;state Idle{inspect(shared self);}}struct Token:State{value:i32,}impl Token{fn inspect(shared self){print(self.value);}}fn work(value:Token){}fn main(){@thread(work(Token::new()));}",
        "unproved",
    ),
    "released_storage": (
        '@import "std.buffer"\nasync fn leaf(){}async fn main(){let value=Buffer::new(2);value.release();await leaf();}',
        "initial state",
    ),
}
env = dict(
    os.environ,
    ASAN_OPTIONS="detect_leaks=1:halt_on_error=1:detect_stack_use_after_return=1",
    UBSAN_OPTIONS="halt_on_error=1",
)


def run(args, **kw):
    return subprocess.run(list(map(str, args)), capture_output=True, text=True, timeout=90, **kw)


with tempfile.TemporaryDirectory() as folder:
    source, binary = Path(folder) / "program.isa", Path(folder) / "program"
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
            assert not done.stderr or (balance and balance[1] == balance[2]), (name, done)
    for name, (text, message) in negative.items():
        source.write_text(prefix + text)
        bad = run([compiler, source, "--emit-mlir", "--color=never"])
        assert bad.returncode == 1 and message in bad.stderr and not bad.stdout, (name, bad)
print(f"{2 * len(cases)} protocol execution runs and {len(negative)} rejection cases passed")
