"""Reproducible simulated-network profiles.

These map onto the binary's SSB64_NETPLAY_SIM_* env vars so a profile is
just data — the same dict feeds the e2e drivers and any future
in-process transport tests.
"""

from __future__ import annotations

from dataclasses import dataclass, asdict


@dataclass(frozen=True)
class NetworkProfile:
    latency_ms: int = 0
    jitter_ms: int = 0
    loss_pct: int = 0
    dup_pct: int = 0
    reorder_pct: int = 0
    reorder_window_ms: int = 0
    corrupt_pct: int = 0
    seed: int = 0

    def env(self) -> dict[str, str]:
        """Translate to the game's SSB64_NETPLAY_SIM_* environment."""
        return {
            "SSB64_NETPLAY_SIM_LATENCY_MS": str(self.latency_ms),
            "SSB64_NETPLAY_SIM_JITTER_MS": str(self.jitter_ms),
            "SSB64_NETPLAY_SIM_LOSS_PCT": str(self.loss_pct),
            "SSB64_NETPLAY_SIM_DUP_PCT": str(self.dup_pct),
            "SSB64_NETPLAY_SIM_REORDER_PCT": str(self.reorder_pct),
            "SSB64_NETPLAY_SIM_REORDER_WINDOW_MS": str(self.reorder_window_ms),
            "SSB64_NETPLAY_SIM_CORRUPT_PCT": str(self.corrupt_pct),
            "SSB64_NETPLAY_SIM_SEED": str(self.seed),
        }


PROFILES = {
    "perfect": NetworkProfile(),
    "wifi": NetworkProfile(latency_ms=35, jitter_ms=15, loss_pct=1),
    "adverse": NetworkProfile(latency_ms=90, jitter_ms=40, loss_pct=5,
                              dup_pct=1, reorder_pct=3),
}
