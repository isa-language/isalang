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
    "shared": (
        """struct View{value:&i32,}
 fn inspect(value:&i32)->i32{return value+1;}
 fn work(view:View)->i32{return inspect(view.value);}
 fn main(){let data=7;print(@thread(work(View{value:&data})));data=9;print(data);}""",
        "8\n9\n",
    ),
    "unique": (
        """struct View{value:&mut i32,}
 fn work(view:View){let edit=&mut view.value;edit=42;}
 fn main(){let data=1;let view=View{value:&mut data};@thread(work(view.move()));print(data);data=43;print(data);}""",
        "42\n43\n",
    ),
    "mixed": (
        """struct View{read:&i32,edit:&mut i32,owner:String,}
 fn inspect(value:&i32)->i32{return value+1;}
 fn work(view:View)->String{let edit=&mut view.edit;edit=42;let number=inspect(view.read);let owner=view.owner.move();owner.push_str("!");return owner.move();}
 fn main(){let a=7;let b=1;let output=@thread(work(View{read:&a,edit:&mut b,owner:String::from("worker")}));print(output);print(b);a=10;print(a);}""",
        "worker!\n42\n10\n",
    ),
    "nested_generic": (
        """struct View<T>{value:&mut T,}struct Outer<T>{inner:View<T>,owner:String,}
 impl<T> Outer<T>{pub fn set(unique self,value:T){let edit=&mut self.inner.value;edit=value;}}
 fn work(view:Outer<i32>)->String{view.set(30);return view.owner.move();}
 fn main(){let data=1;let output=@thread(work(Outer<i32>{inner:View<i32>{value:&mut data},owner:String::from("nested")}));print(output);print(data);}""",
        "nested\n30\n",
    ),
    "some_none": (
        """fn work(value:Option<&i32>)->i32{return match value{Option::Some(item)=>item+1,Option::None=>0,};}
 fn main(){let data=4;print(@thread(work(Option::Some(&data))));let none:Option<&i32>=Option::None;print(@thread(work(none.move())));data=8;print(data);}""",
        "5\n0\n8\n",
    ),
    "result": (
        """fn work(value:Result<&i32,i32>)->i32{return match value{Result::Ok(item)=>item+1,Result::Err(error)=>error,};}
 fn main(){let data=4;let ok:Result<&i32,i32>=Result::Ok(&data);print(@thread(work(ok.move())));let error:Result<&i32,i32>=Result::Err(9);print(@thread(work(error.move())));data=6;print(data);}""",
        "5\n9\n6\n",
    ),
    "nested_optional": (
        """struct View{value:Option<&i32>,}
 fn work(view:View)->i32{let value=view.value.move();return match value{Some(item)=>item+2,None=>0,};}
 fn main(){let data=1;print(@thread(work(View{value:Option::Some(&data)})));let none:Option<&i32>=Option::None;print(@thread(work(View{value:none.move()})));data=3;print(data);}""",
        "3\n0\n3\n",
    ),
    "shared_heap": (
        """struct View{value:&String,}
 fn inspect(value:&String)->usize{return value.len();}
 fn work(view:View)->usize{return inspect(view.value);}
 fn main(){let data=String::from("borrowed");print(@thread(work(View{value:&data})));data.push_str("!");print(data);}""",
        "8\nborrowed!\n",
    ),
    "unique_heap": (
        """struct View{value:&mut String,}
 fn work(view:View){let edit=&mut view.value;edit.push_str("!");}
 fn main(){let data=String::from("heap");@thread(work(View{value:&mut data}));print(data);}""",
        "heap!\n",
    ),
    "array_referent": (
        """struct View{value:&i32[2],}
 fn inspect(value:&i32[2])->i32{return value[0]+value[1];}
 fn work(view:View)->i32{return inspect(view.value);}
 fn main(){let data:i32[2]=[2,3];print(@thread(work(View{value:&data})));data[0]=7;print(data[0]);}""",
        "5\n7\n",
    ),
    "alias_carriers": (
        """struct View{value:&i32,}
 fn inspect(value:&i32)->i32{return value+1;}
 fn work(view:View)->i32{return inspect(view.value);}
 fn main(){let data=1;let a=View{value:&data};let b=View{value:&data};print(@thread(work(a.move())));print(@thread(work(b.move())));data=3;print(data);}""",
        "2\n2\n3\n",
    ),
    "transitive": (
        """struct View{value:&mut i32,owner:Box<String>,}
 fn helper(view:View)->Box<String>{let edit=&mut view.value;edit=19;return view.owner.move();}
 fn work(view:View)->Box<String>{return helper(view.move());}
 fn main(){let data=0;let output=@thread(work(View{value:&mut data,owner:Box<String>::new(String::from("boxed"))}));let text=output.into_inner();print(text);print(data);}""",
        "boxed\n19\n",
    ),
    "early_return": (
        """struct View{value:&mut i32,owner:Vec<String>,}
 fn work(view:View,condition:bool)->i32{if condition{return 1;}let edit=&mut view.value;edit=20;return 2;}
 fn main(){let data=0;print(@thread(work(View{value:&mut data,owner:Vec<String>{"drop"}},true)));print(data);print(@thread(work(View{value:&mut data,owner:Vec<String>{"drop"}},false)));print(data);}""",
        "1\n0\n2\n20\n",
    ),
    "worker_loop": (
        """struct View{value:&mut i32,}
 fn work(view:View){let edit=&mut view.value;let i=0;while i<3{edit=edit+1;i=i+1;}}
 fn main(){let data=0;@thread(work(View{value:&mut data}));print(data);}""",
        "3\n",
    ),
    "reborrow": (
        """struct View{value:&mut i32,}
 fn work(view:View){let edit=&mut view.value;edit=40;}
 fn main(){let data=0;let parent=&mut data;let child=&mut *parent;@thread(work(View{value:&mut *child}));parent=41;print(parent);}""",
        "41\n",
    ),
    "argument_evaluation": (
        """struct View{value:&i32,}
 fn capture(view:View,order:i32)->View{print(order);return view.move();}
 fn inspect(value:&i32)->i32{return value+1;}
 fn work(a:View,b:View)->i32{return inspect(a.value)+inspect(b.value);}
 fn main(){let a=1;let b=2;print(@thread(work(capture(View{value:&a},1),capture(View{value:&b},2))));a=3;b=4;print(a+b);}""",
        "1\n2\n5\n7\n",
    ),
    "helper_result": (
        """struct View{value:&mut i32,}
 fn forward(view:View)->View{return view.move();}
 fn work(view:View){let next=forward(view.move());let edit=&mut next.value;edit=21;}
 fn main(){let data=0;@thread(work(View{value:&mut data}));print(data);}""",
        "21\n",
    ),
    "joined_before_await": (
        """@import "std.async"
 struct View{value:&mut i32,}fn work(view:View){let edit=&mut view.value;edit=22;}
 async fn leaf(){}async fn main(){let data=0;let view=View{value:&mut data};@thread(work(view.move()));await leaf();print(data);}""",
        "22\n",
    ),
    "optional_with_owner": (
        """struct Job{view:Option<&i32>,payload:Vec<String>,}
 fn work(job:Job)->Vec<String>{let view=job.view.move();let value=match view{Some(item)=>item+1,None=>0,};if value>0{job.payload.push(String::from("extra"));}return job.payload.move();}
 fn main(){let data=1;let a=@thread(work(Job{view:Option::Some(&data),payload:Vec<String>{"first"}}));print(a.len());let none:Option<&i32>=Option::None;let b=@thread(work(Job{view:none.move(),payload:Vec<String>{"second"}}));print(b.len());data=3;print(data);}""",
        "2\n1\n3\n",
    ),
    "shared_helper_result": (
        """struct View{value:&String,}
 fn reference(value:&String)->&String{return value;}
 fn work(view:View)->usize{let ignored=reference(view.value);let item=reference(view.value);return item.len();}
 fn main(){let data=String::from("shared");print(@thread(work(View{value:&data})));data.push_str("!");print(data);}""",
        "6\nshared!\n",
    ),
}
negative = {
    "overlapping_origins": (
        """struct View{read:&i32,edit:&mut i32,}fn work(view:View){}
 fn main(){let data=0;@thread(work(View{read:&data,edit:&mut data}));}""",
        "error[",
    ),
    "overlapping_arguments": (
        """struct View{value:&mut i32,}fn work(view:View,other:&mut i32){}
 fn main(){let data=0;let view=View{value:&mut data};@thread(work(view.move(),&mut data));}""",
        "error[",
    ),
    "moved_carrier": (
        """struct View{value:&i32,}fn work(view:View){}
 fn main(){let data=0;let view=View{value:&data};@thread(work(view.move()));@thread(work(view.move()));}""",
        "moved",
    ),
    "live_sibling": (
        """struct View{value:&i32,}fn work(view:View){}
 fn main(){let data=0;let a=View{value:&data};let b=View{value:&data};@thread(work(a.move()));data=1;@thread(work(b.move()));}""",
        "error[",
    ),
    "later_argument": (
        """struct View{value:&i32,}fn mutate(value:&mut i32)->i32{value=10;return 1;}fn work(view:View,n:i32){}
 fn main(){let data=0;@thread(work(View{value:&data},mutate(&mut data)));}""",
        "error[",
    ),
    "borrowed_result": (
        """struct View{value:&i32,}fn work(view:View)->View{return view.move();}
 fn main(){let data=1;let output=@thread(work(View{value:&data}));}""",
        "reference-free result",
    ),
    "local_escape": (
        """struct View{value:&i32,}fn bad()->View{let local=1;return View{value:&local};}fn work(view:View){}
 fn main(){@thread(work(bad()));}""",
        "error[",
    ),
    "future_capture": (
        """@import "std.async"
 struct View{value:&i32,}async fn work(view:View){}fn main(){}""",
        "owned",
    ),
    "heap_reference_storage": (
        """struct View{value:&i32,}fn work(value:Box<View>){}
 fn main(){}""",
        "error[",
    ),
    "hidden_io": (
        """struct View{value:&i32,}fn hidden(view:View){print(view.value);}fn work(view:View){hidden(view.move());}
 fn main(){let data=1;@thread(work(View{value:&data}));}""",
        "unproved",
    ),
    "unsafe_wrapper": (
        """struct View{value:&i32,}impl View{unsafe fn handle(shared self)->usize{return 0usize;}}
 fn work(view:View){}fn main(){let data=1;@thread(work(View{value:&data}));}""",
        "thread transfer contract",
    ),
    "live_across_await": (
        """@import "std.async"
 struct View{value:&i32,}fn work(view:View){}async fn leaf(){}
 async fn main(){let data=1;let view=View{value:&data};await leaf();@thread(work(view.move()));}""",
        "borrow remains active across",
    ),
    "protocol_field": (
        """@import "std.buffer"
 struct View{value:&i32,buffer:Buffer,}fn work(view:View){let buffer=view.buffer.move();buffer.release();}
 fn main(){let data=1;@thread(work(View{value:&data,buffer:Buffer::new(2)}));}""",
        "thread transfer contract",
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
            assert not done.stderr or (balance and balance[1] == balance[2]), (name, done)
    for name, (text, message) in negative.items():
        source.write_text(prefix + text)
        bad = run([compiler, source, "--check", "--color=never"])
        assert bad.returncode == 1 and message in bad.stderr and not bad.stdout, (name, bad)
    for stage in ["hir", "core", "regions", "concurrency", "erased", "lir", "mlir", "llvm"]:
        dump = run([compiler, Path(root) / "examples/thread_carriers.isa", "--emit-" + stage])
        assert dump.returncode == 0, (stage, dump.stderr)
        if stage == "core":
            assert "thread.call" in dump.stdout and "caller-owned noescape" in dump.stdout
        if stage == "concurrency":
            assert "queries only; execution requires checked contracts" in dump.stdout
            assert (
                "require allocator thread contract for Send" in dump.stdout
                and "at self.field[2]" in dump.stdout
            )
        if stage in ["erased", "lir", "mlir", "llvm"]:
            assert "isac_rt_thread_call" in dump.stdout
            for proof in ["LoanId", "RegionId", "BorrowProvenance", "thread.call"]:
                assert proof not in dump.stdout, (stage, proof)
print(f"{2 * len(cases)} scoped carrier native runs, {len(negative)} rejections and 8 dumps passed")
