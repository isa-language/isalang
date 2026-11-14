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
arguments = [[], ["a"], ["a", "b"], ["a", "b", "c"]]


def command(args, **options):
    return subprocess.run(
        list(map(str, args)), capture_output=True, text=True, timeout=90, **options
    )


def counters(run, count):
    assert run.stderr == f"ISAC_RT allocations={count} frees={count}\n", run


cli_output = ["start-one\n", "101\n2\n", "101\n202\nthird\n", "101\n202\n303\nthird\n0\n"]
cli_cases = [
    ("normal", cli_output, [9, 19, 26, 30]),
    ("ready", ["", "", "", ""], [9, 12, 15, 18]),
    ("checkpoint", ["start-one\n", "", "", ""], [9, 14, 17, 20]),
    ("checkpoint:2", ["start-one\n", "101\n2\n", "101\n", "101\n"], [9, 19, 23, 26]),
    (
        "checkpoint:3",
        ["start-one\n", "101\n2\n", "101\n202\nthird\n", "101\n202\n"],
        [9, 19, 26, 30],
    ),
    ("completed", cli_output, [9, 19, 26, 30]),
]
normal = ["owned-0\n", "101\n7\nowned-0-1\n", "101\n7\n202\n3\n", "101\n7\n202\n303\n404\n2\n7\n"]
cancel2 = ["9\n", "101\n7\n9\n", "101\n7\n9\n", "101\n7\n9\n"]
api_cases = [
    ("normal", normal, [9, 14, 24, 28]),
    ("repeat", normal, [9, 14, 24, 28]),
    ("loop", normal, [9, 14, 24, 28]),
    ("cancel:1", ["9\n"] * 4, [9, 14, 17, 20]),
    ("cancel:2", cancel2, [9, 14, 19, 22]),
    ("cancel:3", ["9\n", "101\n7\n9\n", "101\n7\n202\n9\n", "101\n7\n202\n9\n"], [9, 14, 24, 28]),
    ("drop:2", cancel2, [9, 14, 19, 22]),
    ("take:2", normal, [9, 14, 0, 0]),
    ("join", [*cancel2[:3], normal[3]], [13, 20, 27, 38]),
]

with tempfile.TemporaryDirectory() as directory:
    binary = Path(directory) / "program"
    for opt in ["-O0", "-O2"]:
        for state, outputs, counts in cli_cases:
            args = [
                compiler,
                root / "tests/frame_multiple/owned.isa",
                *flags,
                "--frame-step-before=first,second,third",
                "--runtime-test-counters",
                opt,
                "-o",
                binary,
            ]
            if state != "normal":
                args += ["--frame-state-exit=" + state]
            if sanitized:
                args += ["--sanitize"]
            built = command(args)
            assert built.returncode == 0, (state, built.stderr)
            for n, argv in enumerate(arguments):
                run = command([binary, *argv], env=environment)
                assert (
                    run.returncode == ([11, 12, 13, 17][n] if state == "normal" else 0)
                    and run.stdout == outputs[n]
                ), (state, n, run)
                counters(run, counts[n])
        for mode, outputs, counts in api_cases:
            built = command(
                [api, root / "tests", mode, binary, opt[1:], "sanitized" if sanitized else "normal"]
            )
            assert built.returncode == 0, (mode, built.stderr)
            for n, argv in enumerate(arguments):
                run = command(
                    [binary, *argv],
                    env=environment,
                    preexec_fn=lambda: resource.setrlimit(resource.RLIMIT_CORE, (0, 0)),
                )
                if mode == "take:2" and n >= 2:
                    assert (
                        run.returncode != 0
                        and "isac runtime panic: frame result is not completed" in run.stderr
                    ), run
                    assert (
                        "AddressSanitizer" not in run.stderr and "runtime error:" not in run.stderr
                    ), run
                else:
                    assert run.returncode == 0 and run.stdout == outputs[n], (mode, n, run)
                    counters(run, counts[n])

    for sequence in [
        "first,first",
        "second,first",
        "first,,second",
        "first,",
        ",first",
        "first,unknown",
        "first,third,second",
    ]:
        run = command(
            [
                compiler,
                root / "tests/frame_multiple/owned.isa",
                *flags,
                "--frame-step-before=" + sequence,
                "--check",
                "--color=never",
            ]
        )
        assert run.returncode == 1 and "E8006" in run.stderr and not run.stdout, (sequence, run)
    for index in ["0", "-1", "4", "99999999999999999999999999999", "x"]:
        run = command(
            [
                compiler,
                root / "tests/frame_multiple/owned.isa",
                *flags,
                "--frame-step-before=first,second,third",
                "--frame-state-exit=checkpoint:" + index,
                "--check",
            ]
        )
        assert run.returncode == 1 and not run.stdout, (index, run)
    for name in ["blocked_late_borrow", "blocked_late_partial"]:
        run = command(
            [
                compiler,
                root / "tests/frame_multiple" / (name + ".isa"),
                *flags,
                "--frame-step-before=first,second",
                "--check",
                "--color=never",
            ]
        )
        assert run.returncode == 1 and "E8006" in run.stderr and not run.stdout, (name, run)
    for stage in ["core", "regions", "region-constraints", "erased", "lir", "mlir", "llvm"]:
        run = command(
            [
                compiler,
                root / "tests/frame_multiple/owned.isa",
                *flags,
                "--frame-step-before=first,second,third",
                "--emit-" + stage,
                "-O0",
            ]
        )
        assert run.returncode == 0, (stage, run.stderr)
        if stage == "core":
            assert len(re.findall(r"fn #F\d+ __frame_segment_\d+ kind=Normal", run.stdout)) == 3
            assert "variant=#V5" in run.stdout
        if stage in ["erased", "lir", "mlir", "llvm"]:
            for proof in ["RegionId", "LoanId", "MemoryState", "BorrowProvenance"]:
                assert proof not in run.stdout, (stage, proof)
