#pragma once

#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

typedef void (*PSXModVBlankCallback)(void);
typedef void (*PSXModActivationCallback)(void);

/*
 * Register a trusted, statically linked plugin implementation. Package
 * manifests select implementations by this stable id; archives never provide
 * native code or symbol names.
 */
int psx_mod_register_activation_plugin(const char* id,
                                       PSXModActivationCallback callback);
int psx_mod_register_vblank_plugin(const char* id,
                                   PSXModVBlankCallback callback);

/* Narrow guest services available to trusted plugin callbacks. */
int psx_mod_game_started(void);
uint8_t psx_mod_read_byte(uint32_t address);
void psx_mod_write_byte(uint32_t address, uint8_t value);

/*
 * Read the committed value of one of this package's declared options, as the
 * player left it in the launcher (or the manifest default when untouched).
 * Writes a NUL-terminated string into `out` and returns 1; returns 0 with
 * out[0] = '\0' when the plan is not committed, the ids do not resolve, or the
 * value does not fit — the caller then applies its own default rather than
 * treating an empty string as a selection.
 *
 * Why this exists: the manifest schema already carries typed, validated,
 * launcher-rendered, persisted options ([[option]] boolean/choice/integer), but
 * an activation callback takes no arguments and had no way to read them, so a
 * trusted plugin could only ever be an on/off switch. A parameterised feature
 * then had to be modelled as one feature per value — and `constraint` only
 * expresses ordered_integer WITHIN a feature, so those pseudo-features could
 * not even be made mutually exclusive. This closes that gap: one feature, one
 * option, the plugin reads what was chosen.
 *
 * Ids are passed explicitly because registration is by plugin id alone and the
 * callback carries no package/feature context.
 */
int psx_mod_option_value(const char* package_id, const char* feature_id,
                         const char* option_id, char* out, uint32_t out_size);

/*
 * Request a fixed host display aspect before renderer/window initialization.
 * Intended for activation callbacks that move a game's widescreen enhancement
 * out of generic Settings and into its mod catalog.
 */
int psx_mod_set_fixed_display_aspect(uint32_t numerator,
                                     uint32_t denominator);
/*
 * Request resize-driven widescreen, capped at the supplied maximum aspect.
 * The current fixed aspect continues to shape the initial game window, so a
 * plugin may select that first with psx_mod_set_fixed_display_aspect().
 */
int psx_mod_set_adaptive_display_aspect(uint32_t max_numerator,
                                        uint32_t max_denominator);
/*
 * Set the wall-clock cadence of simulated guest VBlanks. A value of zero
 * removes frontend pacing; 60 and higher request that many native guest
 * update opportunities per host second. This intentionally changes whole-
 * machine realtime speed and is for experimental game-owned frame-rate mods.
 */
int psx_mod_set_native_vblank_rate(uint32_t frames_per_second);

/*
 * Enable presentation-only frame interpolation while leaving guest VBlank,
 * game logic, timers, and audio at their stock cadence. The OpenGL presenter
 * blends between completed guest frames at the requested output rate.
 * A value of zero follows the measured host-display refresh rate.
 */
int psx_mod_set_frame_interpolation(uint32_t frames_per_second);
/*
 * Choose how the OpenGL presenter combines completed frames. Linear is the
 * legacy full-frame crossfade. Motion-adaptive retains interpolation for
 * small temporal changes but switches large changes cleanly to reduce the
 * double-image trails produced by moving objects.
 */
enum {
    PSX_MOD_FRAME_INTERPOLATION_LINEAR = 0,
    PSX_MOD_FRAME_INTERPOLATION_MOTION_ADAPTIVE = 1
};
int psx_mod_set_frame_interpolation_blend(uint32_t blend_mode);
int psx_mod_set_auto_skip_fmv(int enabled);

/*
 * Override one player's resolved controller presentation mode for this launch.
 * This is intentionally a trusted-plugin API, not a generic launcher setting:
 * games may hide Hybrid from their normal selector while offering it as an
 * explicit game-owned mod.
 */
enum {
    PSX_MOD_CONTROLLER_HYBRID = 0,
    PSX_MOD_CONTROLLER_ANALOG = 1,
    PSX_MOD_CONTROLLER_DIGITAL = 2
};
int psx_mod_set_controller_mode_override(uint32_t player,
                                         uint32_t controller_mode);

/*
 * Register a C plugin before main() on the compilers supported by the runtime.
 * The registry itself uses function-local initialization, so constructor order
 * between game sources and the framework is safe.
 */
#if defined(_MSC_VER)
#pragma section(".CRT$XCU", read)
#define PSX_MOD_CONSTRUCTOR(name)                                           \
    static void __cdecl name(void);                                        \
    __declspec(allocate(".CRT$XCU"))                                       \
    static void (__cdecl* name##_constructor)(void) = name;                \
    static void __cdecl name(void)
#elif defined(__GNUC__) || defined(__clang__)
#define PSX_MOD_CONSTRUCTOR(name)                                           \
    static void name(void) __attribute__((constructor));                    \
    static void name(void)
#else
#error "PSX mod plugin registration needs a supported constructor mechanism"
#endif

#ifdef __cplusplus
}
#endif
