from pathlib import Path

root = Path(__file__).parent / "cases"
plain = "struct Data { value: i32, }\n"
proto = """protocol FileState {
 initial Closed; final Closed;
 state Closed { open(self) -> Open; }
 state Open { read(shared self); write(unique self); close(self) -> Closed; }
}
struct File: FileState { fd: i32, }
impl File {
 fn open(self) { self.fd = 7; }
 fn read(shared self) { print(self.fd); }
 fn write(unique self) { self.fd = 8; }
 fn close(self) { self.fd = 0; }
}
"""
cases = {
    "valid_move": (
        plain,
        "let data = Data::new(); let other = data.move(); other.value = 4; print(other.value);",
    ),
    "invalid_use_after_move": (
        plain,
        "let data = Data::new(); let other = data.move(); print(data.value);",
    ),
    "valid_drop": (plain, "let data = Data::new(); data.drop();"),
    "invalid_double_drop": (plain, "let data = Data::new(); data.drop(); data.drop();"),
    "valid_shared_borrow": (
        plain,
        "let data = Data::new(); borrow shared data as view { print(view.value); }",
    ),
    "valid_multiple_shared": (
        plain,
        "let data = Data::new(); borrow shared data as a { borrow shared data as b { print(a.value); print(b.value); } }",
    ),
    "invalid_shared_write": (
        plain,
        "let data = Data::new(); borrow shared data as view { view.value = 2; }",
    ),
    "valid_unique_borrow": (
        plain,
        "let data = Data::new(); borrow unique data as edit { edit.value = 20; } print(data.value);",
    ),
    "invalid_two_unique": (
        plain,
        "let data = Data::new(); borrow unique data as a { borrow unique data as b { b.value = 2; } }",
    ),
    "invalid_unique_and_shared": (
        plain,
        "let data = Data::new(); borrow unique data as a { borrow shared data as b { print(b.value); } }",
    ),
    "invalid_shared_and_unique": (
        plain,
        "let data = Data::new(); borrow shared data as a { borrow unique data as b { b.value = 2; } }",
    ),
    "valid_protocol": (proto, "let file = File::new(); file.open(); file.read(); file.close();"),
    "invalid_protocol_transition": (proto, "let file = File::new(); file.read();"),
    "invalid_protocol_drop_state": (proto, "let file = File::new(); file.open();"),
    "valid_copy_i32": ("", "let a: i32 = 10; let b = a; print(a + b);"),
    "valid_branch": (
        proto,
        "let file = File::new(); file.open(); if true { file.close(); } else { file.close(); }",
    ),
    "invalid_branch": (proto, "let file = File::new(); file.open(); if true { file.close(); }"),
    "invalid_implicit_copy": (plain, "let data = Data::new(); let other = data;"),
    "invalid_move_borrowed": (
        plain,
        "let data = Data::new(); borrow shared data as a { let other = data.move(); }",
    ),
    "invalid_owner_read_unique": (
        plain,
        "let data = Data::new(); borrow unique data as a { print(data.value); }",
    ),
    "invalid_alias_move": (
        plain,
        "let data = Data::new(); borrow shared data as a { let other = a.move(); }",
    ),
    "invalid_shared_self": (
        "struct Data { value: i32, } impl Data { fn bad(shared self) { self.value = 2; } }",
        "",
    ),
    "invalid_method_capability": (
        proto.replace("fn read(shared self)", "fn read(unique self)"),
        "",
    ),
    "valid_array": ("", "let a: i32[3] = [2, 3, 4]; a[1] = 8; let b = a.move(); print(b[1]);"),
    "invalid_array_bounds": ("", "let a = [2, 3]; print(a[2]);"),
    "invalid_type": ("", "let a: bool = 10;"),
    "invalid_unknown_name": ("", "print(missing);"),
    "invalid_missing_return": ("fn other() -> i32 { if true { return 1; } }", ""),
    "valid_call": ("fn twice(n: i32) -> i32 { return n * 2; }", "print(twice(21));"),
    "valid_resource_argument": (
        plain + "fn consume(d: Data) { print(d.value); }",
        "let d = Data::new(); consume(d.move());",
    ),
    "invalid_resource_argument_state": (
        proto + "fn consume(f: File) {}",
        "let f = File::new(); f.open(); consume(f.move());",
    ),
    "valid_early_return": (
        proto,
        "let f = File::new(); f.open(); if true { f.close(); return 0; } f.close();",
    ),
    "invalid_early_return": (
        proto,
        "let f = File::new(); f.open(); if true { return 0; } f.close();",
    ),
    "valid_protocol_move": (
        proto,
        "let f = File::new(); f.open(); let g = f.move(); g.read(); g.close();",
    ),
    "valid_borrow_protocol": (
        proto,
        "let f = File::new(); f.open(); borrow shared f as v { v.read(); } borrow unique f as w { w.write(); } f.close();",
    ),
    "invalid_borrow_transition": (
        proto,
        "let f = File::new(); f.open(); borrow unique f as v { v.close(); } f.close();",
    ),
    "valid_no_finals": (proto.replace("final Closed;", ""), "let f = File::new(); f.open();"),
    "valid_multiple_finals": (
        proto.replace("final Closed;", "final Closed; final Open;"),
        "let f = File::new(); f.open();",
    ),
}
for name, (prefix, body) in cases.items():
    (root / (name + ".isa")).write_text(
        prefix + "\nfn main() -> i32 {\n " + body + "\n return 0;\n}\n"
    )
