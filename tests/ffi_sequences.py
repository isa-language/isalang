from pathlib import Path
import os, subprocess, sys, tempfile

compiler, root, clang, *sanitized = sys.argv[1:]
root = Path(root)
env = {
    **os.environ,
    "ASAN_OPTIONS": "detect_leaks=1:halt_on_error=1:detect_stack_use_after_return=1",
    "UBSAN_OPTIONS": "halt_on_error=1:print_stacktrace=1",
}
cflags = ["-fsanitize=address,undefined", "-fno-sanitize-recover=all"] if sanitized else []
sflags = ["--sanitize"] if sanitized else []


def run(args, output=None):
    result = subprocess.run(list(map(str, args)), capture_output=True, env=env)
    assert result.returncode == 0, (args, result.returncode, result.stdout, result.stderr)
    if output is not None:
        assert result.stdout == output, (args, result.stdout, output)
    assert not result.stderr, (args, result.stderr)
    return result.stdout


with tempfile.TemporaryDirectory(prefix="isac sequences ") as directory:
    base = Path(directory)
    support = base / "support.c"
    support.write_text("""#include <stdint.h>
#include <stddef.h>
#include <stdbool.h>
int32_t sum(const int32_t *p,size_t n){int32_t s=0;for(size_t i=0;i<n;i++)s+=p[i];return s;}
void change(uint8_t *p){p[0]=10;p[1]=20;p[2]=30;}
void adjust(int16_t *p,size_t n){for(size_t i=0;i<n;i++)p[i]-=2;}
void increment(int32_t *p){++*p;}
float average(const float *p){return (p[0]+p[1])/2;}
bool flags(const bool *p,size_t n){return n==2&&p[0]&&!p[1];}
size_t text(const uint8_t *p,size_t n){return n==3&&p[1]==0?n:0;}
size_t string(const uint8_t *p,size_t n){return n==4&&p[2]==0xc3&&p[3]==0xa1?n:0;}
""")
    obj = base / "support.o"
    run([clang, *cflags, "-c", support, "-o", obj])
    source = base / "main.isa"
    source.write_text("""extern "C" fn sum(p:&i32[])->i32;
extern "C" fn change(p:&mut u8[3]);
extern "C" fn adjust(p:&mut i16[]);
extern "C" fn increment(p:&mut i32);
extern "C" fn average(p:&f32[2])->f32;
extern "C" fn flags(p:&bool[])->bool;
extern "C" fn text(p:str)->usize;
extern "C" fn string(p:&String)->usize;
fn main(){
 let a:i32[3]=[10,20,12];let b:u8[3]=[1u8,2u8,3u8];
 let s:i16[2]=[-3i16,5i16];let f:f32[2]=[1.5f32,2.5f32];
 let bo:bool[2]=[true,false];let value=41;let t=String::from("Olá");
 unsafe {
  @println("{} {} {} {} {}",sum(&a),average(&f),flags(&bo),text("a\\0b"),string(&t));
  change(&mut b);adjust(&mut s);increment(&mut value);
 }
 @println("{} {} {} {} {} {} {}",b[0],b[1],b[2],s[0],s[1],value,t);
}""")
    exports = base / "exports.isa"
    exports.write_text("""extern "C" fn language_sum(p:&i32[])->i32{return p[0]+p[1];}
extern "C" fn language_change(p:&mut u8[3]){p[1]=99u8;}
extern "C" fn language_adjust(p:&mut i16[]){p[0]=-7i16;}
extern "C" fn language_increment(p:&mut i32){p=p+1;}
extern "C" fn language_average(p:&f32[2])->f32{return (p[0]+p[1])/2.0f32;}
extern "C" fn language_empty(p:&i32[])->usize{return p.len();}
extern "C" fn language_text(p:str){print(p);}
extern "C" fn language_string(p:&String)->usize{return p.len();}
""")
    driver = base / "driver.c"
    driver.write_text("""#include <stdint.h>
#include <stddef.h>
#include <stdio.h>
extern int32_t language_sum(const int32_t*,size_t);
extern void language_change(uint8_t*);
extern void language_adjust(int16_t*,size_t);
extern void language_increment(int32_t*);
extern float language_average(const float*);
extern size_t language_empty(const int32_t*,size_t);
extern void language_text(const uint8_t*,size_t);
extern size_t language_string(const uint8_t*,size_t);
int main(int argc,char**argv){
 if(argc>1){const uint8_t invalid[]={0xc0,0xaf};language_text(invalid,2);return 0;}
 int32_t a[]={20,22},v=41;uint8_t b[]={1,2,3};int16_t s[]={1,2};float f[]={1.5f,2.5f};
 language_change(b);language_adjust(s,2);language_increment(&v);
 printf("%d %u %d %d %.1f %zu %zu\\n",language_sum(a,2),b[1],s[0],v,language_average(f),
 language_empty(NULL,0),language_string((const uint8_t*)"Olá",4));
 const uint8_t bytes[]={'a',0,'b'};language_text(bytes,3);return 0;}
""")
    for level in ["-O0", "-O2"]:
        program = base / "imported"
        run([compiler, source, level, *sflags, "--link-object", obj, "-o", program])
        run([program], b"42 2 true 3 4\n10 20 30 -5 3 42 Ol\xc3\xa1\n")
        library = base / "exports.o"
        if sanitized:
            ir = base / "exports.ll"
            run([compiler, exports, level, "--library", "--emit-llvm", *sflags, "-o", ir])
            run([clang, *cflags, "-Wno-override-module", "-c", ir, "-o", library])
        else:
            run([compiler, exports, level, "--library", "--emit-obj", "-o", library])
        program = base / "exported"
        runtime = (
            [root / "runtime/src/isac_runtime.c", "-I", root / "runtime/include"]
            if sanitized
            else [root / "build/libisac_runtime.a"]
        )
        run([clang, *cflags, driver, library, *runtime, "-o", program])
        run([program], b"42 99 -7 42 2.0 0 4\na\x00b\n")
        invalid = subprocess.run([str(program), "invalid"], capture_output=True, env=env)
        assert invalid.returncode != 0 and b"invalid UTF-8 at C boundary" in invalid.stderr, invalid
    core = run([compiler, source, "--emit-core"])
    assert b"foreign.call" in core and b"reference.argument" in core, core
    erased = run([compiler, source, "--emit-erased"])
    assert b"address" in erased and b"Own" not in erased and b"LoanId" not in erased, erased
