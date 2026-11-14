from pathlib import Path
import json
import os
import re
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


def command(args, **kw):
    return subprocess.run(list(map(str, args)), capture_output=True, text=True, timeout=90, **kw)


cases = {
    "then": (
        ["100\nbranch\n200\nbranch\nroot\ntail\n2\n", "100\n300\nroot\ntail\n2\n"],
        ["100\nbranch\n", "100\n300\nroot\ntail\n2\n"],
        [0, 0],
    ),
    "otherwise": (["300\nroot!\n", "else\n200\nelse\nroot!\n"], ["300\nroot!\n", "else\n"], [0, 0]),
    "nested": (
        [
            "10\n20\ninner\n200\ninner\nmiddle\nbranch-tail\nroot\n",
            "10\n20\n30\nmiddle\nbranch-tail\nroot\n",
            "10\n40\nroot\n",
        ],
        ["10\n20\ninner\n", "10\n20\n30\nmiddle\nbranch-tail\nroot\n", "10\n40\nroot\n"],
        [0, 0, 0],
    ),
    "partial": (
        ["200\nagain\nsecond\n", "300\nfirst\nsecond\n"],
        ["", "300\nfirst\nsecond\n"],
        [0, 0],
    ),
    "argument": (["400\nargument\nroot\n", "300\nroot\n"], ["400\n", "300\nroot\n"], [0, 0]),
    "multiple": (["100\n200\nroot\n", "100\n300\nroot\n"], ["100\n", "100\n300\nroot\n"], [0, 0]),
    "final_returns": (["root\n", "200\nroot\n"], ["root\n", ""], [7, 9]),
}
counts = {}
runs = 0
with tempfile.TemporaryDirectory() as directory:
    binary = Path(directory) / "program"
    for opt in ["-O0", "-O2"]:
        for name, (outputs, stopped, codes) in cases.items():
            source = root / "tests/frame_branch" / (name + ".isa")
            sequence = "first,checkpoint" if name == "multiple" else "checkpoint"
            for mode in ["plain", "normal", "checkpoint", "completed"]:
                args = [compiler, source, opt, "--runtime-test-counters", "-o", binary]
                if mode != "plain":
                    args += flags + ["--frame-step-before=" + sequence]
                    if mode == "checkpoint":
                        args += [
                            "--frame-state-exit=checkpoint" + (":2" if name == "multiple" else "")
                        ]
                    if mode == "completed":
                        args += ["--frame-state-exit=completed"]
                if sanitized:
                    args += ["--sanitize"]
                built = command(args)
                assert built.returncode == 0, (name, mode, built.stderr)
                for n, output in enumerate(outputs):
                    run = command([binary, *(["arg"] * n)], env=environment)
                    runs += 1
                    expected = stopped[n] if mode == "checkpoint" else output
                    code = codes[n] if mode in ["plain", "normal"] else 0
                    assert run.returncode == code and run.stdout == expected, (name, mode, n, run)
                    allocation = re.fullmatch(
                        r"ISAC_RT allocations=(\d+) frees=(\d+)\n", run.stderr
                    )
                    assert allocation and allocation[1] == allocation[2], (name, mode, n, run)
                    key = f"{name}:{mode}:{n}"
                    if key in counts:
                        assert counts[key] == int(allocation[1]), (key, opt, run)
                    counts[key] = int(allocation[1])
        for name in ["owned_result", "rebind"]:
            normal = (
                ["branch\n200\ntail\nowned!\n", "300\ntail\nowned!\n"]
                if name == "owned_result"
                else ["200\nlate\n", "300\nlate\n"]
            )
            canceled = (
                ["branch\n9\n", "300\ntail\n9\n"] if name == "owned_result" else ["", "300\nlate\n"]
            )
            for mode in ["normal", "repeat", "cancel"]:
                built = command(
                    [
                        api,
                        root / "tests",
                        name,
                        mode,
                        binary,
                        opt[1:],
                        "sanitized" if sanitized else "normal",
                    ]
                )
                assert built.returncode == 0, (name, mode, built.stderr)
                for n in [0, 1]:
                    run = command([binary, *(["arg"] * n)], env=environment)
                    runs += 1
                    assert run.returncode == 0 and run.stdout == (
                        canceled[n] if mode == "cancel" else normal[n]
                    ), (name, mode, n, run)
                    allocation = re.fullmatch(
                        r"ISAC_RT allocations=(\d+) frees=(\d+)\n", run.stderr
                    )
                    assert allocation and allocation[1] == allocation[2], (name, mode, n, run)
                    key = f"{name}:{mode}:{n}"
                    if key in counts:
                        assert counts[key] == int(allocation[1]), (key, opt, run)
                    counts[key] = int(allocation[1])

    run = command(
        [
            compiler,
            root / "tests/frame_step/blocked_nested.isa",
            *flags,
            "--frame-step-before=checkpoint",
            "--check",
        ]
    )
    assert run.returncode == 0, run.stderr

    for source, sequence in [
        ("frame_branch/multiple.isa", "checkpoint,first"),
        ("frame_branch/multiple.isa", "checkpoint,checkpoint"),
        ("frame_branch/partial.isa", "checkpoint"),
        ("frame_step/blocked_loop.isa", "checkpoint"),
        ("frame_step/blocked_duplicate.isa", "checkpoint"),
        ("frame_step/blocked_borrow.isa", "checkpoint"),
    ]:
        opts = flags[:-1] if source.endswith("partial.isa") else flags
        run = command(
            [
                compiler,
                root / "tests" / source,
                *opts,
                "--frame-step-before=" + sequence,
                "--check",
                "--color=never",
            ]
        )
        assert run.returncode == 1 and "E8006" in run.stderr and not run.stdout, (
            source,
            sequence,
            run,
        )
    rejected = Path(directory) / "unsupported.isa"
    for statement in [
        "unsafe { checkpoint(); }",
        "loop { if true { checkpoint(); } break; }",
        "if true { checkpoint(); } checkpoint();",
    ]:
        rejected.write_text(
            '@import "std.box"\nfn checkpoint() {}\nfn main(){let text=String::from("x");'
            + statement
            + "print(text);}"
        )
        run = command(
            [
                compiler,
                rejected,
                *flags,
                "--frame-step-before=checkpoint",
                "--check",
                "--color=never",
            ]
        )
        assert run.returncode == 1 and "E8006" in run.stderr and not run.stdout, (statement, run)
    for stage in ["core", "regions", "region-constraints", "erased", "lir", "mlir", "llvm"]:
        run = command(
            [
                compiler,
                root / "tests/frame_branch/nested.isa",
                *flags,
                "--frame-step-before=checkpoint",
                "--emit-" + stage,
                "-O0",
            ]
        )
        assert run.returncode == 0, (stage, run.stderr)
        if stage == "core":
            assert "__frame_segment_" in run.stdout
        if stage in ["erased", "lir", "mlir", "llvm"]:
            for proof in ["RegionId", "LoanId", "MemoryState", "BorrowProvenance"]:
                assert proof not in run.stdout, (stage, proof)
print(json.dumps({"native_runs": runs, "balanced_allocations": counts}, sort_keys=True))
