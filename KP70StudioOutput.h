// Included after Controller and SideDsp in KP70Engine.cpp.
constexpr size_t kCurveBands = 7;
struct StudioChannel {
    int gain, sensitivity, threshold, custom_curve;
    int curve[kCurveBands];
};
struct StudioSettings {
    int shoulder, grip_mode, trigger, base, start, frequency, suppression, shoulder_press, trigger_press;
    StudioChannel channels[6]; // left/right shoulder, left/right grip, left/right trigger
    int press[6];              // 1 = this channel only runs while its button is held
    int trg_base[2], trg_kick[2], trg_start[2], trg_freq[2];
};
static_assert(sizeof(StudioSettings) == 89 * sizeof(int));

// XInput 1.4 exposes these identifiers through ordinal 108. Resolve it at
// runtime so systems without the extension can still use explicit binding.
struct PadCapabilities {
    XINPUT_CAPABILITIES standard;
    WORD vendor, product, version, reserved;
    DWORD extra;
};
static_assert(offsetof(PadCapabilities, vendor) == 20);

PadCapabilities pad_identity(DWORD slot) {
    using Query = DWORD(WINAPI*)(DWORD, DWORD, DWORD, PadCapabilities*);
    static const auto query = reinterpret_cast<Query>(GetProcAddress(
        GetModuleHandleW(L"xinput1_4.dll"), MAKEINTRESOURCEA(108)));
    PadCapabilities info{};
    if (query && query(1, slot, 0, &info) != ERROR_SUCCESS) return {};
    return info;
}

class StudioSession {
    HANDLE mapping = nullptr, mutex = nullptr, stop = nullptr, parent = nullptr;
    const StudioSettings* view = nullptr;
public:
    StudioSession(const char* id, DWORD parent_id) {
        const std::string name(id);
        mapping = OpenFileMappingA(FILE_MAP_READ, FALSE, id);
        if (mapping) view = static_cast<const StudioSettings*>(MapViewOfFile(mapping, FILE_MAP_READ, 0, 0, sizeof(StudioSettings)));
        mutex = OpenMutexA(SYNCHRONIZE | MUTEX_MODIFY_STATE, FALSE, (name + "_lock").c_str());
        stop = OpenEventA(SYNCHRONIZE, FALSE, (name + "_stop").c_str());
        parent = OpenProcess(SYNCHRONIZE, FALSE, parent_id);
    }
    ~StudioSession() {
        if (view) UnmapViewOfFile(view);
        for (auto handle : {mapping, mutex, stop, parent}) if (handle) CloseHandle(handle);
    }
    bool ready() const { return view && mutex && stop && parent; }
    bool ended() const {
        return WaitForSingleObject(stop, 0) != WAIT_TIMEOUT || WaitForSingleObject(parent, 0) != WAIT_TIMEOUT;
    }
    void read(StudioSettings& settings) const {
        const DWORD wait = WaitForSingleObject(mutex, 0);
        if (wait != WAIT_OBJECT_0 && wait != WAIT_ABANDONED) return;
        settings = *view;
        ReleaseMutex(mutex);
    }
};

class StudioOutput {
    Controller& device;
    bool simulate;
    USHORT product;
    bool ambiguous;
    DWORD pad = XUSER_MAX_COUNT;
    bool grip_owned = false;
    unsigned short grip_level[2]{};
    unsigned char shoulder_level[2]{};
    ULONGLONG shoulder_update[2]{};
    unsigned char linear_level[2]{};
    ULONGLONG linear_update[2]{};
    struct Trigger {
        unsigned mode = 0, strength = 0, start = 0, frequency = 0;
        bool armed = true;
        ULONGLONG began = 0;
    } trigger[2];

    void clear_grip() {
        if (grip_owned && !simulate && pad < XUSER_MAX_COUNT) {
            XINPUT_VIBRATION zero{};
            XInputSetState(pad, &zero);
        }
        grip_owned = false;
        grip_level[0] = grip_level[1] = 0;
    }

