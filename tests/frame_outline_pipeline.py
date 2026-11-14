from pathlib import Path
import os
import re
import subprocess
import sys
import tempfile

compiler, root, api, *sanitized = sys.argv[1:]
root = Path(root)
flags = ["--frame-roundtrip", "--frame-heap", "--frame-outline"]
environment = dict(
    os.environ,
    ASAN_OPTIONS="detect_leaks=1:halt_on_error=1:detect_stack_use_after_return=1",
    UBSAN_OPTIONS="halt_on_error=1",
)


def command(args, **options):
    return subprocess.run(
        list(map(str, args)), capture_output=True, text=True, timeout=60, **options
    )


def counters(run, expected):
    found = re.fullmatch(r"ISAC_RT allocations=(\d+) frees=(\d+)\n", run.stderr)
    assert found and int(found[1]) == int(found[2]) == expected, run


cases = [
    ("frame_heap/valid_owned", "one\ntwo\none!\n", 5),
    ("frame_heap/valid_nested", "nested\nnested!\ntail\n", 5),
    ("frame_heap/valid_containers", "1\nbox\nbox!\n2\n", 11),
    ("frame_heap/valid_copy", "3\n-7\n65000\n2.5\n1\n225\n", 1),
    ("frame_heap/valid_regions", "regions\nregions\nregions!\n", 3),
    ("frame_heap/valid_branches", "branches\n", 3),
    ("frame_outline/locals", "6\n5\nlocals\nlocals!\n", 3),
    ("frame_outline/ssa", "11\n22\nssa\n", 3),
]
with tempfile.TemporaryDirectory() as directory:
    binary = Path(directory) / "program"

    def compile_source(source, opt, extra=()):
        args = [compiler, source, opt, "--runtime-test-counters", "-o", binary, *flags, *extra]
        if sanitized:
            args += ["--sanitize"]
        built = command(args)
        assert built.returncode == 0, (source, opt, built.stderr)

    for name, output, count in cases:
        source = root / "tests" / (name + ".isa")
        core = command([compiler, source, *flags, "--emit-core"])
        assert core.returncode == 0, core.stderr
        assert re.search(r"fn #F\d+ __frame_continue_\d+ kind=Normal", core.stdout), core.stdout
        for opt in ["-O0", "-O2"]:
            compile_source(source, opt)
            run = command([binary], env=environment)
            assert run.returncode == 0 and run.stdout == output, (name, opt, run)
            counters(run, count)

    for opt in ["O0", "O2"]:
        built = command(
            [
                api,
                root / "tests",
                "--build-owned-result",
                binary,
                opt,
                "sanitized" if sanitized else "normal",
            ]
        )
        assert built.returncode == 0, built.stderr
        run = command([binary], env=environment)
        assert run.returncode == 0 and run.stdout == "returned!\n", run
        counters(run, 3)

    for name, output, count in [
        ("owned", "101\none\ntwo\none!\n", 11),
        ("containers", "101\n1\nbox\nnested\n", 16),
    ]:
        for opt in ["-O0", "-O2"]:
            compile_source(
                root / "tests/frame_dispatch" / (name + ".isa"), opt, ["--frame-destroy-if=stop"]
            )
            for exits in [False, True]:
                run = command([binary, *(["destroy"] if exits else [])], env=environment)
                assert run.returncode == 0 and run.stdout == ("101\n" if exits else output), (
                    name,
                    opt,
                    run,
                )
                counters(run, count + (3 if exits else 0))

    for stage in ["core", "regions", "erased", "lir", "mlir", "llvm"]:
        report = command(
            [
                compiler,
                root / "tests/frame_heap/valid_regions.isa",
                *flags,
                "-O0",
                "--emit-" + stage,
            ]
        )
        assert report.returncode == 0, (stage, report.stderr)
        if stage == "llvm":
            assert re.search(r"call .*@isa_\d+___frame_continue_", report.stdout), report.stdout
            assert re.search(r"call .*@isa_\d+___frame_restore_", report.stdout), report.stdout
        if stage in ["erased", "lir", "mlir", "llvm"]:
            for proof in [
                "RegionId",
                "LoanId",
                "Capture states:",
                "BorrowProvenance",
                "Lifecycle CFG",
            ]:
                assert proof not in report.stdout, (stage, proof)

    for extra in [
        ["--frame-outline"],
        ["--frame-roundtrip", "--frame-outline"],
        [*flags, "--frame-construction-exit=0"],
    ]:
        invalid = command(
            [
                compiler,
                root / "tests/frame_heap/valid_owned.isa",
                *extra,
                "--check",
                "--color=never",
            ]
        )
        assert invalid.returncode == 1 and "--frame-outline requires" in invalid.stderr, invalid
