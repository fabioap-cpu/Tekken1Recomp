#!/usr/bin/env python3
"""Guard Hybrid mode's physical-controller detection in dev-any routing."""

from pathlib import Path


ROOT = Path(__file__).resolve().parents[2]
MAIN = (ROOT / "runtime" / "src" / "main.cpp").read_text(encoding="utf-8")

assert "hybrid_stick_active(const PlayerInput& p, bool dev_any)" in MAIN
assert "hybrid_dpad_active(const PlayerInput& p, int player, bool dev_any)" in MAIN
assert "hybrid_stick_active(p, dev_here)" in MAIN
assert "hybrid_dpad_active(p, player, dev_here)" in MAIN

stick_body = MAIN.split(
    "static bool hybrid_stick_active(const PlayerInput& p, bool dev_any)", 1
)[1].split("static bool hybrid_dpad_active", 1)[0]
dpad_body = MAIN.split(
    "static bool hybrid_dpad_active(const PlayerInput& p, int player, bool dev_any)",
    1,
)[1].split("/* Sample each player's live device state", 1)[0]

for name, body, detector in (
    ("stick", stick_body, "controller_stick_active(handle)"),
    ("D-pad", dpad_body, "controller_dpad_active(handle)"),
):
    assert "SDL_NumJoysticks()" in body, (
        f"Hybrid {name} detection must inspect all dev-any controllers"
    )
    assert "SDL_GameControllerFromInstanceID" in body
    assert detector in body

print("Hybrid dev-any physical-controller guard passed")
