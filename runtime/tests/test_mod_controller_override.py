#!/usr/bin/env python3
"""Guard the game-owned controller-mode override lifecycle."""

from pathlib import Path


ROOT = Path(__file__).resolve().parents[2]
MAIN = (ROOT / "runtime" / "src" / "main.cpp").read_text(encoding="utf-8")
HEADER = (ROOT / "runtime" / "include" / "mod_plugins.h").read_text(
    encoding="utf-8"
)

for symbol in (
    "PSX_MOD_CONTROLLER_HYBRID",
    "PSX_MOD_CONTROLLER_ANALOG",
    "PSX_MOD_CONTROLLER_DIGITAL",
    "psx_mod_set_controller_mode_override",
):
    assert symbol in HEADER, f"missing trusted-plugin controller API: {symbol}"

reset = """g_mod_controller_mode_override[0] = -1;
    g_mod_controller_mode_override[1] = -1;
    mod_runtime_activate_plugins();"""
apply = """if (g_mod_controller_mode_override[0] >= 0)
        p1_mode = g_mod_controller_mode_override[0];
    if (g_mod_controller_mode_override[1] >= 0)
        p2_mode = g_mod_controller_mode_override[1];"""

assert reset in MAIN, "controller overrides must reset before every activation pass"
assert apply in MAIN, "controller overrides must replace the resolved launch modes"
assert MAIN.index(reset) < MAIN.index(apply), "reset/activate must precede application"

print("mod controller override lifecycle guard passed")
