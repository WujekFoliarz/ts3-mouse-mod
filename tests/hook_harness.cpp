// Windows/Wine fixture: execute the actual patched CALL relays and C++ hooks
// against synthetic objects. This is an ABI/loader test, not a gameplay test.
#include <dinput.h>
#include "../src/TS3Mouse.cpp"

void Check(bool ok, const char* message) {
    if (!ok) { std::fprintf(stderr, "FAIL: %s (Win32 %lu)\n", message, GetLastError()); std::exit(1); }
}
HRESULT WINAPI FakeMouse(void*, DWORD size, void* output) {
    Check(size == 20, "poll hook forwards DIMOUSESTATE2 size");
    auto* mouse = static_cast<MouseState*>(output);
    *mouse = {300, 1, -120, {0x80, 0, 0x80, 0, 0, 0, 0, 0}};
    return S_OK;
}
unsigned nativeCameraCalls = 0;
unsigned nativeAimCalls = 0;
bool FakeAim(void*, float x, float y, unsigned flags) {
    ++nativeAimCalls;
    Check(x == 0.2f && y == -0.1f && flags == 0, "native aim ABI");
    return true;
}
void FakeDriver(void* driver) {
    Write(driver, 0x70, 0.0f);
    Write(driver, 0x74, 0.0f);
}
void FakeCamera(void*, void*) {
    ++nativeCameraCalls;
    Check(g_scope && !g_scope->raw, "disabled mapper uses original camera context");
}
void AbsoluteJump(Byte* target, void* destination) {
    const Byte code[] = {0xFF, 0x25, 0, 0, 0, 0};
    std::memcpy(target, code, sizeof(code));
    std::memcpy(target + 6, &destination, sizeof(destination));
}
Byte* MakeCallable(std::uintptr_t site, std::size_t size) {
    auto* location = reinterpret_cast<Byte*>(g_base + site);
    const Byte prologue[] = {0x48, 0x83, 0xEC, 0x28}; // x64 shadow space + alignment
    const Byte epilogue[] = {0x48, 0x83, 0xC4, 0x28, 0xC3};
    std::memcpy(location - 4, prologue, sizeof(prologue));
    std::memcpy(location + size, epilogue, sizeof(epilogue));
    return location - 4;
}
int main() {
    auto* memory = static_cast<Byte*>(VirtualAlloc(nullptr, kImageSize, MEM_RESERVE | MEM_COMMIT,
                                                  PAGE_EXECUTE_READWRITE));
    Check(memory != nullptr, "allocate synthetic game image");
    g_base = reinterpret_cast<std::uintptr_t>(memory);
    const Byte poll[] = {0xBA, 0x14, 0, 0, 0, 0xFF, 0x50, 0x48};
    const Byte camera[] = {0xE8, 0xEA, 0x34, 0xFE, 0xFF};
    const Byte integrate[] = {0xE8, 0xA5, 0x3A, 0x05, 0};
    std::memcpy(memory + 0x27B8F7, poll, sizeof(poll));
    std::memcpy(memory + 0x21ABC1, camera, sizeof(camera));
    std::memcpy(memory + 0x1FE356, integrate, sizeof(integrate));
    const Byte driver[] = {0xFF,0x90,0xD8,0,0,0};
    const Byte target[] = {0xE8,0xA3,0x63,0xFD,0xFF};
    const Byte aim[] = {0xE8,0xE5,0xA8,0xFB,0xFF};
    // Exact instructions from the verified EXE, including BOTH snapping arms.
    const Byte snap[] = {0x8b,0xd0,0xf7,0xda,0x0f,0x48,0xd0,0x44,0x8b,0xc1,0x41,0xf7,0xd8,
        0x44,0x0f,0x48,0xc1,0x43,0x8d,0x04,0x40,0x3b,0xc2,0x7d,0x09,0x44,0x89,0xb3,
        0xdc,0,0,0,0xeb,0x0f,0x8d,0x04,0x52,0x41,0x3b,0xc0,0x7d,0x07,0x44,0x89,0xb3,0xe0,0,0,0};
    std::memcpy(memory + 0x27BA1B, driver, sizeof(driver));
    std::memcpy(memory + 0x0AF528, target, sizeof(target));
    std::memcpy(memory + 0x0859F6, aim, sizeof(aim));
    std::memcpy(memory + 0x27B932, snap, sizeof(snap));
    // void snapFixture(manager, x, y): set RBX/ECX/EAX/R14D as at RVA 27B932.
    const Byte snapPrologue[] = {0x53,0x41,0x56,0x48,0x89,0xcb,0x89,0xd1,0x44,0x89,0xc0,0x45,0x31,0xf6};
    const Byte snapEpilogue[] = {0x41,0x5e,0x5b,0xc3};
    auto* snapEntry = memory + 0x27B932 - sizeof(snapPrologue);
    std::memcpy(snapEntry, snapPrologue, sizeof(snapPrologue));
    std::memcpy(memory + 0x27B963, snapEpilogue, sizeof(snapEpilogue));
    using Snap = void (*)(void*, int, int);
    const auto runSnap = reinterpret_cast<Snap>(snapEntry);
    std::array<Byte, 0x600> manager{};
    Write(manager.data(), 0xDC, 4); Write(manager.data(), 0xE0, 1);
    runSnap(manager.data(), 4, 1);
    Check(Read<int>(manager.data(), 0xE0) == 0, "original game discards slow diagonal minor axis");
    memory[0x21ABC1] = 0xCC;
    Check(!InstallPatches(), "mismatched call site rejected");
    Check(!std::memcmp(memory + 0x27B8F7, poll, sizeof(poll)), "failure leaves first site intact");
    memory[0x21ABC1] = 0xE8;
    auto* pollEntry = MakeCallable(0x27B8F7, 8);
    auto* cameraEntry = MakeCallable(0x21ABC1, 5);
    auto* integrateEntry = MakeCallable(0x1FE356, 5);
    auto* aimEntry = MakeCallable(0x0859F6, 5);
    auto* driverEntry = MakeCallable(0x27BA1B, 6);
    memory[kNormalize] = 0xC3; // identity float function: preserve incoming XMM0
    AbsoluteJump(memory + kCameraInput, reinterpret_cast<void*>(&FakeCamera));
    AbsoluteJump(memory + 0x402E0, reinterpret_cast<void*>(&FakeAim));
    DWORD oldProtection = 0;
    Check(VirtualProtect(memory + 0x27B000, 4096, PAGE_EXECUTE_READ, &oldProtection) != 0,
          "set original shared code page protection");
    Check(InstallPatches(), "install six rel32 call relays and snap bypass");
    MEMORY_BASIC_INFORMATION info{};
    Check(VirtualQuery(memory + 0x27B943, &info, sizeof(info)) != 0 && info.Protect == PAGE_EXECUTE_READ,
          "multiple patches on one page restore original RX protection");
    FlushInstructionCache(GetCurrentProcess(), memory, kImageSize);
    for (const auto delta : {ts3::Delta{4,1}, {1,4}, {-4,1}, {1,-4}, {100,1}, {0,0}}) {
        Write(manager.data(), 0xDC, static_cast<int>(delta.x));
        Write(manager.data(), 0xE0, static_cast<int>(delta.y));
        runSnap(manager.data(), static_cast<int>(delta.x), static_cast<int>(delta.y));
        Check(Read<int>(manager.data(), 0xDC) == delta.x && Read<int>(manager.data(), 0xE0) == delta.y,
              "patched game instructions preserve both mouse axes");
    }
    void* mouseVtable[10]{};
    mouseVtable[9] = reinterpret_cast<void*>(&FakeMouse);
    void** device = mouseVtable;
    MouseState state{};
    using Poll = HRESULT (WINAPI*)(void*, DWORD, MouseState*);
    Check(reinterpret_cast<Poll>(pollEntry)(&device, 0xFFFFFFFF, &state) == S_OK, "poll relay HRESULT");
    Check(state.x == 300 && state.y == 1 && state.wheel == -120 && state.buttons[2] == 0x80,
          "poll relay preserves relative counts, wheel and buttons");

    std::array<Byte, 0x700> cameraObject{}, mapper{};
    CameraScope scope{};
    scope.raw = true; scope.camera = cameraObject.data();
    scope.yaw = 0.5f; scope.pitch = 0.1f; scope.deltaYaw = 0.2f; scope.deltaPitch = -0.3f;
    Write(cameraObject.data(), 0x208, std::uint32_t{0x40000002});
    g_scope = &scope;
    const auto normalize = reinterpret_cast<float (*)(float)>(integrateEntry);
    Check(std::abs(normalize(9.0f) - 0.7f) < 1e-6f, "float argument/return and replacement yaw");
    Check(std::abs(Read<float>(cameraObject.data(), 0x28) + 0.2f) < 1e-6f, "replacement pitch");
    Check(Read<std::uint32_t>(cameraObject.data(), 0x208) == 0x40000002, "preserve other camera flags");
    scope.deltaYaw = scope.deltaPitch = 0;
    Check(std::abs(normalize(9.0f) - 0.5f) < 1e-6f, "zero movement suppresses native turn");
    Check(Read<std::uint32_t>(cameraObject.data(), 0x208) == 0x40000000, "clear native mouse movement flag");
    reinterpret_cast<void (*)(void*, void*)>(cameraEntry)(cameraObject.data(), mapper.data());
    Check(nativeCameraCalls == 1 && g_scope == &scope, "camera relay and nested scope restoration");
    g_scope = nullptr;
    Check(normalize(1.25f) == 1.25f, "native normalization outside mouse scope");

    std::array<Byte, 0x600> keyboard{}, gamepad{}, targetObject{};
    Write(keyboard.data(), 0, g_base + kKeyboardVtable);
    Write(keyboard.data(), 0x480, BYTE{1});
    Write(keyboard.data(), 0x2E8, static_cast<void*>(manager.data()));
    Write(mapper.data(), 8, static_cast<void*>(keyboard.data()));
    Write(mapper.data(), 0x10, BYTE{1}); Write(mapper.data(), 0x154, BYTE{1});
    Write(mapper.data(), 0x1C, 3); Write(mapper.data(), 0x20, 4);
    void* driverVtable[28]{};
    driverVtable[27] = reinterpret_cast<void*>(&FakeDriver);
    Write(gamepad.data(), 0, static_cast<void*>(driverVtable));
    Write(gamepad.data(), 0x270, static_cast<void*>(keyboard.data()));
    reinterpret_cast<void (*)(void*)>(driverEntry)(gamepad.data());
    Write(gamepad.data(), 0x70, 0.8f); // manager subsequently copies keyboard axes
    Write(mapper.data(), 8, static_cast<void*>(gamepad.data()));
    Check(KeyboardForMapper(mapper.data()) == keyboard.data(), "copied mouse axes must not be mistaken for a physical stick");
    g_drivers[0].x = 0.4f;
    Check(KeyboardForMapper(mapper.data()) == nullptr, "actual physical stick keeps native input");
    Write(mapper.data(), 8, static_cast<void*>(keyboard.data()));

    const HWND window = CreateWindowExW(0, L"STATIC", L"TS3 mouse hook test", WS_OVERLAPPEDWINDOW | WS_VISIBLE,
                                        0, 0, 320, 200, nullptr, nullptr, GetModuleHandleW(nullptr), nullptr);
    Check(window != nullptr, "create focus fixture window");
    SetForegroundWindow(window);
    Check(ProcessHasFocus(), "focus fixture window");
    Write(manager.data(), 0x238, window); Write(manager.data(), 0x501, BYTE{1});
    Write(manager.data(), 0x220, g_sampleDevice);
    Write(targetObject.data(), 0x1A0, -1.0f); Write(targetObject.data(), 0x1A4, 1.0f);
    AimScope aimScope{targetObject.data(), mapper.data()};
    g_aimScope = &aimScope;
    using Aim = bool (*)(void*, float, float, unsigned);
    const auto runAim = reinterpret_cast<Aim>(aimEntry);
    Check(runAim(targetObject.data(), 0.2f, -0.1f, 0) && nativeAimCalls == 1, "aim permissions preserve native fallback");
    const auto tick = GetTickCount64();
    g_aimSamples.submit(true, true, {}, tick);
    for (float dt : {1.0f/30, 1.0f/60, 1.0f/144}) {
        Write(memory, 0x95483C, dt);
        Write(targetObject.data(), 0x18C, 0.0f);
        g_aimSamples.submit(true, true, {4,1}, GetTickCount64());
        Check(runAim(targetObject.data(), 0, 0, 0x800), "small diagonal bypasses native deadzones");
        const auto expected = ts3::radians({4,1}, 0.08, 0.08, g_config.invertX, g_config.invertY);
        Check(std::abs(Read<float>(targetObject.data(), 0x168) * dt - expected.x) < 1e-7,
              "native yaw integration produces displacement at each frame rate");
        Check(std::abs(Read<float>(targetObject.data(), 0x18C) - expected.y) < 1e-7,
              "minor pitch axis reaches actual reticle");
        Check(!runAim(targetObject.data(), 0, 0, 0x800) && Read<float>(targetObject.data(), 0x168) == 0,
              "duplicate sample and stopped mouse have no yaw inertia");
    }
    g_aimSamples.submit(true, true, {8000,8000}, GetTickCount64());
    Check(runAim(targetObject.data(), 0, 0, 0x800) &&
          Read<float>(targetObject.data(), 0x18C) == (g_config.invertY ? -1.0f : 1.0f),
          "fast movement retains native pitch bounds");
    g_aimScope = nullptr;
    DestroyWindow(window);

    // Load the actual shipping DLL. Our EXE is intentionally an unsupported
    // build: its mod must remain off while its DirectInput export still works.
    const HMODULE dll = LoadLibraryW(L".\\dinput8.dll");
    Check(dll != nullptr, "load built proxy DLL");
    using Create = HRESULT (WINAPI*)(HINSTANCE, DWORD, REFIID, void**, LPUNKNOWN);
    const FARPROC address = GetProcAddress(dll, "DirectInput8Create");
    Check(address != nullptr, "DirectInput8Create export");
    Create create = nullptr;
    std::memcpy(&create, &address, sizeof(create));
    IDirectInput8W* input = nullptr;
    Check(SUCCEEDED(create(GetModuleHandleW(nullptr), DIRECTINPUT_VERSION, IID_IDirectInput8W,
                           reinterpret_cast<void**>(&input), nullptr)), "forward real DirectInput8Create");
    Check(input != nullptr, "real DirectInput object");
    input->Release();
    std::puts("PASS: real axis-snap instructions, patch rejection, x64 relays, COM ABI, camera replacement, reticle diagonals/FPS/limits/stop, device arbitration, proxy forwarding");
    return 0;
}
