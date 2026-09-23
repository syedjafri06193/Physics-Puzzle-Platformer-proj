// Input state, and the packing that makes replays possible (doc 14.3).
//
// The replay format stores inputs, never state. That means the packed
// representation *is* the contract: if the number of quantization steps
// changes, every recorded replay decodes to slightly different stick
// positions and stops reproducing. So the quantization lives here, next to
// the packing, and `INPUT_FORMAT_VERSION` is written into every replay.

#pragma once

#include <cmath>
#include <cstdint>

namespace ppp {

/// Bumped whenever the packing changes. Replays store it; a mismatch is
/// reported, never silently decoded with the current layout.
inline constexpr int INPUT_FORMAT_VERSION = 1;

/// Analog axes are quantized before recording so playback is exactly
/// reproducible rather than nearly so (doc 14.3). 32 steps either side of
/// centre is finer than a player can hold a stick and coarse enough that
/// the quantized value survives being written to disk and read back.
inline constexpr int AXIS_STEPS = 32;

/// Deadzone applied radially, with rescaling, before quantization (doc 7.2).
inline constexpr float STICK_DEADZONE = 0.2f;

enum InputButton : uint8_t {
    kButtonJump = 1u << 0,
    kButtonGrab = 1u << 1,
    kButtonRestart = 1u << 2,
};

struct InputState {
    /// Horizontal movement, already deadzoned and rescaled, in [-1, 1].
    float move_x = 0.0f;
    bool jump = false;
    bool grab = false;
    bool restart = false;

    static InputState Empty() { return InputState{}; }

    /// Two bytes per tick, not the design document's one.
    ///
    /// The document says "one byte of button bits per tick" and, in the same
    /// paragraph, "axes quantized" — which does not fit in the same byte
    /// alongside the buttons. Two bytes at 120 Hz is 43 KB for a
    /// three-minute level, still small enough that the argument the document
    /// is making (inputs are tiny, state is megabytes) holds unchanged.
    uint16_t Pack() const {
        int q = static_cast<int>(move_x * static_cast<float>(AXIS_STEPS) +
                                 (move_x >= 0.0f ? 0.5f : -0.5f));
        if (q > AXIS_STEPS) q = AXIS_STEPS;
        if (q < -AXIS_STEPS) q = -AXIS_STEPS;

        uint8_t bits = 0;
        if (jump) bits |= kButtonJump;
        if (grab) bits |= kButtonGrab;
        if (restart) bits |= kButtonRestart;

        const uint8_t axis = static_cast<uint8_t>(q + 128);
        return static_cast<uint16_t>(axis) | (static_cast<uint16_t>(bits) << 8);
    }

    static InputState Unpack(uint16_t packed) {
        InputState in;
        const int q = static_cast<int>(packed & 0xFFu) - 128;
        in.move_x = static_cast<float>(q) / static_cast<float>(AXIS_STEPS);
        const uint8_t bits = static_cast<uint8_t>(packed >> 8);
        in.jump = (bits & kButtonJump) != 0;
        in.grab = (bits & kButtonGrab) != 0;
        in.restart = (bits & kButtonRestart) != 0;
        return in;
    }

    /// Round-trip through the packing. The simulation consumes the
    /// *quantized* input even while a human is playing, so that a live run
    /// and a replay of that run take exactly the same path. Quantizing only
    /// on the way to disk would leave live play and playback on two
    /// different inputs, and the drift shows up minutes later as a replay
    /// that stops completing.
    InputState Quantized() const { return Unpack(Pack()); }

    bool operator==(const InputState& o) const { return Pack() == o.Pack(); }
};

/// Radial deadzone with rescaling (doc 7.2). Per-axis deadzones make
/// diagonals feel wrong, and without the rescale the output jumps from zero
/// to the deadzone magnitude the moment the stick crosses it.
///
/// Declared here for the game layer; `godot/scripts/input.gd` implements the
/// same function for the engine side, and `test_game.cpp` checks the two
/// agree at the boundary values.
inline void ApplyRadialDeadzone(float x, float y, float dead, float* out_x, float* out_y) {
    const float mag2 = x * x + y * y;
    const float dead2 = dead * dead;
    if (mag2 <= dead2 || mag2 <= 0.0f) {
        *out_x = 0.0f;
        *out_y = 0.0f;
        return;
    }
    // One sqrt, and only past the deadzone: a stick at rest is the common
    // case and it costs nothing.
    const float mag = std::sqrt(mag2);
    float scale = (mag - dead) / (1.0f - dead);
    if (scale > 1.0f) scale = 1.0f;  // stick corners read past 1.0
    const float inv = scale / mag;
    *out_x = x * inv;
    *out_y = y * inv;
}

}  // namespace ppp
