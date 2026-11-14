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
    for fixture in ["complete", "result"]:
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
                    [
                        api,
                        root / "tests",
                        fixture + "-" + mode,
                        binary,
                        opt,
                        "sanitized" if sanitized else "normal",
                    ]
                )
                assert built.returncode == 0, (fixture, mode, built.stderr)
                for args in [[], ["continue"]]:
                    early = not args
                    run = command(
                        [binary, *args],
                        env=environment,
                        preexec_fn=lambda: resource.setrlimit(resource.RLIMIT_CORE, (0, 0)),
                    )
                    if mode == "take-checkpoint" and not early:
                        assert (
                            run.returncode != 0
                            and "isac runtime panic: frame result is not completed" in run.stderr
                        ), run
                        assert (
                            "AddressSanitizer" not in run.stderr
                            and "runtime error:" not in run.stderr
                        ), run
                        continue
                    returned = mode in ["normal", "repeat", "loop", "take-checkpoint"] or (
                        mode == "join" and early
                    )
                    if fixture == "complete":
                        prefix = "left\nreturned\n303\n" if early else "left\n"
                        suffix = (
                            ("returned-early\n" if early else "202\nreturned-late\n")
                            if returned
                            else "canceled\n"
                        )
                        count = (13 if early else 18) if returned else (14 if early else 19)
                        if mode == "join":
                            count = 17 if early else 25
                    else:
                        prefix = "303\n" if early else ""
                        suffix = ("result!\n" if early else "202\n7\n") if returned else "9\n"
                        count = 9 if early else 14
                        if mode == "join":
                            count = 13 if early else 20
                    assert run.returncode == 0 and run.stdout == prefix + suffix, (
                        fixture,
                        mode,
                        args,
                        run,
                    )
                    counters(run, count)

    for name in ["early_unit", "early_scalar", "early_dead_return"]:
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
                if state:
                    args += ["--frame-state-exit=" + state]
                if sanitized:
                    args += ["--sanitize"]
                built = command(args)
                assert built.returncode == 0, (name, state, built.stderr)
                for arguments in [[]] if name == "early_dead_return" else [[], ["continue"]]:
                    run = command([binary, *arguments], env=environment)
                    early = not arguments
                    if name == "early_unit":
                        output = (
                            ""
                            if state == "ready" or (state == "checkpoint" and not early)
                            else ("unit\n" if early else "202\nunit\n")
                        )
                        exit_code = 0
                        count = (9 if early else 12) if state == "ready" else (9 if early else 14)
                    elif name == "early_scalar":
                        output = (
                            ""
                            if state == "ready"
                            else "left\n"
                            if early or state == "checkpoint"
                            else "left\n202\nright\n"
                        )
                        exit_code = 0 if state else (17 if early else 23)
                        count = (7 if early else 8) if state == "ready" else (11 if early else 16)
                    else:
                        output = "" if state in ["ready", "checkpoint"] else "return\n"
                        exit_code = 0
                        count = 3 if state == "ready" else 5
                    assert run.returncode == exit_code and run.stdout == output, (
                        name,
                        state,
                        arguments,
                        run,
                    )
                    counters(run, count)

    for stage in ["core", "regions", "region-constraints", "erased", "lir", "mlir", "llvm"]:
        report = command(
            [compiler, root / "tests/frame_step/early_scalar.isa", *flags, "-O0", "--emit-" + stage]
        )
        assert report.returncode == 0, (stage, report.stderr)
        if stage == "core":
            assert "$frame.segment.result." in report.stdout
        if stage in ["erased", "lir", "mlir", "llvm"]:
            for proof in [
                "RegionId",
                "LoanId",
                "MemoryState",
                "BorrowProvenance",
                "Capture states:",
            ]:
                assert proof not in report.stdout, (stage, proof)
