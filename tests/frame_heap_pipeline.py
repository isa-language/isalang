from pathlib import Path
import os
import re
import subprocess
import sys
import tempfile

compiler, root, *sanitized = sys.argv[1:]
root = Path(root)
cases = root / "tests/frame_heap"
environment = dict(
    os.environ,
    ASAN_OPTIONS="detect_leaks=1:halt_on_error=1:detect_stack_use_after_return=1",
    UBSAN_OPTIONS="halt_on_error=1",
)


def command(args, **options):
    return subprocess.run(
        list(map(str, args)), capture_output=True, text=True, timeout=60, **options
    )


def counters(stderr, expected):
    found = re.fullmatch(r"ISAC_RT allocations=(\d+) frees=(\d+)\n", stderr)
    assert (found and int(found[1]) == int(found[2]) == expected) or (
        expected == 0 and not stderr
    ), stderr


positive = [
    ("valid_owned", "one\ntwo\none!\n", 2, 5),
    ("valid_containers", "1\nbox\nbox!\n2\n", 9, 11),
    ("valid_nested", "nested\nnested!\ntail\n", 2, 5),
    ("valid_copy", "3\n-7\n65000\n2.5\n1\n225\n", 0, 1),
    ("valid_regions", "regions\nregions\nregions!\n", 1, 3),
    ("valid_branches", "branches\n", 1, 3),
]
assert {p.stem for p in cases.glob("valid_*.isa")} == {p[0] for p in positive}
flags = ["--frame-roundtrip", "--frame-heap"]
with tempfile.TemporaryDirectory() as directory:
    binary = Path(directory) / "program"

    def execute(source, opt, extra=()):
        args = [compiler, source, opt, "--runtime-test-counters", "-o", binary, *extra]
        if sanitized:
            args += ["--sanitize"]
        built = command(args)
        assert built.returncode == 0, (source, extra, built.stderr)
        run = command([binary], env=environment)
        assert run.returncode == 0, (source, extra, run)
        return run

    for name, output, base_count, heap_count in positive:
        source = cases / (name + ".isa")
        report = command([compiler, source, *flags, "--emit-core"])
        assert report.returncode == 0, (name, report.stderr)
        for token in [
            "$frame.heap",
            "Box<$frame.",
            "__frame_make_",
            "__frame_restore_",
            "__frame_destroy_",
            "box.generic.new",
            "box.generic.into_inner",
        ]:
            assert token in report.stdout, (name, token)

        for helper in ["make", "restore", "destroy"]:
            assert re.search(r"fn #F\d+ __frame_" + helper + r"_\d+ kind=Normal", report.stdout), (
                report.stdout
            )
        for opt in ["-O0", "-O2"]:
            base = execute(source, opt)
            counters(base.stderr, base_count)
            transformed = execute(source, opt, flags)
            counters(transformed.stderr, heap_count)
            assert base.stdout == transformed.stdout == output, (name, opt, base, transformed)

    for name, output, count, complete_count, prefixes in [
        ("valid_owned", "", 2, 5, 2),
        ("valid_nested", "", 2, 5, 1),
        ("valid_containers", "1\nbox\n", 8, 10, 3),
        ("valid_copy", "", 0, 1, 6),
    ]:
        source = cases / (name + ".isa")
        for prefix in range(prefixes + 1):
            extra = [*flags, "--frame-construction-exit=" + str(prefix)]
            report = command([compiler, source, *extra, "--emit-core"])
            assert report.returncode == 0, (name, prefix, report.stderr)
            main = re.search(
                r"fn #F\d+ main kind=.*?(?=\nfn #F\d+ __frame_make_)", report.stdout, re.S
            )[0]

            ids = {
                role: re.search(r"fn #F(\d+) __frame_" + role + "_", report.stdout)[1]
                for role in ["make", "restore", "destroy"]
            }
            for role in ["make", "destroy"]:
                assert bool(re.search(r"call #F" + ids[role] + r"\b", main)) == (
                    prefix == prefixes
                ), (name, prefix, role)
            assert not re.search(r"call #F" + ids["restore"] + r"\b", main), (name, prefix)
            for opt in ["-O0", "-O2"]:
                run = execute(source, opt, extra)
                assert run.stdout == output, (name, prefix, opt, run)
                counters(run.stderr, complete_count if prefix == prefixes else count)

    llvm = command([compiler, cases / "valid_nested.isa", *flags, "-O0", "--emit-llvm"])
    assert llvm.returncode == 0, llvm.stderr
    assert re.search(r"call .*@isa_\d+___frame_make_", llvm.stdout), llvm.stdout
    assert re.search(r"call .*@isa_\d+___frame_restore_", llvm.stdout), llvm.stdout
    assert "isac_rt_alloc" in llvm.stdout and "isac_rt_free" in llvm.stdout
    assert 'target triple = "x86_64-' in llvm.stdout and "target datalayout = " in llvm.stdout

for stage in ["erased", "lir", "mlir", "llvm"]:
    report = command([compiler, cases / "valid_owned.isa", *flags, "--emit-" + stage])
    assert report.returncode == 0, (stage, report.stderr)
    for proof in [
        "RegionId",
        "LoanId",
        "Capture states:",
        "Typed frame IR:",
        "MemoryState",
        "BorrowProvenance",
    ]:
        assert proof not in report.stdout, (stage, proof)
for name, code in [
    ("invalid_async", "E8006"),
    ("invalid_await", "E8006"),
    ("invalid_live_borrow", "E0312"),
]:
    report = command(
        [
            compiler,
            root / "tests/concurrency_foundations" / (name + ".isa"),
            *flags,
            "--emit-core",
            "--color=never",
        ]
    )
    assert (
        report.returncode == 1 and "error[" + code + "]" in report.stderr and not report.stdout
    ), (name, report)
invalid = command([compiler, cases / "valid_owned.isa", "--frame-heap", "--check"])
assert invalid.returncode == 1 and "requires --frame-roundtrip" in invalid.stderr, invalid
invalid = command(
    [compiler, root / "tests/frame_storage/valid_owned.isa", *flags, "--check", "--color=never"]
)
assert invalid.returncode == 1 and "E8006" in invalid.stderr, invalid
