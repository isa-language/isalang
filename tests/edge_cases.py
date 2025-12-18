import subprocess
import tempfile
import sys
from pathlib import Path

compiler = sys.argv[1]
cases = [
    ("struct D { n: i32, } fn main() -> i32 { D::new(); return 0; }", None),
    ("impl Missing {} fn main() -> i32 { return 0; }", "E0102"),
    ("fn main() -> i32 { /*", "E0001"),
    ("fn main() -> i32 { return 0;", "E0002"),
    ("fn main() -> i32 { let a = 1; let a = 2; return 0; }", None),
    ("fn duplicated(a: i32, a: i32) {} fn main() {}", "E0101"),
    ("fn main() -> i32 { let a = 2147483648; return 0; }", "E0202"),
    (
        "protocol P { initial A; state A { next(shared self) -> B; } state B {} } fn main() -> i32 { return 0; }",
        "E0104",
    ),
    (
        "protocol P { initial A; state A { next(self) -> B; } } fn main() -> i32 { return 0; }",
        "E0104",
    ),
    (
        "protocol P { initial A; state A { next(self); } } struct D: P {} fn main() -> i32 { return 0; }",
        "E0105",
    ),
    (
        "struct D { n: i32, } fn main() -> i32 { let d=D::new(); if true { let e=d.move(); } return 0; }",
        "E0420",
    ),
    (
        "struct D { n: i32, } fn main() -> i32 { let d=D::new(); borrow shared d as v { borrow shared v as w { print(w.n); } } return 0; }",
        None,
    ),
    ("fn main() -> i32 { let b: bool[2]=[true,false]; b[1]=true; print(b[1]); return 0; }", None),
    (
        "fn sum(a: i32[2]) -> i32 { return a[0]+a[1]; } fn main() -> i32 { let a=[2,3]; print(sum(a.move())); return 0; }",
        None,
    ),
]
with tempfile.TemporaryDirectory() as d:
    path = Path(d) / "case.isa"
    for source, code in cases:
        path.write_text(source)
        r = subprocess.run([compiler, str(path), "--emit-llvm"], capture_output=True, text=True)
        if code:
            assert r.returncode == 1 and f"error[{code}]" in r.stderr, (source, r.stderr)
        else:
            assert r.returncode == 0, (source, r.stderr)
