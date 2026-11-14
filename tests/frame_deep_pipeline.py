from pathlib import Path
import os
import json
import re
import subprocess
import sys
import tempfile

compiler, root, *sanitized = sys.argv[1:]
root = Path(root)
flags = [
    "--frame-roundtrip",
    "--frame-heap",
    "--frame-outline",
    "--frame-state",
    "--frame-project-fields",
    "--frame-step-before=first,second",
]
environment = dict(
    os.environ,
    ASAN_OPTIONS="detect_leaks=1:halt_on_error=1:detect_stack_use_after_return=1",
    UBSAN_OPTIONS="halt_on_error=1",
)


def command(args, **kw):
    return subprocess.run(list(map(str, args)), capture_output=True, text=True, timeout=90, **kw)


outputs = {
    "recursive": "101\nleft\nright\n7\n202\nagain\n9\nlater\n",
    "empty_child": "101\n8\n202\nrestored\ntail\n",
    "early": "101\nright\n",
    "borrow_restore": "old\nupdated\nother\n",
    "partial_extract": "right\nnew\nright\n",
    "loop_restore": "right\nright\nright\nnext\n",
    "partial_drop": "new-left\nnew-right\ntail\n",
}
first_output = {
    "recursive": "101\nleft\nright\n7\n",
    "empty_child": "101\n8\n",
    "early": outputs["early"],
}
counts = {}
with tempfile.TemporaryDirectory() as directory:
    binary = Path(directory) / "program"
    for opt in ["-O0", "-O2"]:
        for source, output in outputs.items():
            framed = source in first_output
            modes = (
                ["plain", None, "ready", "checkpoint", "checkpoint:2", "completed"]
                if framed
                else [None]
            )
            for mode in modes:
                args = [
                    compiler,
                    root / "tests/frame_deep" / (source + ".isa"),
                    opt,
                    "--runtime-test-counters",
                    "-o",
                    binary,
                ]
                if framed and mode != "plain":
                    args += flags
                    if mode:
                        args += ["--frame-state-exit=" + mode]
                if sanitized:
                    args += ["--sanitize"]
                built = command(args)
                assert built.returncode == 0, (source, mode, built.stderr)
                run = command([binary], env=environment)
                expected = (
                    ""
                    if mode in ["ready", "checkpoint"]
                    else first_output[source]
                    if mode == "checkpoint:2"
                    else output
                )
                assert run.returncode == 0 and run.stdout == expected, (source, mode, run)
                count = re.fullmatch(r"ISAC_RT allocations=(\d+) frees=(\d+)\n", run.stderr)
                assert count and count[1] == count[2], (source, mode, run)
                key = source + ":" + str(mode)
                if key in counts:
                    assert counts[key] == int(count[1]), (opt, key, count)
                counts[key] = int(count[1])
    prelude = """@import "std.box"
struct Inner { count:i32, left:String, right:String, }
struct Outer { inner:Inner, tail:String, }
fn main() {
    let data = Outer{inner:Inner{count:7,left:String::from("left"),right:String::from("right")},tail:String::from("tail")};
"""
    bad = [
        "data.inner.left.drop(); data.inner.left.drop();",
        "data.inner.left.drop(); let copy=data.move();",
        "data.inner.left.drop(); let copy=&data;",
        "data.inner.left.drop(); let copy=&data.inner;",
        "data.inner.drop(); print(data.inner.count);",
        "data.inner.drop(); data.inner.count=9;",
        "data.inner.drop(); data.inner.left=String::new();",
        "data.inner.drop(); let part=data.inner.left.move();",
        "let r=&data.inner.right; data.inner.right.drop(); print(r);",
        "data.inner.left.drop(); let r=&data.inner.right; data.inner.right.drop(); print(r);",
        "let edit=&mut data; edit.inner.left.drop(); print(edit.inner.right.as_str());",
        "data.inner.left.drop(); let hidden=Option::Some(data.inner.move());",
        "data.inner=Inner::new(); print(data.inner.right.as_str());",
        "data.inner=Inner::new(); let whole=data.move();",
        "data.inner=Inner::new(); let payload=Option::Some(data.inner.move());",
        "data.inner=Inner::new(); let view=&mut data;",
    ]
    source = Path(directory) / "negative.isa"
    codes = ["E0301"] * 8 + ["E0312"] * 2 + ["E0310", "E0301"] + ["E0301"] * 4
    for fragment, code in zip(bad, codes, strict=True):
        source.write_text(prelude + fragment + "\n}")
        run = command([compiler, source, "--check", "--color=never"])
        assert run.returncode == 1 and not run.stdout and f"error[{code}]" in run.stderr, (
            fragment,
            run,
        )
        assert "E9001" not in run.stderr, (fragment, run.stderr)

    for fixture, step in [
        ("frame_step/blocked_borrow.isa", "checkpoint"),
        ("frame_projection/deep_partial.isa", "first"),
    ]:
        run = command(
            [
                compiler,
                root / "tests" / fixture,
                *flags[:-1],
                "--frame-step-before=" + step,
                "--check",
                "--color=never",
            ]
        )
        assert run.returncode == 1 and not run.stdout, (fixture, run)
    for name in ["invalid_async", "invalid_await", "invalid_postfix_await"]:
        run = command(
            [
                compiler,
                root / "tests/concurrency_foundations" / (name + ".isa"),
                "--check",
                "--color=never",
            ]
        )
        assert run.returncode == 1 and "E8006" in run.stderr and not run.stdout, (name, run)
    for stage in ["core", "regions", "region-constraints", "erased", "lir", "mlir", "llvm"]:
        run = command(
            [compiler, root / "tests/frame_deep/recursive.isa", *flags, "--emit-" + stage, "-O0"]
        )
        assert run.returncode == 0, (stage, run.stderr)
        if stage == "core":
            assert "ancestor=#1 ancestor=#1 field=#" in run.stdout
        if stage in ["erased", "lir", "mlir", "llvm"]:
            for proof in ["RegionId", "LoanId", "MemoryState", "BorrowProvenance"]:
                assert proof not in run.stdout, (stage, proof)
print(json.dumps({"native_runs": 44, "balanced_allocations": counts}, sort_keys=True))
