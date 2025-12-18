from pathlib import Path
import os
import subprocess
import sys
import tempfile

compiler, root, clang, *sanitized = sys.argv[1:]
root = Path(root)
flags = ["-fsanitize=address,undefined", "-fno-sanitize-recover=all"] if sanitized else []
isac_flags = ["--sanitize"] if sanitized else []
env = {
    **os.environ,
    "ASAN_OPTIONS": "detect_leaks=1:halt_on_error=1",
    "UBSAN_OPTIONS": "halt_on_error=1",
}


def run(args):
    result = subprocess.run(
        [str(a) for a in args], capture_output=True, text=True, timeout=60, env=env
    )
    assert result.returncode == 0, (args, result.stdout, result.stderr)
    return result


with tempfile.TemporaryDirectory() as directory:
    base = Path(directory)
    support = base / "support.c"
    support.write_text("""#include <stdint.h>
#include <stdarg.h>
#include <stdbool.h>
typedef int32_t (*Callback)(int32_t);
int32_t apply(Callback cb, int32_t value) { return cb(value); }
static int32_t twice(int32_t value) { return value*2; }
Callback get_callback(void) { return twice; }
Callback null_callback(void) { return 0; }
union Number { uint64_t integer; double decimal; uint8_t padding[56]; };
struct Data { uint8_t tag; union Number number; int32_t tail; };
struct Data *get_data(void) { static struct Data data={7,{0},9}; return &data; }
int64_t sum(int32_t count, ...) {
    int64_t result=0; va_list args; va_start(args,count);
    for(int i=0;i<count;++i) result+=va_arg(args,int);
    va_end(args); return result;
}
double average(int32_t count, ...) {
    double result=0; va_list args; va_start(args,count);
    for(int i=0;i<count;++i) result+=va_arg(args,double);
    va_end(args); return count ? result/count : 0;
}
typedef double (*Mixed)(int32_t *,int8_t,uint16_t,float,bool);
double invoke_mixed(Mixed callback) { int32_t x=10; return callback(&x,-7,3,0.5f,true); }
struct Operations { Callback callback; };
const struct Operations *operations(void) { static struct Operations x={twice}; return &x; }
int32_t var_apply(int32_t count, ...) {
    va_list args; va_start(args,count); Callback cb=va_arg(args,Callback);
    int32_t value=va_arg(args,int); va_end(args); return cb(value);
}
static int count;
int32_t next(void) { return ++count; }
int32_t current(void) { return count; }
""")
    obj = base / "support.o"
    run([clang, *flags, "-c", support, "-o", obj])
    extra = base / "mixed.isa"
    extra.write_text("""extern "C" type Callback=extern "C" fn(i32)->i32;
extern "C" type Mixed=extern "C" fn(*mut i32,i8,u16,f32,bool)->f64;
extern "C" unsafe fn mixed(p:*mut i32,small:i8,word:u16,value:f32,flag:bool)->f64 {
 *p = *p + small as i32 + word as i32;
 if flag { return *p as f64 + value as f64; }
 return 0.0;
}
extern "C" fn invoke_mixed(callback:Mixed)->f64;
extern "C" struct Operations { callback:Callback, }
extern "C" fn operations()->*const Operations;
extern "C" fn var_apply(n:i32,...)->i32;
extern "C" fn sum(n:i32,...)->i64;
extern "C" fn next()->i32;
extern "C" fn current()->i32;
extern "C" fn increment(x:i32)->i32 {return x+1;}
struct Handler { callback:Callback, }
fn main() {unsafe {
 let handler=Handler::new();
 handler.callback=&increment;
 @assert((handler.callback)(41) == 42);
 @assert(invoke_mixed(&mixed) == 6.5);
 @assert(((*operations()).callback)(21) == 42);
 @assert(var_apply(1,&increment,41) == 42);
 @assert(sum(2,next(),next()) == 3i64);
 @assert(current() == 2);
 @assert(sum(10,1u8,2u8,3u8,4u8,5u8,6u8,7u8,8u8,9u8,10u8) == 55i64);
 @println("checked");
}}
""")
    cases = [
        (root / "tests/ffi/valid_callbacks.isa", "42 42\n"),
        (root / "tests/ffi/valid_union.isa", "56 8\n"),
        (root / "tests/ffi/valid_variadic.isa", "60244 2\n"),
        (extra, "checked\n"),
    ]
    for source, expected in cases:
        for opt in ["-O0", "-O2"]:
            binary = base / "program"
            run([compiler, source, opt, *isac_flags, "--link-object", obj, "-o", binary])
            result = run([binary])
            assert result.stdout == expected and not result.stderr, result

    source = root / "tests/ffi/valid_callbacks.isa"
    driver = base / "driver.c"
    driver.write_text("""#include <stdint.h>
#include <stdio.h>
typedef int32_t (*Callback)(int32_t);
extern Callback export_callback(void);
extern int32_t call_export(Callback,int32_t);
static int32_t twice(int32_t x){return x*2;}
int main(void){Callback cb=export_callback();
 if(cb(41)!=42 || call_export(twice,21)!=42)return 1;
 puts("bidirectional");return 0;}
""")
    for opt in ["-O0", "-O2"]:
        library = base / "library.o"
        run(
            [
                compiler,
                source,
                opt,
                *isac_flags,
                "--library",
                "--emit-llvm",
                "-o",
                base / "library.ll",
            ]
        )
        run([clang, *flags, "-Wno-override-module", "-c", base / "library.ll", "-o", library])
        binary = base / "driver"
        if sanitized:
            runtime = [root / "runtime/src/isac_runtime.c", "-I", root / "runtime/include"]
        else:
            runtime = [root / "build/libisac_runtime.a"]
        run([clang, *flags, driver, library, obj, *runtime, "-o", binary])
        result = run([binary])
        assert result.stdout == "bidirectional\n" and not result.stderr, result

    null = base / "null.isa"
    null.write_text(
        'extern "C" type Callback=extern "C" fn(i32)->i32;'
        'extern "C" fn null_callback()->Callback;'
        "fn main(){unsafe {let cb=null_callback(); print(cb(0));}}"
    )
    binary = base / "null"
    run([compiler, null, *isac_flags, "--link-object", obj, "-o", binary])
    result = subprocess.run([str(binary)], capture_output=True, text=True, env=env)
    assert result.returncode != 0 and "null C function pointer" in result.stderr, result
    assert "AddressSanitizer" not in result.stderr and "runtime error:" not in result.stderr, result
    for stage in ["ast", "hir", "core", "erased", "lir", "mlir", "llvm"]:
        result = run([compiler, source, "--emit-" + stage])
        if stage in ["hir", "core"]:
            assert "function.address" in result.stdout and "foreign.call.indirect" in result.stdout
        if stage == "llvm":
            assert "export_callback" in result.stdout and "call_export" in result.stdout
    bridge = run([compiler, root / "tests/ffi/valid_union.isa", "--emit-c-bridge"]).stdout
    assert "union Number" in bridge and "padding[56]" in bridge and "_Static_assert" in bridge
    hir = run([compiler, root / "tests/ffi/valid_variadic.isa", "--emit-hir"]).stdout
    assert "variadic fixed=1" in hir and "c.default.promotion" in hir

    for target in ["i686-unknown-linux-gnu", "aarch64-unknown-linux-gnu"]:
        result = subprocess.run(
            [compiler, str(source), "--target=" + target, "--emit-llvm"],
            capture_output=True,
            text=True,
            timeout=60,
        )
        if "target unavailable in this LLVM build" in result.stderr:
            print(target + ": LLVM backend unavailable; cross-codegen skipped")
            continue
        assert result.returncode == 0, result.stderr
        assert target in result.stdout and "target datalayout" in result.stdout
    print("unions, typed callbacks and variadic promotions: O0/O2, both ABI directions")
