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
