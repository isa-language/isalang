from pathlib import Path
import os
import re
import subprocess
import sys
import tempfile

compiler, root, *sanitized = sys.argv[1:]
root = Path(root)
cases = root / "tests/frame_storage"
environment = dict(
    os.environ,
    ASAN_OPTIONS="detect_leaks=1:halt_on_error=1:detect_stack_use_after_return=1",
    UBSAN_OPTIONS="halt_on_error=1",
)


def command(args, **options):
    return subprocess.run(
        list(map(str, args)), capture_output=True, text=True, timeout=60, **options
    )


positive = [
    ("valid_owned", "one\ntwo\none!\n", 2),
    ("valid_containers", "1\nbox\nbox!\n2\n", 9),
    ("valid_nested", "nested\nnested!\n", 2),
    ("valid_branches", "branches\n", 1),
    ("valid_copy", "3\n-7\n65000\n2.5\n1\n225\n", 0),
    ("valid_regions", "regions\nregions\nregions!\n", 1),
]
assert {p.stem for p in cases.glob("*.isa")} == {p[0] for p in positive}


def heap(stderr, count):
    found = re.fullmatch(r"ISAC_RT allocations=(\d+) frees=(\d+)\n", stderr)
    assert (found and int(found[1]) == int(found[2]) == count) or (count == 0 and not stderr), (
        stderr
    )


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

    for name, output, count in positive:
        source = cases / (name + ".isa")
        report = command([compiler, source, "--frame-roundtrip", "--emit-core"])
        assert report.returncode == 0, (name, report.stderr)
        assert "$frame.storage" in report.stdout and "$frame." in report.stdout
        if name != "valid_copy":
            assert "field.store" in report.stdout and "field.take" in report.stdout
        for opt in ["-O0", "-O2"]:
            base = execute(source, opt)
            heap(base.stderr, count)
            transformed = execute(source, opt, ["--frame-roundtrip"])
            heap(transformed.stderr, count)
            assert base.stdout == transformed.stdout == output, (name, opt, base, transformed)

    source = cases / "valid_owned.isa"
    report = command([compiler, source, "--frame-roundtrip", "--emit-core"])
    storage = re.search(r"bind #v(\d+)\(\$frame.storage\)", report.stdout)[1]
    pattern = r"field\.store field=#\d+[^\n]* #v" + storage + r"\(\$frame.storage\)"
    count = len(re.findall(pattern, report.stdout))
    assert count == 2, report.stdout
    for prefix in range(count + 1):
        for opt in ["-O0", "-O2"]:
            run = execute(
                source, opt, ["--frame-roundtrip", "--frame-construction-exit=" + str(prefix)]
            )
            assert run.stdout == "", (prefix, opt, run)
            heap(run.stderr, 2)
    for name, output, count in [("valid_nested", "", 2), ("valid_containers", "1\nbox\n", 8)]:
        source = cases / (name + ".isa")
        report = command([compiler, source, "--frame-roundtrip", "--emit-core"])
        storage = re.search(r"bind #v(\d+)\(\$frame.storage\)", report.stdout)[1]
        prefixes = len(
            re.findall(
                r"field\.store field=#\d+[^\n]* #v" + storage + r"\(\$frame.storage\)",
                report.stdout,
            )
        )
        assert prefixes >= 2, (name, prefixes)
        for prefix in range(prefixes + 1):
            for opt in ["-O0", "-O2"]:
                run = execute(
                    source, opt, ["--frame-roundtrip", "--frame-construction-exit=" + str(prefix)]
                )
                assert run.stdout == output, (name, prefix, opt, run)
                heap(run.stderr, count)

for stage in ["erased", "lir", "mlir", "llvm"]:
    report = command([compiler, cases / "valid_owned.isa", "--frame-roundtrip", "--emit-" + stage])
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
            "--frame-roundtrip",
            "--emit-core",
            "--color=never",
        ]
    )
    assert (
        report.returncode == 1 and "error[" + code + "]" in report.stderr and not report.stdout
    ), (name, report)
invalid = command(
    [
        compiler,
        cases / "valid_owned.isa",
        "--frame-roundtrip",
        "--frame-construction-exit=999999",
        "--check",
        "--color=never",
    ]
)
assert invalid.returncode == 1 and "E8006" in invalid.stderr, invalid
invalid = command([compiler, cases / "valid_owned.isa", "--frame-construction-exit=0", "--check"])
assert invalid.returncode == 1 and "requires --frame-roundtrip" in invalid.stderr, invalid
