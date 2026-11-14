from pathlib import Path
import os
import re
import resource
import subprocess
import sys
import tempfile

compiler, root, api, *sanitized = sys.argv[1:]
root = Path(root)
flags = [
    "--frame-roundtrip",
    "--frame-heap",
    "--frame-outline",
    "--frame-state",
    "--frame-step-before=checkpoint",
]
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
    for name, output, prefix, ready_count, count, exit_code in [
        ("owned", "old\n202\nkeep!\nnext\n", "old\n", 5, 9, 0),
        ("containers", "left\n202\n2\nright\n", "left\n", 8, 15, 0),
        ("local_regions", "local\n202\nlocal!\n", "local\n", 3, 5, 0),
        ("scalar_result", "scalar\n202\n6\n", "scalar\n", 3, 5, 23),
        ("ssa", "101\n7\nssa\n8\n", "101\n", 3, 5, 0),
    ]:
        for opt in ["-O0", "-O2"]:
            for state in [None, "ready", "checkpoint", "completed"]:
                args = [
                    compiler,
                    root / "tests/frame_step" / (name + ".isa"),
                    *flags,
                    opt,
                    "--runtime-test-counters",
                    "-o",
                    binary,
                ]
                if sanitized:
                    args += ["--sanitize"]
                if state:
                    args += ["--frame-state-exit=" + state]
                built = command(args)
                assert built.returncode == 0, (name, state, built.stderr)
                run = command([binary], env=environment)
                ready_output = "101\n" if name == "ssa" else ""
                assert run.returncode == (0 if state else exit_code) and run.stdout == (
                    ready_output
                    if state == "ready"
                    else prefix
                    if state == "checkpoint"
                    else output
                ), (name, state, run)
                counters(run, ready_count if state == "ready" else count)

    for opt in ["O0", "O2"]:
        for mode in [
            "normal",
            "repeat",
            "loop",
            "cancel-checkpoint",
            "drop-checkpoint",
            "advance-canceled",
            "take-checkpoint",
            "join",
        ]:
            built = command(
                [api, root / "tests", mode, binary, opt, "sanitized" if sanitized else "normal"]
            )
            assert built.returncode == 0, (mode, built.stderr)
            arguments = [[], ["cancel"]] if mode == "join" else [[]]
            for args in arguments:
                run = command(
                    [binary, *args],
                    env=environment,
                    preexec_fn=lambda: resource.setrlimit(resource.RLIMIT_CORE, (0, 0)),
                )
                if mode == "take-checkpoint":
                    assert (
                        run.returncode != 0
                        and "isac runtime panic: frame result is not completed" in run.stderr
                    ), run
                    assert (
                        "AddressSanitizer" not in run.stderr and "runtime error:" not in run.stderr
                    ), run
                else:
                    returned = mode in ["normal", "repeat", "loop"] or (mode == "join" and not args)
                    expected = "old\n" + ("202\nreturned!\n" if returned else "canceled\n")
                    assert run.returncode == 0 and run.stdout == expected, (mode, run)
                    counters(
                        run, (13 if returned else 17) if mode == "join" else (9 if returned else 10)
                    )

    for opt in ["-O0", "-O2"]:
        args = [
            compiler,
            root / "tests/frame_dispatch/owned.isa",
            *flags[:-1],
            "--frame-step-before=marker",
            "--frame-destroy-if=stop",
            opt,
            "--runtime-test-counters",
            "-o",
            binary,
        ]
        if sanitized:
            args += ["--sanitize"]
        built = command(args)
        assert built.returncode == 0, built.stderr
        for selected in [False, True]:
            run = command([binary, *(["destroy"] if selected else [])], env=environment)
            assert run.returncode == 0 and run.stdout == (
                "101\n" if selected else "101\none\ntwo\none!\n"
            ), run
            counters(run, 14)

    for name in [
        "blocked_borrow",
        "blocked_partial",
        "blocked_loop",
        "blocked_duplicate",
        "blocked_protocol",
    ]:
        invalid = command(
            [
                compiler,
                root / "tests/frame_step" / (name + ".isa"),
                *flags,
                "--check",
                "--color=never",
            ]
        )
        code = "E0410" if name == "blocked_protocol" else "E8006"
        assert (
            invalid.returncode == 1
            and "error[" + code + "]" in invalid.stderr
            and not invalid.stdout
        ), (name, invalid)
    for options in [
        ["--frame-step-before=checkpoint"],
        [*flags[:-1], "--frame-state-exit=checkpoint"],
        [*flags[:-1], "--frame-step-before=unknown"],
        [*flags, "--frame-state-exit=checkpoint", "--frame-destroy-if=stop"],
    ]:
        invalid = command(
            [compiler, root / "tests/frame_step/owned.isa", *options, "--check", "--color=never"]
        )
        assert invalid.returncode == 1 and not invalid.stdout, invalid

    for stage in ["core", "regions", "region-constraints", "erased", "lir", "mlir", "llvm"]:
        report = command(
            [compiler, root / "tests/frame_step/owned.isa", *flags, "-O0", "--emit-" + stage]
        )
        assert report.returncode == 0, (stage, report.stderr)
        if stage == "core":
            assert re.search(r"enum\.construct .*variant=#V3", report.stdout)
            assert len(re.findall(r"fn #F\d+ __frame_continue_\d+ kind=Normal", report.stdout)) == 2
        if stage in ["erased", "lir", "mlir", "llvm"]:
            for proof in [
                "RegionId",
                "LoanId",
                "MemoryState",
                "BorrowProvenance",
                "Capture states:",
            ]:
                assert proof not in report.stdout, (stage, proof)
