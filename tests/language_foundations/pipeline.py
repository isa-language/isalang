import os
import shutil
import subprocess
import sys
import tempfile
from pathlib import Path

compiler, root = sys.argv[1:3]
root = Path(root)
sanitized = len(sys.argv) > 3
fixtures = root / "tests/language_foundations"


def command(args):
    return subprocess.run(args, capture_output=True, text=True, timeout=120)


valid = {
    "valid_fields": "factory\n42 255 2048 11 22\n",
    "valid_repeat": "once\n255 0\n",
    "valid_text": "modern!\n",
    "valid_call_loans": "3\n2\n",
    "valid_array_types": "true 1.5 0 0\n",
    "valid_shadow_owners": "10 20\n",
    "valid_numerals": "4294967295 18446744073709551615 -128 493 1024 3.125 1\n4 10\n",
}
with tempfile.TemporaryDirectory() as directory:
    directory = Path(directory)
    for name, expected in valid.items():
        source = fixtures / (name + ".isa")
        r = command([compiler, str(source), "--check", "--color=never"])
        assert r.returncode == 0, (name, r.stderr)
        for optimization in ["-O0", "-O2"]:
            exe = directory / name
            args = [compiler, str(source), optimization, "-o", str(exe)]
            if sanitized:
                args += ["--sanitize"]
            r = command(args)
            assert r.returncode == 0, (name, r.stderr)
            r = subprocess.run(
                [str(exe)],
                capture_output=True,
                text=True,
                timeout=30,
                env={
                    **os.environ,
                    "ASAN_OPTIONS": "detect_leaks=1:halt_on_error=1",
                    "UBSAN_OPTIONS": "halt_on_error=1",
                },
            )
            assert r.returncode == 0 and r.stdout == expected and not r.stderr, (name, r)

    native = directory / "pixels.c"
    obj = directory / "pixels.o"
    native.write_text(
        "#include <stdint.h>\n#include <stddef.h>\n"
        "void update_pixels(uint8_t *pixels, size_t n) { if(n) pixels[n-1]=255; }\n"
    )
    args = [shutil.which("clang"), "-c", str(native), "-o", str(obj)]
    if sanitized:
        args += ["-fsanitize=address,undefined"]
    r = command(args)
    assert r.returncode == 0, r.stderr
    for optimization in ["-O0", "-O2"]:
        exe = directory / "field_ffi"
        args = [
            compiler,
            str(fixtures / "valid_field_ffi.isa"),
            optimization,
            "--link-object",
            str(obj),
            "-o",
            str(exe),
        ]
        if sanitized:
            args += ["--sanitize"]
        r = command(args)
        assert r.returncode == 0, r.stderr
        r = subprocess.run(
            [str(exe)],
            capture_output=True,
            text=True,
            timeout=30,
            env={
                **os.environ,
                "ASAN_OPTIONS": "detect_leaks=1:halt_on_error=1",
                "UBSAN_OPTIONS": "halt_on_error=1",
            },
        )
        assert r.returncode == 0 and r.stdout == "11 255 22\n" and not r.stderr, r

    r = command([compiler, str(fixtures / "valid_repeat.isa"), "--emit-core"])
    assert r.returncode == 0 and "array.repeat" in r.stdout, r.stderr

    for flag in [
        "--emit-ast",
        "--emit-hir",
        "--emit-erased",
        "--emit-lir",
        "--emit-mlir",
        "--emit-llvm",
    ]:
        r = command([compiler, str(fixtures / "valid_repeat.isa"), flag])
        assert r.returncode == 0, (flag, r.stderr)
        if flag in ["--emit-erased", "--emit-lir"]:
            assert len(r.stdout) < 30000, (flag, len(r.stdout))

    invalid = {
        "invalid_field_shared_write": "E0311",
        "invalid_field_slice_overlap": "E0312",
    }
    for name, code in invalid.items():
        r = command([compiler, str(fixtures / (name + ".isa")), "--check", "--color=never"])
        assert r.returncode == 1 and f"error[{code}]" in r.stderr, (name, r.stderr)
    bad = {
        "huge_index": 'let x = [1; 2]; @println("{}", x[0xffff_ffff_ffff_ffffu64]);',
        "binary_digit": "let x = 0b102;",
        "octal_digit": "let x = 0o89;",
        "hex_empty": "let x = 0x;",
        "hex_digit": "let x = 0xgg;",
        "separator_start": "let x = 0x_FF;",
        "separator_double": "let x = 1__0;",
        "separator_end": "let x = 10_;",
        "suffix": "let x = 0xffu7;",
        "unsigned_overflow": "let x = 0x100u8;",
        "negative_unsigned": "let x = -0b1u8;",
        "u64_overflow": "let x = 0x1_0000_0000_0000_0000u64;",
        "decimal_overflow": "let x = 18446744073709551616u64;",
        "repeat_runtime_length": "let n = 10; let x = [0; n];",
        "repeat_linear": 'let x = [String::from("a"); 2];',
        "repeat_zero": "let x = [1; 0];",
        "repeat_mismatch": "let x: i32[3] = [1; 4];",
        "non_array_uninitialized": "let x: i32;",
        "same_call_alias": "let x = Cpu::new(); combine_refs(&mut x, &mut x);",
        "shared_array_field": 'let x = Cpu::new(); let r = &x; r.cells[0] = 1u8; @println("{}", r.value);',
        "owned_array_field_move": "let x = Cpu::new(); let y = x.cells.move();",
    }
    prefix = (
        "struct Cpu { value: i32, cells: u8[8], }\nfn combine_refs(a: &mut Cpu, b: &mut Cpu) {}\n"
    )
    for name, body in bad.items():
        source = directory / (name + ".isa")
        source.write_text(prefix + "fn main() { " + body + " }\n")
        r = command([compiler, str(source), "--check", "--color=never"])
        assert r.returncode == 1 and "error[" in r.stderr and "error[E900" not in r.stderr, (
            name,
            r.stderr,
        )

    exe = directory / "bounds"
    r = command([compiler, str(fixtures / "invalid_field_bounds.isa"), "-o", str(exe)])
    assert r.returncode == 0, r.stderr
    r = command([str(exe)])
    assert r.returncode != 0 and "index out of bounds" in r.stderr, r
