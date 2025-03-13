import subprocess
import sys
from pathlib import Path

compiler, case = sys.argv[1:3]
r = subprocess.run([compiler, case, "--check"], capture_output=True, text=True)
name = Path(case).stem
codes = {
    "invalid_use_after_move": "E0301",
    "invalid_double_drop": "E0301",
    "invalid_shared_write": "E0311",
    "invalid_two_unique": "E0312",
    "invalid_unique_and_shared": "E0312",
    "invalid_shared_and_unique": "E0312",
    "invalid_protocol_transition": "E0401",
    "invalid_protocol_drop_state": "E0410",
    "invalid_branch": "E0420",
    "invalid_implicit_copy": "E0302",
    "invalid_move_borrowed": "E0312",
    "invalid_owner_read_unique": "E0312",
    "invalid_alias_move": "E0310",
    "invalid_shared_self": "E0311",
    "invalid_method_capability": "E0105",
    "invalid_array_bounds": "E0203",
    "invalid_type": "E0200",
    "invalid_unknown_name": "E0102",
    "invalid_missing_return": "E0205",
    "invalid_resource_argument_state": "E0403",
    "invalid_early_return": "E0410",
    "invalid_borrow_transition": "E0310",
}
if name.startswith("invalid_"):
    assert r.returncode == 1 and f"error[{codes[name]}]" in r.stderr, (
        name,
        r.returncode,
        r.stdout,
        r.stderr,
    )
else:
    assert r.returncode == 0, (name, r.stdout, r.stderr)