    bool read_pad(XINPUT_STATE& state) {
        if (simulate) {
            state.Gamepad.bLeftTrigger = state.Gamepad.bRightTrigger = 255;
            return true;
        }
        if (pad < XUSER_MAX_COUNT) {
            if (XInputGetState(pad, &state) == ERROR_SUCCESS) return true;
            clear_grip();
            pad = XUSER_MAX_COUNT;
        }
        unsigned exact = 0, manual = 0, connected = 0;
        DWORD exact_slot = XUSER_MAX_COUNT, manual_slot = XUSER_MAX_COUNT, only_slot = XUSER_MAX_COUNT;
        XINPUT_STATE exact_state{}, manual_state{}, only_state{};
        for (DWORD index = 0; index < XUSER_MAX_COUNT; ++index) {
            XINPUT_STATE candidate{};
            if (XInputGetState(index, &candidate) != ERROR_SUCCESS) continue;
            only_slot = index;
            only_state = candidate;
            ++connected;
            const auto id = pad_identity(index);
            if (id.vendor == kVendorId && id.product == product) {
                exact_slot = index;
                exact_state = candidate;
                ++exact;
            } else if (id.vendor == kVendorId || (id.vendor && id.vendor != 0x045e)) {
                continue;
            }
            if ((candidate.Gamepad.wButtons & (XINPUT_GAMEPAD_LEFT_SHOULDER | XINPUT_GAMEPAD_RIGHT_SHOULDER)) !=
                    (XINPUT_GAMEPAD_LEFT_SHOULDER | XINPUT_GAMEPAD_RIGHT_SHOULDER)) continue;
            manual_slot = index;
            manual_state = candidate;
            ++manual;
        }
        if (exact == 1 && !ambiguous) {
            pad = exact_slot;
            state = exact_state;
        } else if (manual == 1) {
            pad = manual_slot;
            state = manual_state;
        } else if (connected == 1 && !ambiguous) {
            pad = only_slot;
            state = only_state;
        } else return false;
        return true;
    }

    bool write_trigger(unsigned side, unsigned mode, unsigned value, unsigned start, unsigned frequency) {
        auto& old = trigger[side];
        if (mode == old.mode && value == old.strength && start == old.start && frequency == old.frequency)
            return true;
        if (!simulate) {
            bool ok = mode == 2 ? device.trigger_gun(side, start, value, frequency) :
                mode == 1 ? device.trigger_race(side, start, value) : device.trigger_clear(side);
            if (!ok) return false;
        }
        old.mode = mode; old.strength = value; old.start = start; old.frequency = frequency;
        return true;
    }

public:
    bool pad_ready = false;
    int meters[6]{}, trigger_modes[2]{};

    StudioOutput(Controller& device, bool simulate, USHORT product, bool ambiguous)
        : device(device), simulate(simulate), product(product), ambiguous(ambiguous) {}
    ~StudioOutput() {
        clear_grip();
        if (simulate) return;
        for (unsigned side = 0; side < 2; ++side) {
            if (shoulder_level[side] && !device.shoulder_level(side, 0)) device.shoulder_level(side, 0);
            if (trigger[side].mode && !device.trigger_clear(side)) device.trigger_clear(side);
            if (linear_level[side] && !device.linear_trigger(side, 0)) device.linear_trigger(side, 0);
        }
    }

