"""smash_test — shared helpers for the pytest suite.

Thin wrappers; the heavy netplay machinery lives in
tests/support/harness/netplay/session_runner.py (imported as `session_runner`
via the path shim below) so pytest tests and the standalone drivers
share one implementation.
"""

from __future__ import annotations

import sys
from pathlib import Path

ROOT = Path(__file__).resolve().parents[4]
_HARNESS = ROOT / "tests" / "support" / "harness" / "netplay"
if str(_HARNESS) not in sys.path:
    sys.path.insert(0, str(_HARNESS))

from .network import NetworkProfile, PROFILES  # noqa: F401,E402
from .processes import run_script, spawn  # noqa: F401,E402
