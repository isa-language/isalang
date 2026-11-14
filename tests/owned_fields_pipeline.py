from pathlib import Path
import subprocess, sys, tempfile, os

compiler, root, *extra = sys.argv[1:]
source = Path(root) / "tests/containers/valid_owned_field_impl_cleanup.isa"
for stage in ["ast", "hir", "core", "erased", "lir", "mlir", "llvm"]:
    out = subprocess.run([compiler, str(source), "--emit-" + stage], capture_output=True, text=True)
    assert out.returncode == 0, out.stderr
    if stage == "core":
        assert "field.store" in out.stdout and "field.take" in out.stdout
    if stage in ["erased", "lir", "mlir", "llvm"]:
        assert "field.take" not in out.stdout
with tempfile.TemporaryDirectory() as directory:
    for opt in ["-O0", "-O2"]:
        exe = Path(directory) / ("program" + opt)
        args = [compiler, str(source), opt, "--runtime-test-counters", "-o", str(exe)]
        if extra:
            args += ["--sanitize"]
        built = subprocess.run(args, capture_output=True, text=True)
        assert built.returncode == 0, built.stderr
        run = subprocess.run(
            [str(exe)],
            capture_output=True,
            text=True,
            env={
                **os.environ,
                "ASAN_OPTIONS": "detect_leaks=1:halt_on_error=1",
                "UBSAN_OPTIONS": "halt_on_error=1",
            },
        )
        assert (run.returncode, run.stdout, run.stderr) == (
            0,
            "new\n",
            "ISAC_RT allocations=3 frees=3\n",
        ), run


for name, count, output in [
    ("valid_nested_owned_partial_store", 2, "second\n"),
    ("valid_ordinary_owned_partial_store", 2, "initialized\nagain\n"),
    ("valid_deep_owned_methods", 2, "42\n3\n"),
    ("valid_stack_owned_projection", 1, "2\n"),
    ("valid_stack_owned_unique_projection", 1, "3\n"),
    ("valid_deep_owned_shared", 1, "42\n42\n"),
    ("valid_deep_owned_unique", 1, "42!\n"),
    ("valid_deep_stack_method", 1, "3\n"),
    ("valid_deep_protocol_poststate", 1, "42\n"),
    ("valid_deep_protocol_local_restore", 1, ""),
    ("valid_deep_owned_reborrow", 1, "3\n"),
    ("valid_deep_scalar_projection", 1, "0\n"),
    ("valid_deep_generic_projection", 1, "8\n"),
    ("valid_field_poststate_release", 1, "42\n"),
    ("valid_field_poststate_impl", 1, ""),
    ("valid_field_poststate_forward", 1, ""),
    ("valid_field_poststate_early_return", 2, ""),
    ("valid_field_poststate_restore", 2, "4\n"),
    ("valid_field_poststate_two_parameters", 2, ""),
    ("valid_nested_owned_take", 2, "b\n"),
    ("valid_nested_owned_drop", 2, ""),
    ("valid_nested_owned_replace", 4, ""),
    ("valid_nested_owned_parameter", 2, "a\n"),
    ("valid_ordinary_owned_forward", 2, "second\n17\n2\n"),
    ("valid_ordinary_owned_backward", 1, "nested\n"),
    ("valid_ordinary_owned_replace", 2, ""),
    ("valid_ordinary_owned_deep", 2, "second\n"),
    ("valid_ordinary_owned_generic_field", 1, "generic\n19\n"),
    ("valid_ordinary_owned_generic_argument", 1, ""),
    ("valid_ordinary_owned_early_return", 1, ""),
    ("valid_protocol_field_transfer", 1, "42\n"),
    ("valid_protocol_field_released_drop", 1, ""),
    ("valid_protocol_field_replace", 2, ""),
    ("valid_protocol_field_nested", 1, ""),
    ("valid_protocol_field_parameter", 1, "3\n"),
    ("valid_protocol_field_nested_parameter", 1, ""),
    ("valid_protocol_field_early_return", 1, ""),
    ("valid_owned_projection_shared_unique", 1, "hello\nhello!\n"),
    ("valid_owned_projection_multiple_shared", 1, "hello\nhello\n"),
    ("valid_owned_projection_box_unique", 1, "20\n"),
    ("valid_owned_projection_after_last_use", 2, "old\nnew\n"),
    ("valid_owned_projection_view", 1, "view\nview!\n"),
    ("valid_owned_fields_enum_payload", 1, "payload\n"),
    ("valid_protocol_projection_methods", 2, "42\n6\n4\n20\n"),
    ("valid_owned_field_autoref_chain", 2, "42\n3\n"),
    ("valid_protocol_projection_preserving_function", 1, "10\n20\n"),
    ("valid_protocol_projection_last_use", 1, "1\n"),
    ("valid_protocol_projection_borrow_released", 1, ""),
]:
    source = Path(root) / "tests/containers" / (name + ".isa")
    with tempfile.TemporaryDirectory() as directory:
        for opt in ["-O0", "-O2"]:
            exe = Path(directory) / ("nested" + opt)
            args = [compiler, str(source), opt, "--runtime-test-counters", "-o", str(exe)]
            if extra:
                args += ["--sanitize"]
            built = subprocess.run(args, capture_output=True, text=True)
            assert built.returncode == 0, built.stderr
            run = subprocess.run(
                [str(exe)],
                capture_output=True,
                text=True,
                env={
                    **os.environ,
                    "ASAN_OPTIONS": "detect_leaks=1:halt_on_error=1",
                    "UBSAN_OPTIONS": "halt_on_error=1",
                },
            )
            assert (run.returncode, run.stdout, run.stderr) == (
                0,
                output,
                f"ISAC_RT allocations={count} frees={count}\n",
            ), run
