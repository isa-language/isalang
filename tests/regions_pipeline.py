from pathlib import Path
import os
import subprocess
import sys
import tempfile

compiler, root = sys.argv[1:3]
root = Path(root)
flags = ["--sanitize"] if len(sys.argv) > 3 else []
environment = {
    **os.environ,
    "ASAN_OPTIONS": "detect_leaks=1:halt_on_error=1",
    "UBSAN_OPTIONS": "halt_on_error=1",
}
with tempfile.TemporaryDirectory() as directory:
    for fixture, expected in [
        ("valid_cfg_branch_last_use", "10\n20\n10\n30\n"),
        ("valid_cfg_shadowing", "1\n9\n"),
        ("valid_cfg_loop_last_use", "0\n1\n2\n2\n"),
        ("valid_cfg_loop_exit", "30\n10\n10\n30\n"),
        ("valid_return_parameter_ref", "10\n20\n"),
        ("valid_return_forward_ref", "hello\nhello!\n"),
        ("valid_return_slice_ref", "3\n20\n"),
        ("valid_return_struct_ref", "42\n42\n43\n"),
        ("valid_return_ref_stored", "stored\nstored!\n"),
        ("valid_return_projection_ref", "42\n10\n30\n"),
        ("valid_projection_local_ref", "42\n7\n20\n30\n"),
        ("valid_projection_forward_ref", "21\n22\n"),
        ("valid_return_option_ref", "hello\nhello!\n0\n0\n"),
        ("valid_return_result_ref", "result\n7\n0\n"),
        ("valid_return_generic_reference_payloads", "pair\npair\npair!\n"),
        ("valid_generic_method_reference", "42\n43\n2.5\n1\n42\n"),
        ("valid_method_reference_argument", "17\n"),
        ("valid_borrowed_container_parameter", "parameter\nparameter!\n0\n0\n"),
        ("valid_borrowed_result_parameter", "result\n7\nresult!\n"),
        ("valid_borrowed_enum_parameter", "left\nright\n0\n0\n"),
        ("valid_payload_region_roundtrip", "payload\npayload!\n0\n"),
        ("valid_payload_region_paths", "paths\npaths\npaths!\n0\n0\ndone\n"),
        ("valid_payload_region_branch", "branches\n0\nbranches!!\n"),
        ("valid_payload_region_variant_sets", "7\n7\n"),
        ("valid_payload_variant_set_join", "1\n3\n5\n7\n7\n7\n"),
        ("valid_multiple_return_origins", "left\nleft\nleft\nright\nleft\nleft!\nright!\n"),
        ("valid_multiple_payload_origins", "b\ndone\ndone\n"),
        ("valid_mixed_return_origins", "container\nparameter\n"),
        ("valid_stack_payload_regions", "42\n42\n43\n"),
        ("valid_stack_aggregate_payload", "42\n43\n43\n3\n30\n40\n"),
        ("valid_stack_payload_projection", "42\n1\n30\n40\n"),
        ("valid_stack_payload_variant_layout", "42\n42\n7\n7\n43\n"),
        ("valid_stack_payload_multiple_origins", "20\n30\n40\n"),
        ("valid_stack_payload_scalar_types", "-7\n1\n2.5\n1.5\n225\n18446744073709551615\n"),
    ]:
        source = root / "tests/references" / (fixture + ".isa")
        for opt in ["-O0", "-O2"]:
            binary = Path(directory) / "program"
            built = subprocess.run(
                [compiler, str(source), opt, *flags, "-o", str(binary)],
                capture_output=True,
                text=True,
            )
            assert built.returncode == 0, built.stderr
            result = subprocess.run([str(binary)], capture_output=True, text=True, env=environment)
            assert (result.returncode, result.stdout, result.stderr) == (0, expected, ""), result
    source = root / "tests/references/valid_cfg_branch_last_use.isa"
    for stage in ["regions", "core", "erased", "lir", "mlir", "llvm"]:
        result = subprocess.run(
            [compiler, str(source), "--emit-" + stage], capture_output=True, text=True
        )
        assert result.returncode == 0, result.stderr
        if stage in ["regions", "core"]:
            assert "loan #L" in result.stdout and "liveIn={" in result.stdout
        else:
            assert "loan #L" not in result.stdout and "liveIn={" not in result.stdout
        if stage == "core":
            assert "[CFG last-use]" in result.stdout

    source = root / "tests/references/valid_reference_reborrow.isa"
    result = subprocess.run(
        [compiler, str(source), "--emit-region-constraints"], capture_output=True, text=True
    )
    assert result.returncode == 0, result.stderr
    assert "reason=Reborrow" in result.stdout and " <= " in result.stdout
    assert "reason=BorrowCreation" in result.stdout and "reason=LiveCarrier" in result.stdout

    source = root / "tests/references/valid_return_parameter_ref.isa"
    for stage in ["hir", "core", "region-constraints", "erased", "lir", "mlir", "llvm"]:
        result = subprocess.run(
            [compiler, str(source), "--emit-" + stage], capture_output=True, text=True
        )
        assert result.returncode == 0, result.stderr
        if stage in ["hir", "core"]:
            assert "return.region" in result.stdout
        elif stage == "region-constraints":
            assert "reason=FunctionResult" in result.stdout
        else:
            assert (
                "return.region" not in result.stdout
                and "reason=FunctionResult" not in result.stdout
            )

    source = root / "tests/references/valid_return_projection_ref.isa"
    for stage, marker in [
        ("core", "place.field"),
        ("regions", "projection=Index"),
        ("erased", "subview"),
        ("mlir", "isac.subview"),
        ("std-mlir", "memref.subview"),
    ]:
        result = subprocess.run(
            [compiler, str(source), "--emit-" + stage], capture_output=True, text=True
        )
        assert result.returncode == 0, result.stderr
        assert marker in result.stdout, result.stdout
    source = Path(directory) / "bounds.isa"
    source.write_text(
        "fn at(v: &i32[], i: usize) -> &i32 { return &v[i]; } "
        "fn main() { let v: i32[2] = [1, 2]; let r = at(&v, 2); print(r); }"
    )
    for opt in ["-O0", "-O2"]:
        binary = Path(directory) / "bounds"
        built = subprocess.run(
            [compiler, str(source), opt, *flags, "-o", str(binary)], capture_output=True, text=True
        )
        assert built.returncode == 0, built.stderr
        result = subprocess.run([str(binary)], capture_output=True, text=True, env=environment)
        assert result.returncode != 0 and "bounds" in result.stderr, result
        assert "AddressSanitizer" not in result.stderr and "runtime error:" not in result.stderr, (
            result.stderr
        )

    source = root / "tests/references/valid_return_option_ref.isa"
    for stage in ["hir", "core", "erased", "lir", "mlir", "llvm"]:
        result = subprocess.run(
            [compiler, str(source), "--emit-" + stage], capture_output=True, text=True
        )
        assert result.returncode == 0, result.stderr
        if stage in ["hir", "core"]:
            assert "result.payload-regions" in result.stdout and "return.region" in result.stdout
        else:
            assert (
                "result.payload-regions" not in result.stdout
                and "return.region" not in result.stdout
            )

    source = root / "tests/references/valid_generic_method_reference.isa"
    for stage in ["hir", "core", "region-constraints", "erased", "lir", "mlir", "llvm"]:
        result = subprocess.run(
            [compiler, str(source), "--emit-" + stage], capture_output=True, text=True
        )
        assert result.returncode == 0, result.stderr
        if stage in ["hir", "core"]:
            assert "return.region" in result.stdout and "Holder<i32>" in result.stdout
        elif stage == "region-constraints":
            assert "reason=FunctionResult" in result.stdout
        else:
            assert (
                "return.region" not in result.stdout
                and "reason=FunctionResult" not in result.stdout
            )

    source = root / "tests/references/valid_borrowed_container_parameter.isa"
    for stage in ["hir", "core", "erased", "lir", "mlir", "llvm"]:
        result = subprocess.run(
            [compiler, str(source), "--emit-" + stage], capture_output=True, text=True
        )
        assert result.returncode == 0, result.stderr
        assert ("payload.region" in result.stdout) == (stage in ["hir", "core"])
    binary = Path(directory) / "counted-parameter"
    built = subprocess.run(
        [compiler, str(source), "-O2", *flags, "--runtime-test-counters", "-o", str(binary)],
        capture_output=True,
        text=True,
    )
    assert built.returncode == 0, built.stderr
    result = subprocess.run([str(binary)], capture_output=True, text=True, env=environment)
    assert result.returncode == 0 and "allocations=1 frees=1" in result.stderr, result

    source = root / "tests/references/valid_payload_region_paths.isa"
    for stage in ["hir", "core", "region-constraints", "erased", "lir", "mlir", "llvm"]:
        result = subprocess.run(
            [compiler, str(source), "--emit-" + stage], capture_output=True, text=True
        )
        assert result.returncode == 0, result.stderr
        if stage in ["hir", "core"]:
            assert (
                "return.payload-source" in result.stdout and "variant=#V1 field=1" in result.stdout
            )
            assert "result-bound" in result.stdout
        elif stage == "region-constraints":
            assert "reason=FunctionResult" in result.stdout
        else:
            assert (
                "return.payload-source" not in result.stdout
                and "payload.region" not in result.stdout
            )
    binary = Path(directory) / "counted-payload-roundtrip"
    source = root / "tests/references/valid_payload_region_roundtrip.isa"
    built = subprocess.run(
        [compiler, str(source), "-O2", *flags, "--runtime-test-counters", "-o", str(binary)],
        capture_output=True,
        text=True,
    )
    assert built.returncode == 0, built.stderr
    result = subprocess.run([str(binary)], capture_output=True, text=True, env=environment)
    assert result.returncode == 0 and "allocations=1 frees=1" in result.stderr, result

    source = root / "tests/references/valid_payload_region_variant_sets.isa"
    for stage in ["core", "erased", "lir", "mlir", "llvm"]:
        result = subprocess.run(
            [compiler, str(source), "--emit-" + stage], capture_output=True, text=True
        )
        assert result.returncode == 0, result.stderr
        if stage == "core":
            assert "possible-variants={#V1 #V2 }" in result.stdout
        else:
            assert "possible-variants" not in result.stdout

    source = root / "tests/references/valid_multiple_payload_origins.isa"
    for stage in ["hir", "core", "region-constraints", "erased", "lir", "mlir", "llvm"]:
        result = subprocess.run(
            [compiler, str(source), "--emit-" + stage], capture_output=True, text=True
        )
        assert result.returncode == 0, result.stderr
        if stage in ["hir", "core"]:
            assert result.stdout.count("return.payload-source") == 4, result.stdout
            assert result.stdout.count("  return.region ") == 4, result.stdout
        elif stage == "region-constraints":
            assert result.stdout.count("reason=FunctionResult") >= 4, result.stdout
        else:
            assert (
                "return.payload-source" not in result.stdout
                and "return.region" not in result.stdout
            )
    binary = Path(directory) / "counted-multiple-origins"
    built = subprocess.run(
        [compiler, str(source), "-O2", *flags, "--runtime-test-counters", "-o", str(binary)],
        capture_output=True,
        text=True,
    )
    assert built.returncode == 0, built.stderr
    result = subprocess.run([str(binary)], capture_output=True, text=True, env=environment)
    assert result.returncode == 0 and "allocations=2 frees=2" in result.stderr, result

    source = root / "tests/references/valid_stack_payload_projection.isa"
    for stage in ["core", "erased", "lir", "mlir", "std-mlir", "llvm"]:
        result = subprocess.run(
            [compiler, str(source), "--emit-" + stage], capture_output=True, text=True
        )
        assert result.returncode == 0, result.stderr
        if stage == "core":
            assert "stored-reference" in result.stdout and "return.region" in result.stdout
        else:
            assert "stored-reference" not in result.stdout and "return.region" not in result.stdout
        if stage in ["erased", "lir"]:
            assert "address.view" in result.stdout and " = address " in result.stdout
        if stage == "mlir":
            assert "isac.address" in result.stdout and "isac.address_view" in result.stdout
        if stage == "llvm":
            assert "unrealized_conversion_cast" not in result.stdout
    binary = Path(directory) / "counted-stack-payloads"
    built = subprocess.run(
        [compiler, str(source), "-O2", *flags, "--runtime-test-counters", "-o", str(binary)],
        capture_output=True,
        text=True,
    )
    assert built.returncode == 0, built.stderr
    result = subprocess.run([str(binary)], capture_output=True, text=True, env=environment)

    assert result.returncode == 0 and result.stderr == "", result

    bounds_source = Path(directory) / "stack-payload-bounds.isa"
    bounds_source.write_text(
        "fn item(values: &i32[], index: usize) -> Option<&i32> { return Option::Some(&values[index]); } fn main() { let a: i32[1] = [1]; let v = item(&a, 1); match v { Option::Some(r) => print(r), Option::None => {} } }"
    )
    for opt in ["-O0", "-O2"]:
        binary = Path(directory) / "stack-payload-bounds"
        built = subprocess.run(
            [compiler, str(bounds_source), opt, *flags, "-o", str(binary)],
            capture_output=True,
            text=True,
        )
        assert built.returncode == 0, built.stderr
        result = subprocess.run([str(binary)], capture_output=True, text=True, env=environment)
        assert result.returncode != 0 and "out of bounds" in result.stderr, result
        assert "AddressSanitizer" not in result.stderr and "runtime error:" not in result.stderr, (
            result
        )
