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
    "--frame-project-fields",
]
environment = dict(
    os.environ,
    ASAN_OPTIONS="detect_leaks=1:halt_on_error=1:detect_stack_use_after_return=1",
    UBSAN_OPTIONS="halt_on_error=1",
)


def command(args, **options):
    return subprocess.run(
        list(map(str, args)), capture_output=True, text=True, timeout=90, **options
    )


def counters(run, count):
    assert run.stderr == f"ISAC_RT allocations={count} frees={count}\n", run


cases = [
    (
        "frame_projection/direct.isa",
        "first,second",
        "101\nleft\n7\n202\nnew\n7\nlater\n",
        7,
        16,
        11,
        "101\nleft\n7\n",
    ),
    ("frame_projection/nested_complete.isa", "first", "101\nnested\n", 5, 7, 7, None),
    ("frame_projection/ssa.isa", "first", "303\nargument\nright\n", 5, 9, 9, None),
    ("frame_step/blocked_partial.isa", "checkpoint", "left\nright\n", 5, 8, 8, None),
    ("frame_multiple/blocked_late_partial.isa", "first,second", "left\nright\n", 5, 11, 8, ""),
]
normal = ["101\nfirst\nreturned\n", "101\nfirst\n202\n1\nagain\n7\n"]
fallback = ["101\nfirst\n9\n"] * 2
modes = [
    ("normal", normal, [21, 28]),
    ("repeat", normal, [21, 28]),
    ("loop", normal, [21, 28]),
    ("cancel:1", ["9\n"] * 2, [21, 24]),
    ("cancel:2", fallback, [21, 28]),
    ("drop:2", fallback, [21, 28]),
    ("take:1", normal, [0, 0]),
    ("take:2", normal, [21, 0]),
    ("join", [fallback[0], normal[1]], [25, 34]),
]
with tempfile.TemporaryDirectory() as directory:
    binary = Path(directory) / "program"
    for opt in ["-O0", "-O2"]:
        for source, steps, output, ready, total, first, second in cases:
            for state in [
                None,
                "ready",
                "checkpoint",
                *(["checkpoint:2"] if second is not None else []),
                "completed",
            ]:
                args = [
                    compiler,
                    root / "tests" / source,
                    *flags,
                    "--frame-step-before=" + steps,
                    "--runtime-test-counters",
                    opt,
                    "-o",
                    binary,
                ]
                if state:
                    args += ["--frame-state-exit=" + state]
                if sanitized:
                    args += ["--sanitize"]
                built = command(args)
                assert built.returncode == 0, (source, state, built.stderr)
                run = command([binary], env=environment)
                expected = (
                    ""
                    if state == "ready"
                    else ("303\n" if source.endswith("ssa.isa") else "")
                    if state == "checkpoint"
                    else second
                    if state == "checkpoint:2"
                    else output
                )
                count = ready if state == "ready" else first if state == "checkpoint" else total
                assert run.returncode == 0 and run.stdout == expected, (source, state, run)
                counters(run, count)
        for mode, outputs, counts in modes:
            built = command(
                [api, root / "tests", mode, binary, opt[1:], "sanitized" if sanitized else "normal"]
            )
            assert built.returncode == 0, (mode, built.stderr)
            for n, args in enumerate([[], ["continue"]]):
                run = command(
                    [binary, *args],
                    env=environment,
                    preexec_fn=lambda: resource.setrlimit(resource.RLIMIT_CORE, (0, 0)),
                )
                if mode == "take:1" or (mode == "take:2" and n == 1):
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
    for source, step, code in [
        ("frame_projection/deep_partial.isa", "first", "E0310"),
        ("frame_step/blocked_borrow.isa", "checkpoint", "E8006"),
        ("frame_heap/blocked_protocol.isa", "marker", "E8006"),
    ]:
        run = command(
            [
                compiler,
                root / "tests" / source,
                *flags,
                "--frame-step-before=" + step,
                "--check",
                "--color=never",
            ]
        )
        assert run.returncode == 1 and code in run.stderr and not run.stdout, (source, run)
    run = command(
        [
            compiler,
            root / "tests/frame_projection/direct.isa",
            *flags[:-1],
            "--frame-step-before=first,second",
            "--check",
            "--color=never",
        ]
    )
    assert run.returncode == 1 and "E8006" in run.stderr and not run.stdout, run
    run = command(
        [compiler, root / "tests/frame_projection/direct.isa", "--frame-project-fields", "--check"]
    )
    assert run.returncode == 1 and not run.stdout, run
    for stage in ["core", "regions", "region-constraints", "erased", "lir", "mlir", "llvm"]:
        run = command(
            [
                compiler,
                root / "tests/frame_projection/direct.isa",
                *flags,
                "--frame-step-before=first,second",
                "--emit-" + stage,
                "-O0",
            ]
        )
        assert run.returncode == 0, (stage, run.stderr)
        if stage == "core":
            assert "$frame.projection." in run.stdout
        if stage in ["erased", "lir", "mlir", "llvm"]:
            for proof in ["RegionId", "LoanId", "MemoryState", "BorrowProvenance"]:
                assert proof not in run.stdout, (stage, proof)
