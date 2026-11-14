from pathlib import Path
import os
import re
import subprocess
import sys
import tempfile

compiler, root, *sanitized = sys.argv[1:]
root = Path(root)
cases = root / "tests/frame_dispatch"
flags = ["--frame-roundtrip", "--frame-heap", "--frame-destroy-if=stop"]
environment = dict(
    os.environ,
    ASAN_OPTIONS="detect_leaks=1:halt_on_error=1:detect_stack_use_after_return=1",
    UBSAN_OPTIONS="halt_on_error=1",
)


def command(args, **options):
    return subprocess.run(
        list(map(str, args)), capture_output=True, text=True, timeout=60, **options
    )


def allocations(run):
    found = re.fullmatch(r"ISAC_RT allocations=(\d+) frees=(\d+)\n", run.stderr)
    assert found and found[1] == found[2], run
    return int(found[1])


with tempfile.TemporaryDirectory() as directory:
    binary = Path(directory) / "program"
    for name, output, overhead in [
        ("owned", "101\none\ntwo\none!\n", 3),
        ("containers", "101\n1\nbox\nnested\n", 2),
    ]:
        source = cases / (name + ".isa")
        report = command([compiler, source, *flags, "--emit-core"])
        assert report.returncode == 0, report.stderr
        ids = {
            role: re.search(r"fn #F(\d+) __frame_" + role + "_", report.stdout)[1]
            for role in ["restore", "destroy"]
        }

        assert re.search(
            r"if %\d+\n.*?call #F"
            + ids["destroy"]
            + r".*?return.*?else.*?call #F"
            + ids["restore"],
            report.stdout,
            re.S,
        ), report.stdout
        for opt in ["-O0", "-O2"]:
            baseline = {}
            for transformed in [False, True]:
                args = [compiler, source, opt, "--runtime-test-counters", "-o", binary]
                if transformed:
                    args += flags
                if sanitized:
                    args += ["--sanitize"]
                built = command(args)
                assert built.returncode == 0, (name, opt, built.stderr)
                for exits in [False, True]:
                    run = command([binary, *(["destroy"] if exits else [])], env=environment)
                    expected = 0 if transformed or not exits or name == "containers" else 17
                    assert run.returncode == expected, (name, opt, transformed, exits, run)
                    count = allocations(run)
                    if not transformed:
                        baseline[exits] = count
                        assert run.stdout == output + (
                            "17\n" if exits and name == "containers" else ""
                        ), run
                    else:
                        assert count == baseline[exits] + overhead, (name, exits, count, baseline)
                        assert run.stdout == ("101\n" if exits else output), run

                    assert run.stdout.count("101\n") == 1, run

    for stage in ["regions", "erased", "lir", "mlir", "llvm"]:
        report = command([compiler, cases / "owned.isa", *flags, "-O0", "--emit-" + stage])
        assert report.returncode == 0, (stage, report.stderr)
        if stage == "llvm":
            for role in ["make", "restore", "destroy"]:
                assert re.search(r"call .*@isa_\d+___frame_" + role + "_", report.stdout), (
                    report.stdout
                )
            assert "br i1" in report.stdout
        if stage != "regions":
            for proof in [
                "RegionId",
                "LoanId",
                "Capture states:",
                "Lifecycle CFG",
                "BorrowProvenance",
            ]:
                assert proof not in report.stdout, (stage, proof)

    for extra, expected in [
        (["--frame-destroy-if=stop"], "requires --frame-roundtrip"),
        (["--frame-roundtrip", "--frame-destroy-if=stop"], "requires --frame-roundtrip"),
        ([*flags, "--frame-construction-exit=0"], "excludes construction-prefix"),
        (["--frame-roundtrip", "--frame-heap", "--frame-destroy-if=unknown"], "live bool"),
        (["--frame-roundtrip", "--frame-heap", "--frame-destroy-if=one"], "live bool"),
    ]:
        invalid = command([compiler, cases / "owned.isa", *extra, "--check", "--color=never"])
        assert invalid.returncode == 1 and expected in invalid.stderr and not invalid.stdout, (
            invalid
        )
