#include "../src/mouse_input.hpp"
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <initializer_list>

void require(bool condition, const char* message) {
    if (!condition) { std::fprintf(stderr, "FAIL: %s\n", message); std::exit(1); }
}
bool near(double a, double b) { return std::abs(a - b) < 1e-10; }

int main() {
    // One physical movement must turn equally whether it arrived in one slow
    // frame or many fast frames, including movements above the native stick cap.
    const auto whole = ts3::radians({1200, 120}, 0.08, 0.08, false, false);
    for (int frames : {30, 60, 144, 240}) {
        ts3::Delta sum{};
        for (int i = 0; i < frames; ++i) {
            const auto step = ts3::radians({1200.0 / frames, 120.0 / frames}, 0.08, 0.08, false, false);
            sum.x += step.x; sum.y += step.y;
        }
        require(near(sum.x, whole.x) && near(sum.y, whole.y), "frame partition changes turn distance");
    }
    const auto diagonal = ts3::radians({100, 1}, 0.08, 0.08, false, false);
    require(diagonal.y > 0 && near(diagonal.x / diagonal.y, 100), "shallow diagonal lost its minor axis");
    const auto inverted = ts3::radians({100, 1}, 0.08, 0.08, true, true);
    require(near(inverted.x, -diagonal.x) && near(inverted.y, -diagonal.y), "inversion");

    // Actual consumer contract: native code multiplies yawVelocity by dt once.
    // A 4:1 movement is discarded by the original game. Both axes must survive
    // even at one count, without a different total angle at higher frame rates.
    for (int fps : {30, 60, 144, 240}) {
        const float dt = 1.0f / fps;
        const auto angle = ts3::radians({4, 1}, 0.08, 0.08, false, false);
        const auto step = ts3::aimStep(angle, dt, 0, -1, 1);
        require(std::abs(step.yawVelocity * dt - angle.x) < 1e-8 &&
                std::abs(step.pitch - angle.y) < 1e-8, "reticle loses slow diagonals or depends on FPS");
        const auto stop = ts3::aimStep({}, dt, step.pitch, -1, 1);
        require(stop.yawVelocity == 0 && stop.pitch == step.pitch, "reticle keeps moving after input stops");
        const auto fast = ts3::aimStep(ts3::radians({1200, 0}, 0.08, 0.08, false, false), dt, 0, -1, 1);
        require(std::abs(fast.yawVelocity * dt - whole.x) < 1e-6, "reticle retains a turn-speed cap");
    }
    require(ts3::aimStep({0, 20}, 0.016f, 0, -1, 1).pitch == 1, "upper pitch limit");
    require(ts3::aimStep({0, -20}, 0.016f, 0, -1, 1).pitch == -1, "lower pitch limit");

    ts3::Samples samples;
    ts3::Delta out;
    samples.submit(true, true, {900, 900}, 10);
    require(samples.take(10, out) && out.x == 0 && out.y == 0, "first focus sample must be discarded");
    samples.submit(true, true, {37, 2}, 20);
    require(samples.take(20, out) && out.x == 37 && out.y == 2, "new displacement");
    require(samples.take(20, out) && out.x == 0 && out.y == 0, "sample applied twice");
    samples.submit(true, true, {}, 30);
    require(samples.take(30, out) && out.x == 0 && out.y == 0, "movement continued after mouse stopped");
    samples.submit(true, false, {400, 400}, 40);
    require(!samples.take(40, out), "background input leaked");
    samples.submit(true, true, {500, 500}, 50);
    require(samples.take(50, out) && out.x == 0, "focus regain jump");
    samples.submit(false, true, {600, 600}, 60);
    require(!samples.take(60, out), "failed DirectInput read accepted");
    samples.submit(true, true, {700, 700}, 70);
    require(samples.take(70, out) && out.x == 0, "reacquire jump");
    samples.submit(true, true, {800, 800}, 80);
    require(!samples.take(181, out), "stale sample accepted after pause");
    samples.submit(true, true, {1000, 1000}, 200); // menu frame, not consumed
    samples.submit(true, true, {3, 4}, 210);       // first gameplay frame
    require(samples.take(210, out) && out.x == 3 && out.y == 4, "menu movement accumulated");
    samples.submit(true, true, {2000, 2000}, 1000);
    require(samples.take(1000, out) && out.x == 0 && out.y == 0, "movement accumulated during a polling gap");
    std::puts("PASS: frame independence, diagonals, inversion, single consumption, stop, focus, failure, staleness, menus");
}
