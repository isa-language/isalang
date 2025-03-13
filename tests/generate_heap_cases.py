from pathlib import Path
import json

root = Path(__file__).parent / "heap"
box = '@import "std.box"\n'
buf = '@import "std.buffer"\n'
cases = {
    "valid_heap_alloc": (box, "let a=BoxI32::new(42); print(a.get());", None, "42\n", 1),
    "valid_heap_move": (
        box,
        "let a=BoxI32::new(10); let b=a.move(); print(b.get());",
        None,
        "10\n",
        1,
    ),
    "invalid_heap_use_after_move": (
        box,
        "let a=BoxI32::new(10); let b=a.move(); print(a.get());",
        "E0301",
        "",
        0,
    ),
    "valid_heap_drop": (box, "let a=BoxI32::new(10); a.drop();", None, "", 1),
    "invalid_heap_double_drop": (box, "let a=BoxI32::new(10); a.drop(); a.drop();", "E0301", "", 0),
    "valid_heap_shared_borrow": (
        box,
        "let a=BoxI32::new(10); borrow shared a as v { print(v.get()); }",
        None,
        "10\n",
        1,
    ),
    "valid_heap_multiple_shared": (
        box,
        "let a=BoxI32::new(10); borrow shared a as v { borrow shared a as w { print(v.get()); print(w.get()); } }",
        None,
        "10\n10\n",
        1,
    ),
    "valid_heap_unique_borrow": (
        box,
        "let a=BoxI32::new(10); borrow unique a as v { v.set(20); } print(a.get());",
        None,
        "20\n",
        1,
    ),
    "invalid_heap_shared_write": (
        box,
        "let a=BoxI32::new(10); borrow shared a as v { v.set(20); }",
        "E0311",
        "",
        0,
    ),
    "invalid_heap_two_unique": (
        box,
        "let a=BoxI32::new(10); borrow unique a as v { borrow unique a as w {} }",
        "E0312",
        "",
        0,
    ),
    "invalid_heap_unique_and_shared": (
        box,
        "let a=BoxI32::new(10); borrow unique a as v { borrow shared a as w {} }",
        "E0312",
        "",
        0,
    ),
    "invalid_heap_shared_and_unique": (
        box,
        "let a=BoxI32::new(10); borrow shared a as v { borrow unique a as w {} }",
        "E0312",
        "",
        0,
    ),
    "invalid_move_during_borrow": (
        box,
        "let a=BoxI32::new(10); borrow shared a as v { let b=a.move(); }",
        "E0312",
        "",
        0,
    ),
    "invalid_drop_during_borrow": (
        box,
        "let a=BoxI32::new(10); borrow shared a as v { a.drop(); }",
        "E0312",
        "",
        0,
    ),
    "invalid_heap_implicit_copy": (box, "let a=BoxI32::new(10); let b=a;", "E0302", "", 0),
    "invalid_heap_raw_field": (box, "let a=BoxI32::new(10); let p=a.ptr;", "E0500", "", 0),
    "invalid_public_rawptr": ("", "let p: RawPtr=0;", "E0500", "", 0),
    "invalid_public_ffi": ('extern "C" fn isac_rt_free(ptr: RawPtr);', "", "E0500", "", 0),
    "invalid_direct_runtime": ('@import "core.runtime"', "let p=isac_rt_alloc(4);", "E0500", "", 0),
    "invalid_custom_heap_impl": (
        box + "impl BoxI32 { fn bad(shared self) {} }",
        "",
        "E0500",
        "",
        0,
    ),
    "valid_buffer": (
        buf,
        "let b=Buffer::new(32); b.write(0,10); b.write(1,20); print(b.read(0)); print(b.read(1)); b.release();",
        None,
        "10\n20\n",
        1,
    ),
    "valid_buffer_release": (buf, "let b=Buffer::new(32); b.release(); b.drop();", None, "", 1),
    "invalid_buffer_after_release": (
        buf,
        "let b=Buffer::new(32); b.release(); print(b.read(0));",
        "E0401",
        "",
        0,
    ),
    "invalid_buffer_drop_before_final": (buf, "let b=Buffer::new(32);", "E0410", "", 0),
    "invalid_buffer_explicit_drop_ready": (buf, "let b=Buffer::new(32); b.drop();", "E0410", "", 0),
    "invalid_buffer_double_release": (
        buf,
        "let b=Buffer::new(32); b.release(); b.release();",
        "E0401",
        "",
        0,
    ),
    "invalid_buffer_branch": (
        buf,
        "let b=Buffer::new(32); if true { b.release(); }",
        "E0420",
        "",
        0,
    ),
    "valid_buffer_branch_release": (
        buf,
        "let b=Buffer::new(32); if true { b.release(); } else { b.release(); }",
        None,
        "",
        1,
    ),
    "valid_buffer_move_released": (
        buf,
        "let b=Buffer::new(32); b.release(); let c=b.move();",
        None,
        "",
        1,
    ),
    "valid_buffer_move_ready": (
        buf,
        "let b=Buffer::new(32); let c=b.move(); c.write(0,42); print(c.read(0)); c.release();",
        None,
        "42\n",
        1,
    ),
    "valid_buffer_borrow": (
        buf,
        "let b=Buffer::new(2); borrow unique b as w { w.write(1,65); } borrow shared b as r { print(r.read(1)); } b.release();",
        None,
        "65\n",
        1,
    ),
    "valid_buffer_zero": (
        buf,
        "let b=Buffer::new(0); print(b.len()); b.release();",
        None,
        "0\n",
        1,
    ),
    "valid_heap_discard": (box, "BoxI32::new(42);", None, "", 1),
    "valid_heap_branch_move": (
        box,
        "let a=BoxI32::new(10); if true { let b=a.move(); print(b.get()); } else { let c=a.move(); }",
        None,
        "10\n",
        1,
    ),
    "valid_heap_branch_drop": (
        box,
        "let a=BoxI32::new(10); if false { a.drop(); } else { a.drop(); }",
        None,
        "",
        1,
    ),
    "valid_heap_parameter": (
        box + "fn consume(a: BoxI32) { print(a.get()); }",
        "let a=BoxI32::new(30); consume(a.move()); consume(BoxI32::new(40));",
        None,
        "30\n40\n",
        2,
    ),
    "valid_early_return_drop": (
        box + "fn foo(cond: bool) { let x=BoxI32::new(10); if cond { return; } print(x.get()); }",
        "foo(true); foo(false);",
        None,
        "10\n",
        2,
    ),
    "valid_nested_return_drop": (
        box
        + "fn foo() { let x=BoxI32::new(10); borrow shared x as a { let y=BoxI32::new(20); if true { print(y.get()); return; } } }",
        "foo();",
        None,
        "20\n",
        2,
    ),
    "valid_buffer_early_return": (
        buf
        + "fn foo(cond: bool) { let b=Buffer::new(2); if cond { b.release(); return; } b.release(); }",
        "foo(true); foo(false);",
        None,
        "",
        2,
    ),
    "invalid_buffer_early_return": (
        buf + "fn foo(cond: bool) { let b=Buffer::new(2); if cond { return; } b.release(); }",
        "foo(true);",
        "E0410",
        "",
        0,
    ),
    "valid_usize": (
        '@import "std.io"\nfn half(n: usize) -> usize { return n / 2; }',
        "let n: usize=64; n=n+2; print_usize(half(n)); print(18446744073709551615usize > n); print(18446744073709551615usize + 1);",
        None,
        "33\n1\n0\n",
        0,
    ),
}
manifest = {}
for name, (prefix, body, code, out, count) in cases.items():
    (root / (name + ".isa")).write_text(
        prefix + "\nfn main() -> i32 {\n " + body + "\n return 0;\n}\n"
    )
    manifest[name] = {"code": code, "stdout": out, "allocations": count}
(root / "expectations.json").write_text(json.dumps(manifest, indent=2) + "\n")
