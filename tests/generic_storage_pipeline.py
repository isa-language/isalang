from pathlib import Path
import json, os, resource, subprocess, sys, tempfile

compiler, root, *extra = sys.argv[1:]
root = Path(root)
source = root / "examples/containers/generic_storage.isa"
for stage in ["ast", "hir", "core", "erased", "lir", "mlir", "llvm"]:
    result = subprocess.run(
        [compiler, str(source), "--emit-" + stage], capture_output=True, text=True
    )
    assert result.returncode == 0, result.stderr
    if stage == "hir":
        assert (
            "Vec<Box<String>>" in result.stdout and "DropGlue=Elements+Allocation" in result.stdout
        )
    if stage == "core":
        assert (
            "container.call" in result.stdout
            and "drop owned elements, then free storage" in result.stdout
        )
    if stage in ["erased", "lir", "mlir", "llvm"]:
        assert "container.call" not in result.stdout and "RegionId" not in result.stdout
        assert (
            "isac_rt_vec_push_slot" in result.stdout and "isac_rt_vec_drop_storage" in result.stdout
        )


counts = {
    "valid_box_generic_enum_drop": 3,
    "valid_box_generic_move": 3,
    "valid_box_generic_new": 1,
    "valid_box_generic_owned_struct": 3,
    "valid_box_generic_recursive_drop": 4,
    "valid_box_generic_scalar_array": 1,
    "valid_box_generic_shared_access": 1,
    "valid_box_generic_string_return": 3,
    "valid_box_generic_input_return": 3,
    "valid_vec_generic_clear_twice": 6,
    "valid_vec_generic_early_return": 10,
    "valid_vec_generic_enum_elements": 4,
    "valid_vec_generic_field_owner": 4,
    "valid_vec_generic_growth": 2,
    "valid_vec_generic_nested_drop": 7,
    "valid_vec_generic_new": 1,
    "valid_vec_generic_numeric_types": 7,
    "valid_vec_generic_owned_struct": 4,
    "valid_vec_generic_pop_empty": 1,
    "valid_vec_generic_string_return": 6,
    "valid_vec_generic_unique_parameter": 4,
    "valid_vec_generic_input_storage": 4,
}
expectations = json.loads((root / "tests/containers/expectations.json").read_text())
environment = {
    **os.environ,
    "ASAN_OPTIONS": "detect_leaks=1:halt_on_error=1:detect_stack_use_after_return=1",
    "UBSAN_OPTIONS": "halt_on_error=1",
}
with tempfile.TemporaryDirectory() as directory:
    for name, count in counts.items():
        case = root / "tests/containers" / (name + ".isa")
        expected = expectations[name]
        for optimization in ["-O0", "-O2"]:
            executable = Path(directory) / "program"
            command = [
                compiler,
                str(case),
                optimization,
                "--runtime-test-counters",
                "-o",
                str(executable),
            ]
            if extra:
                command += ["--sanitize"]
            built = subprocess.run(command, capture_output=True, text=True)
            assert built.returncode == 0, (name, optimization, built.stderr)
            run = subprocess.run(
                [str(executable)],
                input=expected["stdin"],
                capture_output=True,
                text=True,
                env=environment,
                timeout=15,
            )
            assert (run.returncode, run.stdout, run.stderr) == (
                0,
                expected["stdout"],
                f"ISAC_RT allocations={count} frees={count}\n",
            ), (name, optimization, run)
    executable = Path(directory) / "demo"
    command = [compiler, str(source), "-O2", "--runtime-test-counters", "-o", str(executable)]
    if extra:
        command += ["--sanitize"]
    built = subprocess.run(command, capture_output=True, text=True)
    assert built.returncode == 0, built.stderr
    run = subprocess.run([str(executable)], capture_output=True, text=True, env=environment)
    assert (run.returncode, run.stdout, run.stderr) == (
        0,
        "heap\nremaining = 1\n",
        "ISAC_RT allocations=10 frees=10\n",
    ), run

    failure = Path(directory) / "overflow.isa"
    failure.write_text(
        '@import "std.vec" fn main(){ let v=Vec<i32>::with_capacity(18446744073709551615usize); }'
    )
    command = [compiler, str(failure), "-o", str(executable)]
    if extra:
        command += ["--sanitize"]
    built = subprocess.run(command, capture_output=True, text=True)
    assert built.returncode == 0, built.stderr
    run = subprocess.run(
        [str(executable)],
        capture_output=True,
        text=True,
        env=environment,
        preexec_fn=lambda: resource.setrlimit(resource.RLIMIT_CORE, (0, 0)),
    )
    assert run.returncode != 0 and "isac runtime panic: vector capacity overflow" in run.stderr, run
