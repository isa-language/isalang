from pathlib import Path
import os, subprocess, sys, tempfile

compiler, root, clang, *sanitized = sys.argv[1:]
root = Path(root)
env = {
    **os.environ,
    "ASAN_OPTIONS": "detect_leaks=1:halt_on_error=1:detect_stack_use_after_return=1",
    "UBSAN_OPTIONS": "halt_on_error=1:print_stacktrace=1",
}
sanitize = ["--sanitize"] if sanitized else []
cflags = ["-fsanitize=address,undefined", "-fno-sanitize-recover=all"] if sanitized else []


def run(args, expected=None):
    r = subprocess.run([str(a) for a in args], capture_output=True, text=True, env=env)
    assert r.returncode == 0, (args, r.returncode, r.stdout, r.stderr)
    if expected is not None:
        assert r.stdout == expected, (args, r.stdout, expected)
    assert "runtime error:" not in r.stderr and "ERROR: AddressSanitizer" not in r.stderr, r.stderr
    return r.stdout


with tempfile.TemporaryDirectory(prefix="isac C ABI ") as directory:
    base = Path(directory)
    support = base / "support.c"
    support.write_text("""#include <stdint.h>
#include <stdbool.h>
#include <stddef.h>
int8_t negate_i8(int8_t x){return -x;}
uint8_t echo_u8(uint8_t x){return x;}
int16_t negate_i16(int16_t x){return -x;}
uint16_t echo_u16(uint16_t x){return x;}
int64_t negate_i64(int64_t x){return -x;}
uint64_t echo_u64(uint64_t x){return x;}
size_t echo_size(size_t x){return x;}
ptrdiff_t echo_signed_size(ptrdiff_t x){return x;}
float half_f32(float x){return x/2;}
double half_f64(double x){return x/2;}
bool invert_bool(bool x){return !x;}
void update(int32_t *x){++*x;}
void update_bool(bool *x){*x=!*x;}
void update_float(float *x){*x=3.5f;}
static int32_t global=42;
static int32_t *pointer=&global;
int32_t **nested_pointer(void){return &pointer;}
static int calls;
int32_t once(void){return ++calls;}
int32_t evaluate(int32_t a,int32_t b){return a*10+b;}
""")
    object = base / "support.o"
    run([clang, *cflags, "-c", support, "-o", object])
    source = base / "abi.isa"
    source.write_text("""extern "C" type Byte=u8;
extern "C" fn negate_i8(x:i8)->i8;
extern "C" fn echo_u8(x:Byte)->Byte;
extern "C" fn negate_i16(x:i16)->i16;
extern "C" fn echo_u16(x:u16)->u16;
extern "C" fn negate_i64(x:i64)->i64;
extern "C" fn echo_u64(x:u64)->u64;
extern "C" fn echo_size(x:usize)->usize;
extern "C" fn echo_signed_size(x:isize)->isize;
extern "C" fn half_f32(x:f32)->f32;
extern "C" fn half_f64(x:f64)->f64;
extern "C" fn invert_bool(x:bool)->bool;
extern "C" fn update(x:*mut i32);
extern "C" fn update_bool(x:*mut bool);
extern "C" fn update_float(x:*mut f32);
extern "C" fn nested_pointer()->*const *mut i32;
extern "C" fn once()->i32;
extern "C" fn evaluate(a:i32,b:i32)->i32;
unsafe fn read(p:*const i32)->i32{return *p;}
fn main()->i32 {
 unsafe {
  @println("{} {} {} {}",negate_i8(-7),echo_u8(250),negate_i16(-32000),echo_u16(60000));
  @println("{} {}",negate_i64(-5000000000i64),echo_u64(18000000000000000000u64));
  @println("{} {}",echo_size(123),echo_signed_size(-123));
  @println("{} {} {}",half_f32(7.0f32),half_f64(5.0),invert_bool(true));
  let x=41; let p=&mut x as *mut i32; update(p); @println("{}",read(p as *const i32));
  *p=43; @println("{}",*p);
  let b=true;update_bool(&mut b as *mut bool);@println("{}",b);
  let f=1.0f32;update_float(&mut f as *mut f32);@println("{}",f);
  let pp=nested_pointer(); @println("{}",**pp);
  let raw=p as usize; let restored=raw as *mut i32; @println("{}",*restored);
  @println("{}",evaluate(once(),once()));
 }
 return 0;
}""")
    expected = "7 250 32000 60000\n5000000000 18000000000000000000\n123 -123\n3.5 2.5 false\n42\n43\nfalse\n3.5\n42\n43\n12\n"
    for level in ["-O0", "-O2"]:
        program = base / "native program"
        run([compiler, source, level, "-o", program, "--link-object", object, *sanitize])
        run([program], expected)
        record_object = base / "record.o"
        run([clang, *cflags, "-c", root / "examples/ffi/record.c", "-o", record_object])
        run(
            [
                compiler,
                root / "examples/ffi/record.isa",
                level,
                "-o",
                program,
                "--link-object",
                record_object,
                *sanitize,
            ]
        )
        run([program], "3 40 true\n47 2.5\nsize=40 align=8\n")

        libc = base / "libc.isa"
        libc.write_text(
            'extern "C" fn abs(x:i32)->i32;extern "C" fn sqrt(x:f64)->f64;'
            'fn main(){unsafe{@println("{} {}",abs(-42),sqrt(9.0));}}'
        )
        run([compiler, libc, level, "-o", program, "-lm", *sanitize])
        run([program], "42 3\n")

        export = base / "exports.o"
        if sanitized:
            ir = base / "exports.ll"
            run(
                [
                    compiler,
                    root / "examples/ffi/exports.isa",
                    "--library",
                    "--emit-llvm",
                    level,
                    "-o",
                    ir,
                    "--sanitize",
                ]
            )
            run([clang, *cflags, "-c", ir, "-o", export])
        else:
            run(
                [
                    compiler,
                    root / "examples/ffi/exports.isa",
                    "--library",
                    "--emit-obj",
                    level,
                    "-o",
                    export,
                ]
            )
        run(
            [
                clang,
                *cflags,
                root / "examples/ffi/driver.c",
                export,
                root / "build/libisac_runtime.a",
                "-o",
                program,
            ]
        )
        run([program], "42 2.5 42\n")

    combined = base / "combined.isa"
    combined.write_text(
        'extern "C" fn multiply(a:i32,b:i32)->i32;'
        'extern "C" fn language_compute(x:i32)->i32{let y=unsafe{multiply(x,2)};return y+2;}'
    )
    obj = base / "combined.o"
    run([compiler, combined, "--library", "--emit-obj", "-o", obj])
    driver = base / "combined.c"
    driver.write_text(
        "#include <stdio.h>\nint multiply(int a,int b){return a*b;}\n"
        'extern int language_compute(int);int main(void){printf("%d\\n",language_compute(20));return 0;}'
    )
    program = base / "bidirectional"
    run([clang, *cflags, driver, obj, root / "build/libisac_runtime.a", "-o", program])
    run([program], "42\n")
    cleanup = root / "tests/ffi/valid_unsafe_keeps_cleanup.isa"
    run([compiler, cleanup, "-o", program, "--runtime-test-counters", *sanitize])
    r = subprocess.run([str(program)], capture_output=True, text=True, env=env)
    assert r.returncode == 0 and "allocations=1 frees=1" in r.stderr, r
    for stage in ["ast", "hir", "core", "regions", "erased", "lir", "mlir", "llvm", "c-bridge"]:
        text = run([compiler, root / "examples/ffi/record.isa", "--emit-" + stage])
        if stage == "core":
            assert "unsafe.scope" in text and "foreign.call" in text and "raw.load" in text, text
        if stage in ["erased", "lir", "mlir", "llvm"]:
            assert "__isac_c_import_" in text, (stage, text)
            for proof in ["unsafe.scope", "foreign.call", "LoanId", "RegionId", "quantity="]:
                assert proof not in text, (stage, proof)
        if stage == "llvm":
            assert "@sample_new" in text and "@sample_free" in text, text
        if stage == "c-bridge":
            assert "_Static_assert" in text and "C field offset mismatch" in text, text

    for triple in ["i686-unknown-linux-gnu", "aarch64-unknown-linux-gnu"]:
        r = subprocess.run(
            [
                compiler,
                root / "examples/ffi/exports.isa",
                "--library",
                "--emit-obj",
                "--target=" + triple,
                "-o",
                base / "cross.o",
            ],
            capture_output=True,
            text=True,
        )
        if r.returncode:
            assert "target unavailable" in r.stderr, (triple, r.stderr)
        else:
            assert (base / "cross.o").stat().st_size > 0
print("C imports/exports, typed pointers, native layouts, unsafe checking and erasure validated")
