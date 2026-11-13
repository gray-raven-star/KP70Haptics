// KP70Engine - audio-driven haptics for the Beitong Kunpeng 70.
//
// Replaces the 42-235 Hz band-RMS + fixed-pulse approach with the structure
// the mature implementations use:
//
//   Flydigi xHaptics (Engine/config/x-haptics/dsp_profiles/grip_dsp/*.json)
//     analysis 300 Hz - 12 kHz, three bands with their own attack/release and
//     thresholds, harmonic (music/voice) suppression, percentile auto-gain,
//     soft-knee output shaping.
//   DS5Dongle-Studio (src/audio.cpp)
//     envelope-only band processing and a SMOOTHED gate (~5 ms open, ~60 ms
//     close) because a hard gate chatters and chops the output.
//
// Hardware facts established by measurement with KP70Cal (see
// 协议解码-20261107.md):
//   - shoulder normal effect: channel 0 = LEFT, channel 1 = RIGHT
//   - channels 4 and 5 are the stop commands
//   - a single packet is barely perceptible; effects must be resent
//     continuously to sustain
//
// Still unverified, so both are exposed as options rather than hard-coded:
//   - whether bytes 4/5 are the per-step duration  (--step)
//   - how fast the device consumes the 10 envelope steps (--refresh)

#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <mmdeviceapi.h>
#include <audioclient.h>
#include <setupapi.h>
#include <cfgmgr32.h>
#include <xinput.h>
#include <algorithm>
#include <array>
#include <cctype>
#include <cmath>
#include <cwchar>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <deque>
#include <memory>
#include <stdexcept>
#include <string>
#include <string_view>
#include <vector>

#ifndef M_PI
#define M_PI 3.14159265358979323846
#endif

// ---------------------------------------------------------------- HID output

extern "C" {
typedef struct _HIDD_ATTRIBUTES { ULONG Size; USHORT VendorID, ProductID, VersionNumber; } HIDD_ATTRIBUTES;
typedef struct _HIDP_CAPS {
    USHORT Usage, UsagePage, InputReportByteLength, OutputReportByteLength;
    USHORT FeatureReportByteLength, Reserved[17];
    USHORT NumberLinkCollectionNodes, NumberInputButtonCaps, NumberInputValueCaps;
    USHORT NumberInputDataIndices, NumberOutputButtonCaps, NumberOutputValueCaps;
    USHORT NumberOutputDataIndices, NumberFeatureButtonCaps, NumberFeatureValueCaps;
    USHORT NumberFeatureDataIndices;
} HIDP_CAPS;
typedef void* PHIDP_PREPARSED_DATA;
void   __stdcall HidD_GetHidGuid(LPGUID);
BOOLEAN __stdcall HidD_GetAttributes(HANDLE, HIDD_ATTRIBUTES*);
BOOLEAN __stdcall HidD_GetPreparsedData(HANDLE, PHIDP_PREPARSED_DATA*);
BOOLEAN __stdcall HidD_FreePreparsedData(PHIDP_PREPARSED_DATA);
LONG   __stdcall HidP_GetCaps(PHIDP_PREPARSED_DATA, HIDP_CAPS*);
BOOLEAN __stdcall HidD_SetOutputReport(HANDLE, PVOID, ULONG);
}

