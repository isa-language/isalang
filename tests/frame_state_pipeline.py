from pathlib import Path
import os
import re
import resource
import subprocess
import sys
import tempfile

compiler, root, api, *sanitized = sys.argv[1:]
root = Path(root)
flags = ["--frame-roundtrip", "--frame-heap", "--frame-outline", "--frame-state"]
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


with tempfile.TemporaryDirectory() as directory:
    binary = Path(directory) / "program"

    def compile_source(source, opt, extra=()):
        args = [compiler, source, opt, "--runtime-test-counters", "-o", binary, *flags, *extra]
        if sanitized:
            args += ["--sanitize"]
        built = command(args)
        assert built.returncode == 0, (source, extra, built.stderr)

    for name, output, count in [
        ("valid_owned", "one\ntwo\none!\n", 5),
        ("valid_nested", "nested\nnested!\ntail\n", 5),
        ("valid_containers", "1\nbox\nbox!\n2\n", 11),
        ("valid_copy", "3\n-7\n65000\n2.5\n1\n225\n", 1),
        ("valid_regions", "regions\nregions\nregions!\n", 3),
        ("valid_branches", "branches\n", 3),
    ]:
        source = root / "tests/frame_heap" / (name + ".isa")
        for opt in ["-O0", "-O2"]:
            for exit_state in [None, "ready", "completed"]:
                compile_source(
                    source, opt, [] if exit_state is None else ["--frame-state-exit=" + exit_state]
                )
                run = command([binary], env=environment)
                ready_output = "1\nbox\n" if name == "valid_containers" else ""
                ready_count = 10 if name == "valid_containers" else count
                assert run.returncode == 0 and run.stdout == (
                    ready_output if exit_state == "ready" else output
                ), (name, exit_state, run)
                counters(run, ready_count if exit_state == "ready" else count)

    source = root / "tests/frame_dispatch/owned.isa"
    for opt in ["-O0", "-O2"]:
        compile_source(source, opt)
        run = command([binary, "argument"], env=environment)
        assert run.returncode == 17 and run.stdout == "101\none\ntwo\none!\n", run
        counters(run, 14)
        compile_source(source, opt, ["--frame-destroy-if=stop"])
        for exits in [False, True]:
            run = command([binary, *(["destroy"] if exits else [])], env=environment)
            assert run.returncode == 0 and run.stdout == (
                "101\n" if exits else "101\none\ntwo\none!\n"
            ), run
            counters(run, 14 if exits else 11)

    for opt in ["O0", "O2"]:
        for mode in [
            "normal",
            "repeat",
            "repeat-loop",
            "cancel-ready",
            "cancel-completed",
            "advance-canceled",
            "drop-completed",
            "take-ready",
            "take-canceled",
        ]:
            built = command(
                [api, root / "tests", mode, binary, opt, "sanitized" if sanitized else "normal"]
            )
            assert built.returncode == 0, (mode, built.stderr)
            run = command(
                [binary],
                env=environment,
                preexec_fn=lambda: resource.setrlimit(resource.RLIMIT_CORE, (0, 0)),
            )
            if mode.startswith("take-"):
                assert (
                    run.returncode != 0
                    and "isac runtime panic: frame result is not completed" in run.stderr
                ), run
                assert (
                    "AddressSanitizer" not in run.stderr and "runtime error:" not in run.stderr
                ), run
                assert not run.stdout, run
            else:
                returned = mode in ["normal", "repeat", "repeat-loop"]
                executed = returned or mode in ["cancel-completed", "drop-completed"]
                expected = ("404\n" if executed else "") + (
                    "returned!\n" if returned else "canceled\n"
                )
                assert run.returncode == 0 and run.stdout == expected, (mode, run)
                counters(run, 3 if returned else 4)

    for opt in ["O0", "O2"]:
        built = command(
            [api, root / "tests", "join", binary, opt, "sanitized" if sanitized else "normal"]
        )
        assert built.returncode == 0, built.stderr
        for args, output, count in [([], "404\nreturned!\n", 9), (["cancel"], "canceled\n", 13)]:
            run = command([binary, *args], env=environment)
            assert run.returncode == 0 and run.stdout == output, run
            counters(run, count)

    for stage in ["core", "regions", "erased", "lir", "mlir", "llvm"]:
        report = command(
            [compiler, root / "tests/frame_heap/valid_owned.isa", *flags, "-O0", "--emit-" + stage]
        )
        assert report.returncode == 0, (stage, report.stderr)
        if stage == "core":
            assert "$frame.state." in report.stdout
            for role in ["new", "advance", "cancel", "drop", "take"]:
                assert re.search(
                    r"fn #F\d+ __frame_state_" + role + r"_\d+ kind=Normal", report.stdout
                )
        if stage == "llvm":
            for role in ["new", "advance", "take"]:
                assert re.search(r"call .*@isa_\d+___frame_state_" + role + "_", report.stdout)
        if stage in ["erased", "lir", "mlir", "llvm"]:
            for proof in [
                "RegionId",
                "LoanId",
                "MemoryState",
                "BorrowProvenance",
                "Capture states:",
            ]:
                assert proof not in report.stdout, (stage, proof)
    for extra in [
        ["--frame-state"],
        ["--frame-roundtrip", "--frame-heap", "--frame-state"],
        [*flags, "--frame-construction-exit=0"],
        [*flags, "--frame-state-exit=other"],
        [*flags, "--frame-state-exit=ready", "--frame-destroy-if=stop"],
    ]:
        invalid = command([compiler, source, *extra, "--check", "--color=never"])
        assert invalid.returncode == 1 and not invalid.stdout, invalid
