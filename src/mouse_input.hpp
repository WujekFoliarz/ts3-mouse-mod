#pragma once
#include <cstdint>
#include <algorithm>

namespace ts3 {
struct Delta { double x = 0; double y = 0; };

// DirectInput returns displacement since the previous poll. Never multiply this
// by frame time, normalize its length, discard its smaller axis, or cap it to 1.
inline Delta radians(Delta counts, double degreesX, double degreesY,
                     bool invertX, bool invertY) noexcept {
    constexpr double degreesToRadians = 0.017453292519943295769;
    return {counts.x * degreesX * degreesToRadians * (invertX ? -1.0 : 1.0),
            counts.y * degreesY * degreesToRadians * (invertY ? -1.0 : 1.0)};
}

struct AimStep { float yawVelocity; float pitch; };
// The native reticle consumes yaw as radians/second and pitch as radians.
// Caller verifies finite inputs, positive dt and ordered native pitch limits.
inline AimStep aimStep(Delta angle, float dt, float pitch, float minimum, float maximum) noexcept {
    return {static_cast<float>(angle.x / dt),
            std::clamp(pitch + static_cast<float>(angle.y), minimum, maximum)};
}

// The game polls once per input update. Keep only that update's sample: movement
// in menus must never accumulate into a camera jump when gameplay resumes.
// The caller supplies synchronization; tests can use this class without Win32.
class Samples {
public:
    void submit(bool success, bool focused, Delta counts, std::uint64_t now) noexcept {
        ++serial_;
        valid_ = success && focused;
        const bool continuous = wasFocused_ && now >= time_ && now - time_ <= 100;
        counts_ = valid_ && continuous ? counts : Delta{};
        wasFocused_ = valid_;
        time_ = now;
    }
    bool take(std::uint64_t now, Delta& out) noexcept {
        out = {};
        if (!valid_ || now < time_ || now - time_ > 100) return false;
        if (consumed_ != serial_) {
            out = counts_;
            consumed_ = serial_;
        }
        // A second camera update uses zero displacement, suppressing a second
        // application of the game's virtual stick for the same mouse sample.
        return true;
    }
private:
    Delta counts_{};
    std::uint64_t serial_ = 0, consumed_ = 0, time_ = 0;
    bool valid_ = false, wasFocused_ = false;
};
} // namespace ts3