namespace {

constexpr USHORT kVendorId = 0x20BC, kProductId = 0x519C;
constexpr USHORT kUsagePage = 0x00FF, kUsage = 0x0003;
constexpr size_t kReportLen = 64;
constexpr unsigned kSteps = 10;          // envelope slots in one packet
constexpr unsigned kLeftChannel = 0;     // measured
constexpr unsigned kRightChannel = 1;    // measured

bool supported_kp70c(USHORT product) {
    // Beitong's shoulder/trigger model list and matching direct-output firmware.
    switch (product) {
        case 0x5173: case 0x5174: case 0x518A: case 0x518B: case 0x518C: case 0x518D:
        case 0x519B: case 0x519C: case 0x519D: case 0x519E: return true;
        default: return false;
    }
}

bool supports_force_trigger(USHORT product) {
    // The A/A1/B firmware handles the same 0x56 trigger report as the C family.
    switch (product) {
        case 0x512F: case 0x5130: case 0x516D: case 0x516E:
        case 0x515F: case 0x5160: return true;
        default: return supported_kp70c(product);
    }
}

constexpr bool supports_linear_trigger(USHORT product) {
    return product == 0x5194 || product == 0x5195;
}

bool supports_control(USHORT product) {
    return supports_force_trigger(product) || supports_linear_trigger(product);
}

constexpr std::array<unsigned char, kReportLen> linear_trigger_report(unsigned side, unsigned char level) {
    // KP40E's game detector uses mode 2. Wave 0x15 plays; wave 0 is its stop pattern.
    return {2, 0x76, 2, static_cast<unsigned char>(side), 1,
        static_cast<unsigned char>(level ? 0x15 : 0), static_cast<unsigned char>(level ? level : 0xff),
        static_cast<unsigned char>(level ? 6 : 1)};
}
static_assert(linear_trigger_report(1, 0xff)[5] == 0x15 && linear_trigger_report(0, 0)[5] == 0);

struct DeviceChoice {
    USHORT product = 0;
    std::string path;
    const char* transport = "usb";
    const char* access = "grip";
};

struct ControlProbe {
    HANDLE handle = INVALID_HANDLE_VALUE;
    const char* status = "open";
    DWORD error = 0;
    HIDP_CAPS caps{};
};

ControlProbe probe_control(const char* path, USHORT product) {
    ControlProbe probe;
    constexpr auto share = FILE_SHARE_READ | FILE_SHARE_WRITE;
    HANDLE handle = CreateFileA(path, GENERIC_WRITE | GENERIC_READ, share, nullptr, OPEN_EXISTING, 0, nullptr);
    if (handle == INVALID_HANDLE_VALUE)
        handle = CreateFileA(path, GENERIC_WRITE, share, nullptr, OPEN_EXISTING, 0, nullptr);
    if (handle == INVALID_HANDLE_VALUE) {
        probe.error = GetLastError();
        return probe;
    }
    HIDD_ATTRIBUTES attrs{sizeof(attrs)};
    PHIDP_PREPARSED_DATA parsed = nullptr;
    probe.status = "metadata";
    if (HidD_GetAttributes(handle, &attrs) && HidD_GetPreparsedData(handle, &parsed)) {
        const auto status = HidP_GetCaps(parsed, &probe.caps);
        HidD_FreePreparsedData(parsed);
        if (status != 0x00110000) probe.error = static_cast<DWORD>(status);
        else if (attrs.VendorID != kVendorId || attrs.ProductID != product ||
                 probe.caps.UsagePage != kUsagePage || probe.caps.Usage != kUsage) probe.status = "interface";
        else if (probe.caps.OutputReportByteLength < kReportLen) probe.status = "length";
        else {
            probe.status = "ready";
            probe.handle = handle;
            return probe;
        }
    } else probe.error = GetLastError();
    CloseHandle(handle);
    return probe;
}

const char* device_transport(DEVINST instance) {
    constexpr DEVPROPKEY description{{0x540b947e, 0x8b40, 0x45bc,
        {0xa8, 0xa2, 0x6a, 0x0b, 0x89, 0x4c, 0xbd, 0xa2}}, 4};
    for (unsigned depth = 0; depth < 5; ++depth) {
        wchar_t text[256]{};
        DEVPROPTYPE type{};
        ULONG size = sizeof(text);
        if (CM_Get_DevNode_PropertyW(instance, &description, &type,
                reinterpret_cast<BYTE*>(text), &size, 0) == CR_SUCCESS && type == DEVPROP_TYPE_STRING) {
            _wcslwr(text);
            if (std::wcsstr(text, L"dongle") || std::wcsstr(text, L"receiver")) return "receiver";
        }
        if (CM_Get_Device_IDW(instance, text, ARRAYSIZE(text), 0) == CR_SUCCESS &&
                (_wcsnicmp(text, L"BTHENUM", 7) == 0 || _wcsnicmp(text, L"BTHLE", 5) == 0)) return "bluetooth";
        DEVINST parent{};
        if (CM_Get_Parent(&parent, instance, 0) != CR_SUCCESS) break;
        instance = parent;
    }
    return "usb";
}

std::vector<DeviceChoice> betop_devices() {
    std::vector<DeviceChoice> devices;
    GUID guid{};
    HidD_GetHidGuid(&guid);
    const HDEVINFO set = SetupDiGetClassDevsA(&guid, nullptr, nullptr, DIGCF_PRESENT | DIGCF_DEVICEINTERFACE);
    if (set == INVALID_HANDLE_VALUE) return devices;
    SP_DEVICE_INTERFACE_DATA iface{sizeof(iface)};
    for (DWORD i = 0; SetupDiEnumDeviceInterfaces(set, nullptr, &guid, i, &iface); ++i) {
        DWORD size = 0;
        SetupDiGetDeviceInterfaceDetailA(set, &iface, nullptr, 0, &size, nullptr);
        if (!size) continue;
        std::vector<char> storage(size);
        auto* detail = reinterpret_cast<SP_DEVICE_INTERFACE_DETAIL_DATA_A*>(storage.data());
        detail->cbSize = sizeof(*detail);
        SP_DEVINFO_DATA node{sizeof(node)};
        if (!SetupDiGetDeviceInterfaceDetailA(set, &iface, detail, size, nullptr, &node)) continue;
        std::string path = detail->DevicePath;
        std::string lower = path;
        std::transform(lower.begin(), lower.end(), lower.begin(),
            [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
        const auto start = lower.find("vid_20bc&pid_");
        if (start == std::string::npos || lower.size() < start + 17) continue;
        const auto product = static_cast<USHORT>(std::strtoul(lower.substr(start + 13, 4).c_str(), nullptr, 16));
        if (!product) continue;

        char ids[1024]{};
        bool control = false;
        if (SetupDiGetDeviceRegistryPropertyA(set, &node, SPDRP_HARDWAREID, nullptr,
                reinterpret_cast<BYTE*>(ids), sizeof(ids), nullptr)) {
            for (const char* id = ids; *id; id += std::strlen(id) + 1)
                if (_stricmp(id, "HID_DEVICE_UP:00FF_U:0003") == 0) control = true;
        }
        const bool candidate = control || lower.find("&mi_01&col01") != std::string::npos;
        const char* access = "grip";
        if (supports_control(product)) {
            const auto probe = probe_control(path.c_str(), product);
            access = probe.status;
            if (probe.handle != INVALID_HANDLE_VALUE) CloseHandle(probe.handle);
            else if (!candidate) continue;
        } else if (!candidate) continue;
        devices.push_back({product, std::move(path), device_transport(node.DevInst), access});
    }
    SetupDiDestroyDeviceInfoList(set);
    return devices;
}

class Controller {
    HANDLE handle = INVALID_HANDLE_VALUE;
    bool clear_shoulders;
    bool direct_shoulders;
    bool auto_cleanup;
    USHORT product = 0;
    std::vector<unsigned char> report;

    void attach(const ControlProbe& probe, USHORT id) {
        report.resize(probe.caps.OutputReportByteLength);
        handle = probe.handle;
        product = id;
    }

public:
    Controller(bool clear_shoulders, bool direct_shoulders, bool auto_cleanup = true)
        : clear_shoulders(clear_shoulders), direct_shoulders(direct_shoulders), auto_cleanup(auto_cleanup) {}
    bool open(const std::string& selected_path = {}, USHORT selected_product = 0) {
        if (!selected_path.empty()) {
            const auto probe = probe_control(selected_path.c_str(), selected_product);
            if (probe.handle != INVALID_HANDLE_VALUE) {
                attach(probe, selected_product);
                return true;
            }
            std::fprintf(stderr, "CONTROL %s %lu %04X %04X %u\n", probe.status,
                (unsigned long)probe.error, probe.caps.UsagePage, probe.caps.Usage, probe.caps.OutputReportByteLength);
            return false;
        }
        for (const auto& candidate : betop_devices()) {
            if (auto_cleanup ? candidate.product != kProductId : !supports_control(candidate.product)) continue;
            const auto probe = probe_control(candidate.path.c_str(), candidate.product);
            if (probe.handle == INVALID_HANDLE_VALUE) continue;
            if (handle != INVALID_HANDLE_VALUE) {
                CloseHandle(probe.handle);
                CloseHandle(handle);
                handle = INVALID_HANDLE_VALUE;
                product = 0;
                return false;
            }
            attach(probe, candidate.product);
            if (!direct_shoulders) break;
        }
        return handle != INVALID_HANDLE_VALUE;
    }
    ~Controller() { if (handle != INVALID_HANDLE_VALUE) { if (auto_cleanup) stop(); CloseHandle(handle); } }
    bool ready() const { return handle != INVALID_HANDLE_VALUE; }
    USHORT product_id() const { return product; }

    bool send(const unsigned char* data) {
        if (handle == INVALID_HANDLE_VALUE) return false;
        std::memcpy(report.data(), data, kReportLen);
        const auto length = static_cast<DWORD>(report.size());
        DWORD written = 0;
        if (WriteFile(handle, report.data(), length, &written, nullptr) && written == length)
            return true;
        return HidD_SetOutputReport(handle, report.data(), length) != 0;
    }
    bool linear_trigger(unsigned side, unsigned char level) {
        return send(linear_trigger_report(side, level).data());
    }
    // 02 76 01 <ch> <a> <b> 0a + ten envelope bytes
    void shoulder(unsigned channel, unsigned step, const unsigned char env[kSteps]) {
        unsigned char p[kReportLen]{};
        p[0] = 2; p[1] = 0x76; p[2] = 1; p[3] = (unsigned char)channel;
        p[4] = p[5] = (unsigned char)step; p[6] = (unsigned char)kSteps;
        std::memcpy(p + 7, env, kSteps);
        send(p);
    }
    void stop() {
        if (direct_shoulders) {
            for (unsigned side : {0u, 1u}) {
                // A transient write error should not skip the other side.
                if (!shoulder_level(side, 0)) {
                    Sleep(30);
                    if (!shoulder_level(side, 0))
                        std::fprintf(stderr, "Could not clear shoulder channel %u\n", side);
                }
            }
            return;
        }
        if (clear_shoulders) {
            for (unsigned channel : {4u, 5u, 0u, 1u, 6u, 7u}) {
                unsigned char p[kReportLen]{};
                p[0] = 2; p[1] = 0x76; p[2] = 1; p[3] = (unsigned char)channel;
                send(p);
            }
        }
        for (unsigned side : {0u, 1u}) trigger_clear(side);
    }

    bool shoulder_level(unsigned side, unsigned char level) {
        unsigned char p[kReportLen]{};
        p[0] = 2; p[1] = 0x76; p[2] = 1;
        p[3] = (unsigned char)(4 + side);
        p[4] = level;
        return send(p);
    }

    // 02 56 <side> <mode> <start> <params...>. Verified by hand: all three
    // modes are distinguishable on the hardware.
    //   mode 1 race : start, force
    //   mode 2 gun  : start, force, initial force, frequency
    //   mode 3 shot : start, force, range
    bool trigger_gun(unsigned side, unsigned start, unsigned force, unsigned frequency) {
        unsigned char p[kReportLen]{};
        p[0] = 2; p[1] = 0x56; p[2] = (unsigned char)side;
        p[3] = 2;
        p[4] = (unsigned char)start;
        p[5] = p[6] = (unsigned char)std::clamp(force, 1u, 0x50u);
        p[7] = (unsigned char)std::clamp(frequency, 1u, 0x3Fu);
        return send(p);
    }
    bool trigger_race(unsigned side, unsigned start, unsigned force) {
        unsigned char p[kReportLen]{};
        p[0] = 2; p[1] = 0x56; p[2] = (unsigned char)side;
        p[3] = 1;
        p[4] = (unsigned char)start;
        p[5] = (unsigned char)std::clamp(force, 1u, 0x50u);
        return send(p);
    }
    bool trigger_clear(unsigned side) {
        unsigned char p[kReportLen]{};
        p[0] = 2; p[1] = 0x56; p[2] = (unsigned char)side;
        return send(p);
    }
};

// ----------------------------------------------------------------- DSP parts

// RBJ band-pass biquad. Used only to split energy for envelope extraction,
// so phase response does not matter.
struct Biquad {
    float b0 = 1, b1 = 0, b2 = 0, a1 = 0, a2 = 0;
    float x1 = 0, x2 = 0, y1 = 0, y2 = 0;
    void band(float low_hz, float high_hz, float rate) {
        const float centre = std::sqrt(low_hz * high_hz);
        const float q = centre / std::max(1.f, high_hz - low_hz);
        const float w = 2.f * (float)M_PI * centre / rate;
        const float alpha = std::sin(w) / (2.f * q);
        const float a0 = 1 + alpha;
        b0 = alpha / a0; b1 = 0; b2 = -alpha / a0;
        a1 = -2.f * std::cos(w) / a0; a2 = (1 - alpha) / a0;
    }
    float operator()(float x) {
        const float y = b0 * x + b1 * x1 + b2 * x2 - a1 * y1 - a2 * y2;
        x2 = x1; x1 = x; y2 = y1; y1 = y;
        return y;
    }
};

struct BandSpec { float low_hz, high_hz, attack_ms, release_ms, gain_db, threshold_db; };

// Mirrors grip_dsp/hpss-balanced.json: three bands over 1 kHz - 12 kHz with
// progressively faster release and higher gain toward the top.
constexpr BandSpec kBands[3] = {
    {1000,  3000, 1, 20, 6,  -32},
    {3000,  6000, 1, 10, 8,  -35},
    {6000, 12000, 1,  5, 10, -35},
};

inline float db_to_gain(float db) { return std::pow(10.f, db / 20.f); }

class Band {
    Biquad filter;
    float fast = 0, slow = 0;
    float attack = 0, release = 0, slow_coeff = 0;
    float gain = 1, threshold = 0;
public:
    void prepare(const BandSpec& spec, float rate, float harmonic_window_ms) {
        filter.band(spec.low_hz, spec.high_hz, rate);
        attack  = std::exp(-1.f / (spec.attack_ms  * rate / 1000.f));
        release = std::exp(-1.f / (spec.release_ms * rate / 1000.f));
        slow_coeff = std::exp(-1.f / (harmonic_window_ms * rate / 1000.f));
        gain = db_to_gain(spec.gain_db);
        threshold = db_to_gain(spec.threshold_db);
    }
    // Returns the percussive part of this band: the fast envelope with the
    // slowly-tracked harmonic floor (music, engine drone, speech) removed.
    float process(float x, float harmonic) {
        const float magnitude = std::fabs(filter(x));
        const float coeff = magnitude > fast ? attack : release;
        fast = magnitude + coeff * (fast - magnitude);
        slow = magnitude + slow_coeff * (slow - magnitude);
        const float percussive = fast - harmonic * slow;
        if (percussive <= threshold) return 0.f;
        return (percussive - threshold) * gain;
    }
    void reset() { fast = slow = 0; }
};

struct Options {
    float harmonic = 1.0f;     // 0 = keep music, 1.5 = aggressive suppression
    float sensitivity = 1.0f;  // user gain before the limiter
    bool input_sensitivity = false;
    float gate = 0.02f;        // gate threshold on the normalised envelope
    float target = 0.35f;      // auto-gain target level
    float auto_gain_max = 24.f;// dB of makeup the auto-gain may apply
    unsigned step = 0x28;      // shoulder bytes 4/5: effect length, ~step*25 ms
    unsigned refresh_ms = 50;  // packet cadence; also the envelope latency
    bool auto_gain = true;
    bool dry_run = false;

    // Triggers are the part that is verified working on this hardware, so
    // they are on by default. Resting resistance is always present, and the
    // audio level rides on top of it.
    bool triggers = true;
    bool shoulder = true;
    bool shoulder_live = false;
    bool trigger_impact = false;
    unsigned trigger_base = 10;        // resting resistance, 1..0x50
    unsigned trigger_max = 0x50;       // force at full audio level
    unsigned trigger_interval_ms = 40; // minimum gap between trigger updates
};

// One shot per rising audio envelope. Hold a brief gun effect, then restore
// the race resistance on a wall-clock deadline, even if capture goes silent.
class TriggerImpact {
    bool initialized = false;
    bool armed = true;
    ULONGLONG end_at = 0;
public:
    enum class Action { None, Base, Kick };

    Action update(float peak, ULONGLONG now) {
        if (!initialized) {
            initialized = true;
            return Action::Base;
        }
        if (end_at) {
            if (now < end_at) return Action::None;
            end_at = 0;
            return Action::Base;
        }
        if (peak < 0.08f) armed = true;
        if (!armed || peak < 0.20f) return Action::None;
        armed = false;
        end_at = now + 90;
        return Action::Kick;
    }
};

// One physical side.
class SideDsp {
    std::array<Band, 3> bands;
    float gate_gain = 0, gate_open_coeff = 0, gate_close_coeff = 0;
    float peak_hold = 0, peak_decay = 0;
    float makeup = 1;
public:
    float level = 0;   // post-gain envelope, for the meter
    float raw = 0;     // pre-gain envelope, for the meter

    void prepare(float rate) {
        for (size_t i = 0; i < bands.size(); ++i) bands[i].prepare(kBands[i], rate, 300.f);
        // DS5 uses roughly 5 ms to open and 60 ms to close so the gate cannot
        // chatter around the threshold and chop the output.
        gate_open_coeff  = std::exp(-1.f / (5.f  * rate / 1000.f));
        gate_close_coeff = std::exp(-1.f / (60.f * rate / 1000.f));
        // Auto-gain peak tracker: instant attack, ~10 s release, matching the
        // reference's "very slow release keeps the feel stable" setting.
        peak_decay = std::exp(-1.f / (10.f * rate));
    }

    float process(float x, const Options& options) {
        if (options.input_sensitivity) x *= options.sensitivity;
        float sum = 0;
        for (auto& band : bands) sum += band.process(x, options.harmonic);
        raw = sum;

        if (options.auto_gain) {
            peak_hold = std::max(sum, peak_hold * peak_decay);
            const float limit = db_to_gain(options.auto_gain_max);
            makeup = peak_hold > 1e-6f ? std::clamp(options.target / peak_hold, 1.f, limit) : 1.f;
        } else {
            makeup = 1.f;
        }

        float value = sum * makeup * (options.input_sensitivity ? 1.f : options.sensitivity);

        // Smoothed gate rather than a hard threshold.
        const float wanted = value > options.gate ? 1.f : 0.f;
        const float coeff = wanted > gate_gain ? gate_open_coeff : gate_close_coeff;
        gate_gain = wanted + coeff * (gate_gain - wanted);
        value *= gate_gain;

        // Soft knee, as the reference Composer does before the USB layer.
        constexpr float knee = 0.6f;
        if (value > knee) value = knee + (1.f - knee) * std::tanh((value - knee) / (1.f - knee));
        level = std::clamp(value, 0.f, 1.f);
        return level;
    }
    void reset() { for (auto& band : bands) band.reset(); gate_gain = 0; peak_hold = 0; }
};

// Collects the envelope into packet-sized groups of ten peak-held slots.
class Packer {
    std::array<unsigned char, kSteps> slots{};
    unsigned slot_index = 0, samples_in_slot = 0, samples_per_slot = 1;
    float slot_peak = 0;
public:
    void prepare(float rate, unsigned refresh_ms) {
        samples_per_slot = std::max(1u, (unsigned)(rate * refresh_ms / 1000.f / kSteps));
        slot_index = samples_in_slot = 0; slot_peak = 0;
        slots.fill(0);
    }
    // Returns true when a full packet is ready in `out`.
    bool push(float value, unsigned char (&out)[kSteps]) {
        slot_peak = std::max(slot_peak, value);
        if (++samples_in_slot < samples_per_slot) return false;
        slots[slot_index] = (unsigned char)std::clamp(slot_peak * 255.f, 0.f, 255.f);
        samples_in_slot = 0; slot_peak = 0;
        if (++slot_index < kSteps) return false;
        slot_index = 0;
        std::memcpy(out, slots.data(), kSteps);
        return true;
    }
};

#include "KP70StudioOutput.h"

struct CurveMix {
    bool enabled = false;
    std::array<float, kCurveBands> weights{};

    void read(const StudioChannel& channel) {
        enabled = channel.custom_curve != 0;
        for (size_t i = 0; i < weights.size(); ++i)
            weights[i] = std::clamp(channel.curve[i], 0, 100) / 100.f;
    }
};

class CurveSpectrum {
    struct BandState {
        Biquad filter;
        float fast = 0, slow = 0;
        bool available = false;
    };
    std::array<BandState, kCurveBands> bands{};
    float attack = 0, release = 0, average = 0;

public:
    void prepare(float rate) {
        constexpr std::array<float, kCurveBands> centres{40, 100, 250, 630, 1600, 4000, 10000};
        attack = std::exp(-1.f / (.005f * rate));
        release = std::exp(-1.f / (.045f * rate));
        average = std::exp(-1.f / (.12f * rate));
        for (size_t i = 0; i < bands.size(); ++i) {
            const float low = centres[i] / 1.58f;
            const float high = std::min(centres[i] * 1.58f, rate * .47f);
            bands[i].available = low < high;
            if (bands[i].available) bands[i].filter.band(low, high, rate);
        }
    }

    void update(float sample) {
        for (auto& band : bands) {
            if (!band.available) continue;
            const float magnitude = std::fabs(band.filter(sample));
            const float coefficient = magnitude > band.fast ? attack : release;
            band.fast = magnitude + coefficient * (band.fast - magnitude);
            band.slow = magnitude + average * (band.slow - magnitude);
        }
    }

    float level(const CurveMix& curve, float sensitivity, float suppression) const {
        float sum = 0;
        for (size_t i = 0; i < bands.size(); ++i)
            sum += curve.weights[i] * std::max(0.f, bands[i].fast - suppression * bands[i].slow);
        return std::clamp(sum * sensitivity * 6.f, 0.f, 1.f);
    }
};

// ------------------------------------------------------------ WASAPI capture

struct ComScope {
    bool ok;
    ComScope() { ok = SUCCEEDED(CoInitializeEx(nullptr, COINIT_MULTITHREADED)); }
    ~ComScope() { if (ok) CoUninitialize(); }
};
template <class T> struct Com {
    T* p = nullptr;
    ~Com() { if (p) p->Release(); }
    T** operator&() { return &p; }
    T* operator->() const { return p; }
    explicit operator bool() const { return p != nullptr; }
};

volatile bool g_stop = false;
BOOL WINAPI on_console_signal(DWORD) { g_stop = true; return TRUE; }

int run(Options options, unsigned seconds, StudioSession* studio = nullptr,
        USHORT product = 0, const std::string& path = {}) {
    ComScope com;
    if (!com.ok) { std::fprintf(stderr, "COM init failed\n"); return 1; }

    Com<IMMDeviceEnumerator> enumerator;
    if (FAILED(CoCreateInstance(__uuidof(MMDeviceEnumerator), nullptr, CLSCTX_ALL,
                                __uuidof(IMMDeviceEnumerator), (void**)&enumerator))) {
        std::fprintf(stderr, "Could not create the audio device enumerator\n"); return 1;
    }
    Com<IMMDevice> endpoint;
    if (FAILED(enumerator->GetDefaultAudioEndpoint(eRender, eConsole, &endpoint))) {
        std::fprintf(stderr, "No default playback device\n"); return 1;
    }
    Com<IAudioClient> client;
    if (FAILED(endpoint->Activate(__uuidof(IAudioClient), CLSCTX_ALL, nullptr, (void**)&client))) {
        std::fprintf(stderr, "Could not activate the audio client\n"); return 1;
    }
    WAVEFORMATEX* mix = nullptr;
    if (FAILED(client->GetMixFormat(&mix))) {
        std::fprintf(stderr, "Could not read the mix format\n"); return 1;
    }
    const unsigned rate = mix->nSamplesPerSec, channels = mix->nChannels;
    const bool is_float = mix->wFormatTag == WAVE_FORMAT_IEEE_FLOAT ||
        (mix->wFormatTag == WAVE_FORMAT_EXTENSIBLE &&
         reinterpret_cast<WAVEFORMATEXTENSIBLE*>(mix)->SubFormat == KSDATAFORMAT_SUBTYPE_IEEE_FLOAT);
    const unsigned bits = mix->wBitsPerSample;

    if (FAILED(client->Initialize(AUDCLNT_SHAREMODE_SHARED, AUDCLNT_STREAMFLAGS_LOOPBACK,
                                  10000000, 0, mix, nullptr))) {
        std::fprintf(stderr, "Could not open loopback capture\n");
        CoTaskMemFree(mix); return 1;
    }
    Com<IAudioCaptureClient> capture;
    if (FAILED(client->GetService(__uuidof(IAudioCaptureClient), (void**)&capture))) {
        std::fprintf(stderr, "Could not get the capture service\n");
        CoTaskMemFree(mix); return 1;
    }

    Controller controller(!options.trigger_impact, options.shoulder_live || studio, !studio);
    const bool needs_hid = !studio || (supports_control(product) && !path.empty());
    const bool hardware = needs_hid && !options.dry_run &&
        (!options.trigger_impact || options.triggers) && controller.open(path, product);
    if ((options.shoulder_live || (studio && needs_hid)) && !options.dry_run && !hardware) {
        if (!studio) std::fprintf(stderr, "Could not open the selected control interface\n");
        CoTaskMemFree(mix);
        return 1;
    }
    if (!studio && !options.dry_run && !hardware)
        std::fprintf(stderr, "Controller not found - running without haptic output.\n"
                             "Connect the 2.4G receiver and close the Beitong software.\n");

    std::array<SideDsp, 2> dsp;
    std::array<Packer, 2> packer;
    for (int side = 0; side < 2; ++side) { dsp[side].prepare((float)rate); packer[side].prepare((float)rate, options.refresh_ms); }
    std::unique_ptr<StudioOutput> studio_output;
    StudioSettings studio_settings{};
    std::array<SideDsp, 2> trigger_dsp;
    std::array<Options, 2> shoulder_options{options, options}, trigger_options{options, options};
    std::array<CurveMix, 6> curves;
    std::array<CurveSpectrum, 2> spectra;
    std::array<Biquad, 2> grip_filter;
    std::array<float, 2> grip_env{}, grip_peak{}, trigger_peak{};
    std::array<float, 2> studio_audio{}, studio_grip{}, studio_trigger{};
    float input_peak = 0;
    const float grip_attack = std::exp(-1.f / (0.008f * rate));
    const float grip_release = std::exp(-1.f / (0.070f * rate));
    std::array<float, 2> grip_sensitivity{1.f, 1.f};
    auto apply_audio_settings = [&] {
        const float suppression = std::clamp(studio_settings.suppression, 0, 150) / 100.f;
        const auto sensitivity = [](const StudioChannel& channel) {
            return std::clamp(channel.sensitivity, 10, 250) / 100.f;
        };
        for (unsigned side = 0; side < 2; ++side) {
            const auto& shoulder = studio_settings.channels[side];
            shoulder_options[side].sensitivity = sensitivity(shoulder);
            shoulder_options[side].gate = std::clamp(shoulder.threshold, 0, 100) / 100.f;
            shoulder_options[side].harmonic = trigger_options[side].harmonic = suppression;
            trigger_options[side].sensitivity = sensitivity(studio_settings.channels[4 + side]);
            grip_sensitivity[side] = sensitivity(studio_settings.channels[2 + side]);
        }
        for (size_t i = 0; i < curves.size(); ++i) curves[i].read(studio_settings.channels[i]);
    };
    if (studio) {
        const auto devices = options.dry_run ? std::vector<DeviceChoice>{} : betop_devices();
        const auto same_model = std::count_if(devices.begin(), devices.end(),
            [product](const DeviceChoice& device) { return device.product == product; });
        studio_output = std::make_unique<StudioOutput>(controller, options.dry_run, product, same_model > 1);
        studio->read(studio_settings);
        for (unsigned side = 0; side < 2; ++side) {
            shoulder_options[side].input_sensitivity = trigger_options[side].input_sensitivity = true;
            trigger_options[side].gate = 0.f;
            trigger_dsp[side].prepare((float)rate);
            spectra[side].prepare((float)rate);
        }
        apply_audio_settings();
        grip_filter[0].band(35.f, 190.f, (float)rate);
        grip_filter[1].band(85.f, 330.f, (float)rate);
    }

    std::printf("Capturing system audio: %u Hz, %u channels, %s%u\n",
                rate, channels, is_float ? "float" : "int", bits);
    std::printf("Haptics: %s | step 0x%02x | refresh %u ms | harmonic %.2f | sensitivity %.2f\n",
                hardware ? "on" : "OFF", options.step, options.refresh_ms,
                options.harmonic, options.sensitivity);
    std::printf("Ctrl+C to stop.\n\n");

    SetConsoleCtrlHandler(on_console_signal, TRUE);
    if (FAILED(client->Start())) {
        std::fprintf(stderr, "Could not start loopback capture\n");
        CoTaskMemFree(mix);
        return 1;
    }
    if (studio) { std::puts("READY"); std::fflush(stdout); }

    const ULONGLONG deadline = seconds ? GetTickCount64() + seconds * 1000ull : 0;
    ULONGLONG next_meter = 0, last_packet = 0, next_trigger = 0;
    unsigned last_force[2] = {9999, 9999};
    std::array<TriggerImpact, 2> impact;
    std::array<float, 2> last_impact_peak{};
    std::array<float, 2> max_input{}, max_envelope{};
    ULONGLONG last_audio = 0;
    bool output_failed = false;
    std::array<float, 2> shoulder_peak{};
    std::array<unsigned char, 2> last_level{};
    std::array<ULONGLONG, 2> shoulder_sent_at{};
    ULONGLONG next_shoulder = 0;
    ULONGLONG last_shoulder_audio = 0;
    bool shoulder_has_frames = false;
    unsigned nonzero_shoulder_frames[2]{};
    unsigned char max_shoulder_level[2]{};
    unsigned char env[2][kSteps]{};
    bool pending[2] = {false, false};
    unsigned long long frames_seen = 0;

    while (!g_stop && !output_failed && (!deadline || GetTickCount64() < deadline) &&
            (!studio || !studio->ended())) {
        UINT32 available = 0;
        std::array<float, 2> peak{};
        bool fresh_audio = false;
        if (FAILED(capture->GetNextPacketSize(&available)) ) break;
        if (!available) { Sleep(2); }
        while (available) {
            BYTE* data = nullptr; UINT32 frames = 0; DWORD flags = 0;
            if (FAILED(capture->GetBuffer(&data, &frames, &flags, nullptr, nullptr))) break;
            const bool silent = (flags & AUDCLNT_BUFFERFLAGS_SILENT) != 0;
            for (UINT32 i = 0; i < frames; ++i) {
                float left = 0, right = 0;
                if (!silent && data) {
                    if (is_float) {
                        const float* f = reinterpret_cast<const float*>(data) + (size_t)i * channels;
                        left = f[0]; right = channels > 1 ? f[1] : f[0];
                    } else if (bits == 16) {
                        const int16_t* s = reinterpret_cast<const int16_t*>(data) + (size_t)i * channels;
                        left = s[0] / 32768.f; right = channels > 1 ? s[1] / 32768.f : left;
                    }
                }
                const float out_l = dsp[0].process(left, studio ? shoulder_options[0] : options);
                const float out_r = dsp[1].process(right, studio ? shoulder_options[1] : options);
                if (options.trigger_impact) {
                    peak[0] = std::max(peak[0], out_l);
                    peak[1] = std::max(peak[1], out_r);
                    if (options.dry_run) {
                        max_input[0] = std::max(max_input[0], std::fabs(left));
                        max_input[1] = std::max(max_input[1], std::fabs(right));
                        max_envelope[0] = std::max(max_envelope[0], out_l);
                        max_envelope[1] = std::max(max_envelope[1], out_r);
                    }
                }
                if (studio) {
                    input_peak = std::max(input_peak, std::max(std::fabs(left), std::fabs(right)));
                    const float samples[]{left, right};
                    const float shoulder_levels[]{out_l, out_r};
                    for (unsigned side = 0; side < 2; ++side) {
                        const auto& shoulder_curve = curves[side];
                        const auto& grip_curve = curves[2 + side];
                        const auto& trigger_curve = curves[4 + side];
                        auto& spectrum = spectra[side];
                        if (shoulder_curve.enabled || grip_curve.enabled || trigger_curve.enabled)
                            spectrum.update(samples[side]);
                        const float shoulder = shoulder_curve.enabled
                            ? spectrum.level(shoulder_curve, shoulder_options[side].sensitivity, shoulder_options[side].harmonic)
                            : shoulder_levels[side];
                        const float v = std::fabs(grip_filter[side](samples[side]));
                        const float coefficient = v > grip_env[side] ? grip_attack : grip_release;
                        grip_env[side] = v + coefficient * (grip_env[side] - v);
                        const float grip = grip_curve.enabled ? spectrum.level(grip_curve, grip_sensitivity[side], 0.f)
                            : std::clamp(grip_env[side] * 6.f * grip_sensitivity[side], 0.f, 1.f);
                        const float original_trigger = trigger_dsp[side].process(samples[side], trigger_options[side]);
                        const float trigger = trigger_curve.enabled
                            ? spectrum.level(trigger_curve, trigger_options[side].sensitivity, trigger_options[side].harmonic)
                            : original_trigger;
                        shoulder_peak[side] = std::max(shoulder_peak[side], shoulder);
                        grip_peak[side] = std::max(grip_peak[side], grip);
                        trigger_peak[side] = std::max(trigger_peak[side], trigger);
                    }
                } else if (options.shoulder_live) {
                    shoulder_peak[0] = std::max(shoulder_peak[0], out_l);
                    shoulder_peak[1] = std::max(shoulder_peak[1], out_r);
                } else {
                    if (packer[0].push(out_l, env[0])) pending[0] = true;
                    if (packer[1].push(out_r, env[1])) pending[1] = true;
                }
            }
            frames_seen += frames;
            fresh_audio = fresh_audio || frames != 0;
            capture->ReleaseBuffer(frames);
            if (FAILED(capture->GetNextPacketSize(&available))) break;
        }

        const ULONGLONG now = GetTickCount64();

        if ((options.shoulder_live || studio) && fresh_audio) {
            last_shoulder_audio = now;
            shoulder_has_frames = true;
        }
        if (studio && now >= next_shoulder) {
            studio->read(studio_settings);
            apply_audio_settings();
            if (shoulder_has_frames || now - last_shoulder_audio >= 45) {
                studio_audio = shoulder_peak;
                studio_grip = grip_peak;
                studio_trigger = trigger_peak;
            }
            if (!studio_output->update(studio_settings, studio_audio, studio_trigger, studio_grip, now)) {
                std::fprintf(stderr, "Controller output failed; reconnect the controller and restart.\n");
                output_failed = true;
            }
            if (now >= next_meter) {
                const int* m = studio_output->meters;
                std::printf("STUDIO %d %d %d %d %d %d %d %d %d %d\n", studio_output->pad_ready ? 1 : 0,
                    (int)std::lround(std::clamp(input_peak, 0.f, 1.f) * 100),
                    m[0], m[1], m[2], m[3], m[4], m[5], studio_output->trigger_modes[0], studio_output->trigger_modes[1]);
                std::fflush(stdout);
                input_peak = 0;
                next_meter = now + 80;
            }
            shoulder_peak = {};
            shoulder_has_frames = false;
            grip_peak = {};
            trigger_peak = {};
            next_shoulder = now + 20;
            continue;
        }
        if (studio) continue;


        // The direct report changes the active level. Collapse any audio
        // backlog into one target per side and never replay old frames.
        if (options.shoulder_live && now >= next_shoulder &&
            (shoulder_has_frames || now - last_shoulder_audio >= 45)) {
            for (unsigned side = 0; side < 2; ++side) {
                const float value = shoulder_peak[side];
                const auto level = value < options.gate ? (unsigned char)0
                    : (unsigned char)std::lround(value * 0xc0);
                if (level) ++nonzero_shoulder_frames[side];
                max_shoulder_level[side] = std::max(max_shoulder_level[side], level);
                const bool changed = level != last_level[side];
                const bool refresh = level && now - shoulder_sent_at[side] >= 250;
                if (!changed && !refresh) continue;
                if (hardware && !controller.shoulder_level(side, level)) {
                    std::fprintf(stderr, "Shoulder channel %u: output write failed\n", side);
                    output_failed = true;
                    break;
                }
                last_level[side] = level;
                shoulder_sent_at[side] = now;
            }
            shoulder_peak = {};
            shoulder_has_frames = false;
            next_shoulder = now + 20;
        }

        // Shoulder: the effect is armed here and the controller plays it when
        // the bumper is pressed, so what this does is keep the waveform that
        // the next press will produce matched to the current audio.
        if (pending[0] || pending[1]) {
            if (hardware && options.shoulder) {
                if (pending[0]) controller.shoulder(kLeftChannel, options.step, env[0]);
                if (pending[1]) controller.shoulder(kRightChannel, options.step, env[1]);
            }
            pending[0] = pending[1] = false;
            last_packet = now;
        }

        // Triggers: a resting resistance with the audio level riding on top,
        // refreshed only when it actually moves so the link is not flooded.
        if (options.trigger_impact && options.triggers) {
            if (fresh_audio) {
                last_impact_peak = peak;
                last_audio = now;
            } else if (now - last_audio >= 50) {
                last_impact_peak = {};
            }
            for (unsigned side = 0; side < 2; ++side) {
                const auto action = impact[side].update(last_impact_peak[side], now);
                if (action == TriggerImpact::Action::None) continue;
                const bool kick = action == TriggerImpact::Action::Kick;
                if (hardware && !(kick ? controller.trigger_gun(side, 0, options.trigger_max, 20)
                                       : controller.trigger_race(side, 0, options.trigger_base))) {
                    std::fprintf(stderr, "Trigger channel %u: output write failed\n", side);
                    output_failed = true;
                    break;
                }
                std::printf("\nTrigger channel %u: %s%s\n", side, kick ? "impact" : "base resistance",
                    hardware ? "" : " (simulation)");
            }
        } else if (hardware && options.triggers && now >= next_trigger) {
            for (unsigned side = 0; side < 2; ++side) {
                const float level = dsp[side].level;
                const unsigned span = options.trigger_max > options.trigger_base
                                    ? options.trigger_max - options.trigger_base : 0;
                const unsigned force = options.trigger_base + (unsigned)(level * span);
                const unsigned frequency = 8 + (unsigned)(level * 0x18);
                if (force > last_force[side] + 1 || force + 1 < last_force[side]) {
                    controller.trigger_gun(side, 0, force, frequency);
                    last_force[side] = force;
                }
            }
            next_trigger = now + options.trigger_interval_ms;
        }

        // Silence: let the shoulder effect lapse rather than leaving it armed.
        if (hardware && last_packet && now - last_packet > 400) {
            if (options.shoulder) {
                unsigned char quiet[kSteps]{};
                controller.shoulder(kLeftChannel, options.step, quiet);
                controller.shoulder(kRightChannel, options.step, quiet);
            }
            last_packet = 0;
        }

        if (now >= next_meter) {
            auto bar = [](float value) {
                static char text[21];
                const int filled = (int)std::clamp(value * 20.f, 0.f, 20.f);
                for (int i = 0; i < 20; ++i) text[i] = i < filled ? '#' : '.';
                text[20] = 0; return text;
            };
            std::printf("\rL %s %3.0f%%   R %s %3.0f%%  ",
                        bar(dsp[0].level), dsp[0].level * 100,
                        bar(dsp[1].level), dsp[1].level * 100);
            std::fflush(stdout);
            next_meter = now + 60;
        }
    }

    client->Stop();
    if (hardware && !studio) controller.stop();
    studio_output.reset();
    CoTaskMemFree(mix);
    std::printf("\nStopped. Frames processed: %llu\n", frames_seen);
    if (options.trigger_impact && options.dry_run) {
        std::printf("Input peak L/R %.3f / %.3f; trigger envelope L/R %.3f / %.3f\n",
            max_input[0], max_input[1], max_envelope[0], max_envelope[1]);
    }
    if (options.shoulder_live && options.dry_run) {
        std::printf("Shoulder simulation L/R: active frames %u / %u; peak level %u / %u\n",
            nonzero_shoulder_frames[0], nonzero_shoulder_frames[1],
            (unsigned)max_shoulder_level[0], (unsigned)max_shoulder_level[1]);
    }
    return output_failed ? 1 : 0;
}

// Feeds synthetic signals straight through the DSP so the analysis can be
// checked without an audio device or the controller. The point of the new
// design is that sustained tones should stay quiet while transients come
// through, so the test asserts exactly that ordering.
int self_test() {
    constexpr float rate = 48000;
    Options options;
    struct Case { const char* name; int kind; float expect_min, expect_max; };
    const Case cases[] = {
        {"silence",              0, 0.00f, 0.01f},
        {"80 Hz sustained bass", 1, 0.00f, 0.05f},
        {"1 kHz sustained tone", 2, 0.00f, 0.35f},
        {"broadband clicks",     3, 0.15f, 1.01f},
    };
    bool pass = true;
    for (const auto& item : cases) {
        SideDsp dsp; dsp.prepare(rate);
        float peak = 0;
        const unsigned total = (unsigned)(rate * 2);
        unsigned seed = 12345;
        for (unsigned i = 0; i < total; ++i) {
            const float t = i / rate;
            float x = 0;
            switch (item.kind) {
            case 1: x = 0.5f * std::sin(2.f * (float)M_PI * 80.f * t); break;
            case 2: x = 0.5f * std::sin(2.f * (float)M_PI * 1000.f * t); break;
            case 3:
                // A sharp broadband burst every 400 ms.
                if ((i % (unsigned)(rate * 0.4f)) < (unsigned)(rate * 0.004f)) {
                    seed = seed * 1664525u + 1013904223u;
                    x = ((seed >> 9) / (float)(1 << 22) - 1.f) * 0.8f;
                }
                break;
            default: break;
            }
            const float y = dsp.process(x, options);
            if (i > rate * 0.5f) peak = std::max(peak, y); // skip settling
        }
        const bool ok = peak >= item.expect_min && peak <= item.expect_max;
        pass = pass && ok;
        std::printf("  %-22s peak %.3f   expect %.2f..%.2f   %s\n",
                    item.name, peak, item.expect_min, item.expect_max, ok ? "ok" : "FAIL");
    }
    TriggerImpact impact;
    using Action = TriggerImpact::Action;
    const bool trigger_ok = impact.update(0, 0) == Action::Base &&
        impact.update(0.5f, 10) == Action::Kick &&
        impact.update(0, 99) == Action::None &&
        impact.update(0.5f, 100) == Action::Base &&
        impact.update(0.5f, 120) == Action::None &&
        impact.update(0, 130) == Action::None &&
        impact.update(0.5f, 140) == Action::Kick &&
        impact.update(0, 230) == Action::Base;
    std::printf("  trigger impact timing and rearm   %s\n", trigger_ok ? "ok" : "FAIL");
    pass = pass && trigger_ok;
    std::printf("%s\n", pass ? "self-test passed" : "self-test FAILED");
    return pass ? 0 : 1;
}

} // namespace

int main(int argc, char** argv) {
    Options options;
    unsigned seconds = 0;

    if (argc == 2 && std::string_view(argv[1]) == "--devices") {
        const auto devices = betop_devices();
        std::array<PadCapabilities, XUSER_MAX_COUNT> input{};
        unsigned active = 0;
        for (DWORD slot = 0; slot < XUSER_MAX_COUNT; ++slot) {
            XINPUT_STATE state{};
            if (XInputGetState(slot, &state) == ERROR_SUCCESS) {
                ++active;
                input[slot] = pad_identity(slot);
            }
        }
        for (const auto& device : devices) {
            const bool exact = std::any_of(input.begin(), input.end(), [&](const PadCapabilities& pad) {
                return pad.vendor == kVendorId && pad.product == device.product;
            });
            std::printf("DEVICE\t%04X\t%s\t%s\t%d\t%s\t%d\n", device.product, device.transport,
                device.access,
                exact || (active == 1 && devices.size() == 1), device.path.c_str(), supported_kp70c(device.product));
        }
        for (const auto& pad : input) {
            if (pad.vendor != kVendorId || !pad.product) continue;
            if (std::none_of(devices.begin(), devices.end(), [&](const DeviceChoice& device) {
                return device.product == pad.product;
            })) {
                std::printf("DEVICE\t%04X\tunknown\tgrip\t1\t-\t0\n", pad.product);
            }
        }
        return 0;
    }

    if (argc == 2 && std::string_view(argv[1]) == "--device-info") {
        Controller device(false, true, false);
        if (!device.open()) { std::puts("No unique supported KP70 control interface"); return 1; }
        std::printf("Control interface %04x:%04x\n", kVendorId, device.product_id());
        unsigned matches = 0;
        for (DWORD slot = 0; slot < XUSER_MAX_COUNT; ++slot) {
            const auto id = pad_identity(slot);
            if (id.vendor == kVendorId && id.product == device.product_id()) {
                std::printf("Matching XInput slot %lu\n", slot);
                ++matches;
            }
        }
        return matches == 1 ? 0 : 2;
    }

    const bool studio_check = argc >= 2 && std::string_view(argv[1]) == "--studio-check";
    if ((studio_check && (argc == 4 || argc == 6)) ||
            (argc == 6 && std::string_view(argv[1]) == "--studio")) {
        StudioSession session(argv[2], std::strtoul(argv[3], nullptr, 10));
        if (!session.ready()) { std::fprintf(stderr, "Studio session could not be opened.\n"); return 1; }
        options.dry_run = studio_check;
        const auto product = argc == 6 ? static_cast<USHORT>(std::strtoul(argv[4], nullptr, 16)) : 0;
        const std::string path = argc == 6 && std::string_view(argv[5]) != "-" ? argv[5] : "";
        return run(options, 0, &session, product, path);
    }

    for (int i = 1; i < argc; ++i) {
        const std::string_view key = argv[i];
        auto next = [&]() -> const char* { return i + 1 < argc ? argv[++i] : nullptr; };
        if (key == "--self-test") return self_test();
        else if (key == "--dry-run") options.dry_run = true;
        else if (key == "--no-auto-gain") options.auto_gain = false;
        else if (key == "--no-triggers") options.triggers = false;
        else if (key == "--no-shoulder") options.shoulder = false;
        else if (key == "--shoulder-live") options.shoulder_live = true;
        else if (key == "--trigger-impact") options.trigger_impact = true;
        else if (key == "--trigger-base") { if (auto v = next()) options.trigger_base = (unsigned)std::strtoul(v, nullptr, 0); }
        else if (key == "--trigger-max")  { if (auto v = next()) options.trigger_max  = (unsigned)std::strtoul(v, nullptr, 0); }
        else if (key == "--harmonic")    { if (auto v = next()) options.harmonic = (float)std::atof(v); }
        else if (key == "--sensitivity") { if (auto v = next()) options.sensitivity = (float)std::atof(v); }
        else if (key == "--gate")        { if (auto v = next()) options.gate = (float)std::atof(v); }
        else if (key == "--target")      { if (auto v = next()) options.target = (float)std::atof(v); }
        else if (key == "--step")        { if (auto v = next()) options.step = (unsigned)std::strtoul(v, nullptr, 16); }
        else if (key == "--refresh")     { if (auto v = next()) options.refresh_ms = (unsigned)std::atoi(v); }
        else if (key == "--seconds")     { if (auto v = next()) seconds = (unsigned)std::atoi(v); }
        else if (key == "--help" || key == "-h") {
            std::printf(
                "KP70Engine - audio haptics for the Kunpeng 70\n\n"
                "  --dry-run           analyse only, never drive the controller\n"
                "  --harmonic <f>      music/voice suppression, default 1.0\n"
                "                      0 = none, 1.5 = aggressive\n"
                "  --sensitivity <f>   gain before the limiter, default 1.0\n"
                "  --gate <f>          silence gate, default 0.02\n"
                "  --target <f>        auto-gain target, default 0.35\n"
                "  --no-auto-gain      disable the automatic level matching\n"
                "  --trigger-impact    trigger-only prototype: base resistance and short audio kicks\n"
                "  --shoulder-live     shoulder-only prototype: continuous audio level over 04/05\n"
                "  --trigger-base <n>  base force (default 10)\n"
                "  --trigger-max <n>   impact force (default 36 in impact mode)\n"
                "  --step <hex>        shoulder bytes 4/5, default 28  [unverified]\n"
                "  --refresh <ms>      packet cadence and latency, default 50\n"
                "  --seconds <n>       stop after n seconds, default run forever\n");
            return 0;
        }
    }
    options.refresh_ms = std::clamp(options.refresh_ms, 20u, 200u);
    if (options.shoulder_live) {
        if (options.trigger_impact || !options.shoulder) {
            std::fprintf(stderr, "--shoulder-live cannot be combined with --trigger-impact or --no-shoulder\n");
            return 1;
        }
        options.triggers = false;
    }
    if (options.trigger_impact) {
        options.shoulder = false;
        options.trigger_base = std::clamp(options.trigger_base, 1u, 0x24u);
        options.trigger_max = std::clamp(options.trigger_max, options.trigger_base, 0x24u);
    }

    return run(options, seconds);
}