    bool update(const StudioSettings& cfg, const std::array<float, 2>& audio,
                const std::array<float, 2>& trigger_audio,
                const std::array<float, 2>& bass, ULONGLONG now) {
        XINPUT_STATE state{};
        pad_ready = read_pad(state);
        const bool control = device.ready() || simulate;
        const bool force = control && (!product || supports_force_trigger(product));
        const bool shoulders = control && (!product || supported_kp70c(product));
        const bool linear = control && supports_linear_trigger(product);
        const bool audio_grip = cfg.grip_mode == 1 && pad_ready;
        unsigned short next_grip[2]{};
        for (unsigned side = 0; side < 2; ++side) {
            const auto& shoulder_tuning = cfg.channels[side];
            const auto& grip_tuning = cfg.channels[2 + side];
            const auto& trigger_tuning = cfg.channels[4 + side];
            const WORD bumper = side ? XINPUT_GAMEPAD_RIGHT_SHOULDER : XINPUT_GAMEPAD_LEFT_SHOULDER;
            const bool shoulder_open = shoulders && cfg.shoulder && (!cfg.press[side] ||
                (pad_ready && (state.Gamepad.wButtons & bumper)));
            const float shoulder_floor = std::clamp(shoulder_tuning.threshold, 0, 100) / 100.f;
            const float shoulder = shoulder_open && audio[side] >= shoulder_floor
                ? audio[side] * std::clamp(shoulder_tuning.gain, 0, 100) / 100.f : 0;
            const auto level = (unsigned char)std::lround(std::clamp(shoulder, 0.f, 1.f) * 192.f);
            if (level != shoulder_level[side] || (level && now - shoulder_update[side] >= 250)) {
                if (!simulate && !device.shoulder_level(side, level)) return false;
                shoulder_level[side] = level;
                shoulder_update[side] = now;
            }
            meters[side] = (int)std::lround(level * 100.f / 192.f);

            const float grip_floor = std::clamp(grip_tuning.threshold, 0, 100) / 100.f;
            const float grip = audio_grip
                ? std::max(0.f, bass[side] - grip_floor) * std::clamp(grip_tuning.gain, 0, 100) / 100.f : 0;
            next_grip[side] = (unsigned short)std::lround(std::clamp(grip, 0.f, 1.f) * 39321.f);
            meters[2 + side] = (int)std::lround(next_grip[side] * 100.f / 39321.f);

            auto& effect = trigger[side];
            const bool enabled = force && cfg.trigger && pad_ready;
            const auto travel = side ? state.Gamepad.bRightTrigger : state.Gamepad.bLeftTrigger;
            const float trigger_on = std::clamp(trigger_tuning.threshold, 1, 100) / 100.f;
            const float trigger_off = trigger_on * 0.4f;
            const float trigger_level = trigger_audio[side];
            if (linear) {
                const bool active = cfg.trigger && (!cfg.press[4 + side] || (pad_ready && travel >= 24));
                const auto level = active && trigger_level >= trigger_on
                    ? (unsigned char)std::lround(std::clamp(trigger_level, 0.f, 1.f) *
                        std::clamp(trigger_tuning.gain, 0, 100) * 2.55f) : 0;
                const auto elapsed = now - linear_update[side];
                if ((!level && linear_level[side]) ||
                        (level && (elapsed >= 45 || (level != linear_level[side] && elapsed >= 25)))) {
                    if (!simulate && !device.linear_trigger(side, level)) return false;
                    linear_level[side] = level;
                    linear_update[side] = now;
                }
                meters[4 + side] = (int)std::lround(linear_level[side] * 100.f / 255.f);
                trigger_modes[side] = linear_level[side] ? 3 : 0;
                continue;
            }
            if (!enabled || travel < 24 || trigger_level < trigger_off) effect.armed = true;
            if (!enabled || travel < 24 || (effect.began &&
                    ((now - effect.began >= 45 && trigger_level < trigger_off) || now - effect.began >= 140))) effect.began = 0;
            else if (!effect.began && effect.armed && trigger_level >= trigger_on) {
                effect.began = now;
                effect.armed = false;
            }
            const unsigned base = std::clamp(cfg.trg_base[side], 0, 36);
            const unsigned kick = std::clamp(cfg.trg_kick[side], 0, 60);
            const bool hit = effect.began && kick;
            const unsigned mode = enabled ? (hit ? 2u : (base ? 1u : 0u)) : 0u;
            const unsigned value = mode == 2 ? std::max(1u, (unsigned)std::lround(kick * std::max(0.25f, trigger_level))) :
                mode == 1 ? base : 0;
            if (!write_trigger(side, mode, value, mode ? std::clamp(cfg.trg_start[side], 0, 40) : 0,
                    mode == 2 ? std::clamp(cfg.trg_freq[side], 1, 63) : 0)) return false;
            meters[4 + side] = mode == 2 ? (int)(100 * value / 60) : 0;
            trigger_modes[side] = (int)mode;
        }
        if (!audio_grip) {
            clear_grip();
        } else if (next_grip[0] != grip_level[0] || next_grip[1] != grip_level[1]) {
            XINPUT_VIBRATION packet{next_grip[0], next_grip[1]};
            if (!simulate && XInputSetState(pad, &packet) != ERROR_SUCCESS) return false;
            grip_owned = next_grip[0] || next_grip[1];
            grip_level[0] = next_grip[0];
            grip_level[1] = next_grip[1];
        }
        return true;
    }
};
