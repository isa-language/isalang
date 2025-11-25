from pathlib import Path
import json, os, subprocess, sys, tempfile

compiler, root, *sanitized = sys.argv[1:]
root = Path(root)
counts = {
    "valid_box_element_shared": 1,
    "valid_box_element_string": 3,
    "valid_box_element_array": 1,
    "valid_box_element_struct": 1,
    "valid_box_element_return_contract": 1,
    "valid_box_element_temporary": 3,
    "valid_vec_element_shared": 2,
    "valid_vec_element_string": 4,
    "valid_vec_element_first_last": 2,
    "valid_vec_element_missing": 2,
    "valid_vec_element_return_contract": 2,
    "valid_vec_element_array": 2,
    "valid_vec_element_nested_chain": 5,
    "valid_vec_element_loop": 2,
    "valid_box_protocol_final_storage": 1,
    "valid_vec_protocol_final_storage": 2,
    "valid_box_element_format": 5,
    "valid_box_element_format_temporary": 4,
    "valid_vec_element_format": 6,
    "valid_vec_protocol_nested_drop": 3,
    "valid_box_protocol_enum_drop": 1,
}
expectations = json.loads((root / "tests/containers/expectations.json").read_text())
source = root / "tests/containers/valid_vec_element_return_contract.isa"
for stage in ["hir", "core", "regions", "region-constraints", "erased", "lir", "mlir", "llvm"]:
    emitted = subprocess.run(
        [compiler, str(source), "--emit-" + stage], capture_output=True, text=True
    )
    assert emitted.returncode == 0, emitted.stderr
    if stage == "core":
        assert "container.call" in emitted.stdout and "vec.get" in emitted.stdout, emitted.stdout
    if stage in ["erased", "lir", "mlir", "llvm"]:
        assert "isac_rt_vec_at" in emitted.stdout, emitted.stdout
        for proof in ["RegionId", "LoanId", "container.call", "quantity="]:
            assert proof not in emitted.stdout, (stage, proof)
env = {
    **os.environ,
    "ASAN_OPTIONS": "detect_leaks=1:halt_on_error=1:detect_stack_use_after_return=1",
    "UBSAN_OPTIONS": "halt_on_error=1",
}
with tempfile.TemporaryDirectory() as directory:
    executable = Path(directory) / "program"
    for name, count in counts.items():
        source = root / "tests/containers" / (name + ".isa")
        expected = expectations[name]
        for optimization in ["-O0", "-O2"]:
            command = [
                compiler,
                str(source),
                optimization,
                "--runtime-test-counters",
                "-o",
                str(executable),
            ]
            if sanitized:
                command += ["--sanitize"]
            built = subprocess.run(command, capture_output=True, text=True)
            assert built.returncode == 0, (name, optimization, built.stderr)
            run = subprocess.run(
                [str(executable)],
                input=expected["stdin"],
                capture_output=True,
                text=True,
                env=env,
                timeout=15,
            )
            assert (run.returncode, run.stdout, run.stderr) == (
                0,
                expected["stdout"],
                f"ISAC_RT allocations={count} frees={count}\n",
            ), (name, optimization, run)

    command = [
        compiler,
        str(root / "examples/imports/main.isa"),
        "-O2",
        "--runtime-test-counters",
        "-o",
        str(executable),
    ]
    if sanitized:
        command += ["--sanitize"]
    built = subprocess.run(command, capture_output=True, text=True)
    assert built.returncode == 0, built.stderr
    run = subprocess.run([str(executable)], capture_output=True, text=True, env=env, timeout=15)
    assert (run.returncode, run.stdout, run.stderr) == (
        0,
        "value = 42\nIsalang: 2 elements\n",
        "ISAC_RT allocations=7 frees=7\n",
    ), run
