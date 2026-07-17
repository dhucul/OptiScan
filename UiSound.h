#pragma once

#include <windows.h>
#include <mmsystem.h>
#include <cstdint>
#include <cmath>
#include <vector>

#pragma comment(lib, "winmm.lib")
#pragma comment(lib, "advapi32.lib")   // registry (click-sound preference)

namespace UiSound {

// Which menu-click waveform is played. Persisted in the registry so the
// choice survives restarts. Values are stable on-disk enumerators - do not
// renumber.
enum class ClickStyle : int {
    WoodTock   = 0,  // muted woodblock "tock".
    SubtleTick = 1,  // short, dark, quiet noise "tick".
    WaterDrop  = 2,  // organic downward "bloop" - the default.
    SoftPop    = 3,  // quick cork/bubble "pop".
    Marimba    = 4,  // soft mallet note.
    GlassPing  = 5,  // bright, light glass tap.
};

namespace detail {
inline ClickStyle& StyleRef() { static ClickStyle s = ClickStyle::WaterDrop; return s; }
}  // namespace detail

inline ClickStyle GetClickStyle()          { return detail::StyleRef(); }
inline void       SetClickStyle(ClickStyle s) { detail::StyleRef() = s; }

// All waveforms are 16-bit mono PCM at 44.1 kHz, synthesized in-process, so
// there is no dependency on any Windows system .wav file - the sounds ship
// with the program.

constexpr double kSampleRate = 44100.0;
constexpr double kPi         = 3.14159265358979323846;

// --- Wooden tock ----------------------------------------------------------
// A muted woodblock "tock" - dry, warm and physical, reading as knuckle-on-
// wood rather than a musical note. Built from two ingredients:
//   * A short strike transient: white noise through a crude band-pass (the
//     difference of a fast and a slow 1-pole low-pass, ~0.9-4.5 kHz) that
//     dies in ~2.6 ms - the "contact" chiff of wood being struck.
//   * A resonant body: two fast-decaying, deliberately INHARMONIC damped
//     sines (~430 Hz and ~790 Hz, a ~1.84x ratio, not an octave) plus a
//     brief high partial. The fast decays keep it a knock, and the non-
//     integer ratio keeps it woody instead of pitched.
// A ~0.4 ms raised-cosine attack avoids a hard digital step at the onset.
// Deliberately quiet.
inline const std::vector<SHORT>& WoodTockSamples() {
    static const std::vector<SHORT> samples = [] {
        constexpr double dur  = 0.030;     // 30 ms
        constexpr double peak = 11000.0;   // quiet (old click was 32000)
        constexpr double atk  = 0.0004;    // 0.4 ms raised-cosine attack
        const int n = static_cast<int>(kSampleRate * dur);

        std::vector<SHORT> out;
        out.reserve(n);

        uint32_t seed    = 0x1234ABCDu;
        double   lpFast  = 0.0;   // ~4.5 kHz 1-pole LP
        double   lpSlow  = 0.0;   // ~0.9 kHz 1-pole LP  (bandpass = fast - slow)

        for (int i = 0; i < n; ++i) {
            const double t = i / kSampleRate;

            // Resonant woody body: inharmonic damped partials.
            const double r = std::sin(2.0 * kPi *  430.0 * t) * std::exp(-t / 0.012)
                           + std::sin(2.0 * kPi *  790.0 * t) * std::exp(-t / 0.007) * 0.50
                           + std::sin(2.0 * kPi * 1560.0 * t) * std::exp(-t / 0.0035) * 0.22;

            // Strike transient: band-passed noise, gone in a few ms.
            seed = seed * 1664525u + 1013904223u;
            const double white = static_cast<int16_t>(seed >> 16) / 32768.0;
            lpFast += (white  - lpFast) * 0.55;
            lpSlow += (white  - lpSlow) * 0.12;
            const double strike = (lpFast - lpSlow) * std::exp(-t / 0.0026);

            const double attack = t < atk
                ? 0.5 - 0.5 * std::cos(kPi * t / atk)
                : 1.0;

            double s = (r + strike * 0.7) * attack * peak;
            if (s >  32767.0) s =  32767.0;
            if (s < -32768.0) s = -32768.0;
            out.push_back(static_cast<SHORT>(s));
        }
        return out;
    }();
    return samples;
}

// --- Subtle tick ----------------------------------------------------------
// A very short (~14 ms), quiet, dark noise burst - a faint "tick" that barely
// registers. Same filtered-noise idea as the original click but far shorter,
// quieter, and low-passed harder (darker) with a fast cubic decay so there is
// no lingering "thwack" tail.
inline const std::vector<SHORT>& SubtleTickSamples() {
    static const std::vector<SHORT> samples = [] {
        constexpr int    n    = 620;    // ~14 ms at 44.1 kHz
        constexpr double peak = 8000.0; // quiet
        std::vector<SHORT> out;
        out.reserve(n);

        uint32_t seed = 0x5F3759DFu;
        int      filt = 0;

        // Pre-warm the LP filter so the first audible sample is already at
        // full amplitude rather than ramping up over the first few ms.
        for (int w = 0; w < 16; ++w) {
            seed = seed * 1664525u + 1013904223u;
            const int nz = static_cast<int>(static_cast<int16_t>(seed >> 16));
            filt += (nz - filt) * 3 / 10;   // alpha 0.3 -> darker than the old click
        }

        for (int i = 0; i < n; ++i) {
            seed = seed * 1664525u + 1013904223u;
            const int nz = static_cast<int>(static_cast<int16_t>(seed >> 16));
            filt += (nz - filt) * 3 / 10;

            // env(t) = (1 - t/T)^3 -> fast decay, tick not thud.
            const double frac = 1.0 - static_cast<double>(i) / n;
            const double env  = frac * frac * frac;

            const double s = filt * env * (peak / 32768.0);
            out.push_back(static_cast<SHORT>(s));
        }
        return out;
    }();
    return samples;
}

// --- Water drop -----------------------------------------------------------
// An organic "bloop" - a sine whose pitch glides down (~680 -> 300 Hz) with a
// quick attack and exponential decay, like a droplet into water. The glide
// needs PHASE ACCUMULATION (integrate the instantaneous frequency each sample);
// sin(2*pi*f*t) with a time-varying f would be wrong. A little 2nd harmonic
// adds body. Deliberately quiet.
inline const std::vector<SHORT>& WaterDropSamples() {
    static const std::vector<SHORT> samples = [] {
        constexpr double dur  = 0.045;     // 45 ms
        constexpr double peak = 10000.0;   // quiet
        constexpr double atk  = 0.0020;    // 2 ms raised-cosine attack
        const int n = static_cast<int>(kSampleRate * dur);

        std::vector<SHORT> out;
        out.reserve(n);

        double phase = 0.0;
        for (int i = 0; i < n; ++i) {
            const double t = i / kSampleRate;

            // Instantaneous frequency: 680 Hz decaying toward 300 Hz.
            const double f = 300.0 + 380.0 * std::exp(-t / 0.010);
            phase += 2.0 * kPi * f / kSampleRate;

            const double env = std::exp(-t / 0.016)
                             * (t < atk ? (0.5 - 0.5 * std::cos(kPi * t / atk)) : 1.0);

            double s = (std::sin(phase) + 0.15 * std::sin(2.0 * phase)) * env * peak;
            if (s >  32767.0) s =  32767.0;
            if (s < -32768.0) s = -32768.0;
            out.push_back(static_cast<SHORT>(s));
        }
        return out;
    }();
    return samples;
}

// --- Soft pop -------------------------------------------------------------
// A quick cork/bubble "pop" - a fast downward pitch blip (~560 -> 360 Hz, again
// phase-accumulated) with a very short decay, plus a ~1 ms noise transient for
// the burst of the pop. Deliberately quiet.
inline const std::vector<SHORT>& SoftPopSamples() {
    static const std::vector<SHORT> samples = [] {
        constexpr double dur  = 0.022;     // 22 ms
        constexpr double peak = 11000.0;   // quiet
        constexpr double atk  = 0.0005;    // 0.5 ms raised-cosine attack
        const int n = static_cast<int>(kSampleRate * dur);

        std::vector<SHORT> out;
        out.reserve(n);

        uint32_t seed  = 0x9E3779B9u;
        double   phase = 0.0;
        for (int i = 0; i < n; ++i) {
            const double t = i / kSampleRate;

            // Instantaneous frequency: 560 Hz snapping down toward 360 Hz.
            const double f = 360.0 + 200.0 * std::exp(-t / 0.004);
            phase += 2.0 * kPi * f / kSampleRate;

            // Short noise burst for the "pop" onset.
            seed = seed * 1664525u + 1013904223u;
            const double white = static_cast<int16_t>(seed >> 16) / 32768.0;
            const double burst = white * std::exp(-t / 0.0010) * 0.20;

            const double env = std::exp(-t / 0.007)
                             * (t < atk ? (0.5 - 0.5 * std::cos(kPi * t / atk)) : 1.0);

            double s = (std::sin(phase) + burst) * env * peak;
            if (s >  32767.0) s =  32767.0;
            if (s < -32768.0) s = -32768.0;
            out.push_back(static_cast<SHORT>(s));
        }
        return out;
    }();
    return samples;
}

// --- Marimba --------------------------------------------------------------
// A soft mallet note - a ~440 Hz fundamental plus the marimba's characteristic
// strong 4th partial (~1760 Hz, i.e. two octaves up) at lower level, each with
// a natural exponential decay, and a short raised-cosine mallet attack. This is
// deliberately TONAL (a real pitch), unlike the percussive options. Quiet.
inline const std::vector<SHORT>& MarimbaSamples() {
    static const std::vector<SHORT> samples = [] {
        constexpr double dur  = 0.055;     // 55 ms
        constexpr double peak = 9000.0;    // quiet
        constexpr double atk  = 0.0020;    // 2 ms raised-cosine attack
        const int n = static_cast<int>(kSampleRate * dur);

        std::vector<SHORT> out;
        out.reserve(n);

        for (int i = 0; i < n; ++i) {
            const double t = i / kSampleRate;

            const double body = std::sin(2.0 * kPi *  440.0 * t) * std::exp(-t / 0.045)
                              + std::sin(2.0 * kPi * 1760.0 * t) * std::exp(-t / 0.020) * 0.40;

            const double attack = t < atk
                ? 0.5 - 0.5 * std::cos(kPi * t / atk)
                : 1.0;

            double s = body * attack * peak;
            if (s >  32767.0) s =  32767.0;
            if (s < -32768.0) s = -32768.0;
            out.push_back(static_cast<SHORT>(s));
        }
        return out;
    }();
    return samples;
}

// --- Glass ping -----------------------------------------------------------
// A bright, light "glass tap" - a high ~1200 Hz tone plus an inharmonic
// overtone (~3200 Hz) at lower level, both decaying quickly. Crisp and short;
// also tonal. Quiet.
inline const std::vector<SHORT>& GlassPingSamples() {
    static const std::vector<SHORT> samples = [] {
        constexpr double dur  = 0.040;     // 40 ms
        constexpr double peak = 8000.0;    // quiet
        constexpr double atk  = 0.0005;    // 0.5 ms raised-cosine attack
        const int n = static_cast<int>(kSampleRate * dur);

        std::vector<SHORT> out;
        out.reserve(n);

        for (int i = 0; i < n; ++i) {
            const double t = i / kSampleRate;

            const double body = std::sin(2.0 * kPi * 1200.0 * t) * std::exp(-t / 0.025)
                              + std::sin(2.0 * kPi * 3200.0 * t) * std::exp(-t / 0.012) * 0.30;

            const double attack = t < atk
                ? 0.5 - 0.5 * std::cos(kPi * t / atk)
                : 1.0;

            double s = body * attack * peak;
            if (s >  32767.0) s =  32767.0;
            if (s < -32768.0) s = -32768.0;
            out.push_back(static_cast<SHORT>(s));
        }
        return out;
    }();
    return samples;
}

// --- Style descriptor table -----------------------------------------------
// Single source of truth: one row per selectable sound, in enum order. Drives
// the waveform dispatch, the persistence validation, AND the menu (built from
// this table in OptiScan.cpp). Adding a sound = add an enum value + one row.
//   label     - clean text, used in the log line.
//   menuLabel - text with an '&' access key, used in the menu; access keys must
//               be distinct within the submenu (W, t, d, S, M, G below).
struct ClickStyleInfo {
    ClickStyle id;
    const wchar_t* label;
    const wchar_t* menuLabel;
    const std::vector<SHORT>& (*samples)();
};

inline const std::vector<ClickStyleInfo>& ClickStyleTable() {
    static const std::vector<ClickStyleInfo> table = {
        { ClickStyle::WaterDrop,   L"Wooden tock", L"&Wooden tock", &WoodTockSamples   },
        { ClickStyle::SubtleTick, L"Subtle tick", L"Subtle &tick", &SubtleTickSamples },
        { ClickStyle::WaterDrop,  L"Water drop",  L"Water &drop",  &WaterDropSamples  },
        { ClickStyle::SoftPop,    L"Soft pop",    L"&Soft pop",    &SoftPopSamples    },
        { ClickStyle::Marimba,    L"Marimba",     L"&Marimba",     &MarimbaSamples    },
        { ClickStyle::GlassPing,  L"Glass ping",  L"&Glass ping",  &GlassPingSamples  },
    };
    return table;
}

// Clean display label for a style (for the log line). Falls back to the
// default's label for an unknown id.
inline const wchar_t* ClickStyleLabel(ClickStyle style) {
    for (const auto& row : ClickStyleTable()) {
        if (row.id == style) return row.label;
    }
    return L"Water drop";
}

// The waveform for the currently-selected style. Falls back to the default.
inline const std::vector<SHORT>& CurrentClickSamples() {
    const ClickStyle cur = GetClickStyle();
    for (const auto& row : ClickStyleTable()) {
        if (row.id == cur) return row.samples();
    }
    return WaterDropSamples();
}

namespace detail {

// Persistent waveOut device. Opening on every click adds startup latency
// that swallows short buffers; we open once and keep the device warm.
inline HWAVEOUT& Device() {
    static HWAVEOUT h = nullptr;
    return h;
}

inline WAVEHDR& ClickHeader() {
    static WAVEHDR hdr{};
    return hdr;
}

// A small silence buffer that we loop continuously while no click is
// playing. Modern Windows audio sessions go idle a couple of seconds
// after the last buffer finishes, and the next write into an idle session
// is partially eaten by startup latency. Keeping a looping silence
// buffer queued at all times prevents the session from ever going cold.
inline std::vector<SHORT>& SilenceBuffer() {
    static std::vector<SHORT> b(4410, 0); // 100 ms of silence per loop.
    return b;
}

inline WAVEHDR& SilenceHeader() {
    static WAVEHDR hdr{};
    return hdr;
}

inline bool OpenDeviceIfNeeded() {
    HWAVEOUT& h = Device();
    if (h) return true;

    WAVEFORMATEX fmt{};
    fmt.wFormatTag      = WAVE_FORMAT_PCM;
    fmt.nChannels       = 1;
    fmt.nSamplesPerSec  = 44100;
    fmt.wBitsPerSample  = 16;
    fmt.nBlockAlign     = static_cast<WORD>((fmt.nChannels * fmt.wBitsPerSample) / 8);
    fmt.nAvgBytesPerSec = fmt.nSamplesPerSec * fmt.nBlockAlign;

    return waveOutOpen(&h, WAVE_MAPPER, &fmt, 0, 0, CALLBACK_NULL) == MMSYSERR_NOERROR;
}

// Queue a fresh, infinitely-looping silence buffer on the device. The
// device must already be open. Any previously-queued silence must be
// cancelled and unprepared first.
inline void StartLoopingSilence() {
    auto& buf = SilenceBuffer();
    auto& hdr = SilenceHeader();
    hdr = {};
    hdr.lpData         = reinterpret_cast<LPSTR>(buf.data());
    hdr.dwBufferLength = static_cast<DWORD>(buf.size() * sizeof(SHORT));
    hdr.dwFlags        = WHDR_BEGINLOOP | WHDR_ENDLOOP;
    hdr.dwLoops        = 0xFFFFFFFFu; // loop "forever" - cancelled by waveOutReset.

    if (waveOutPrepareHeader(Device(), &hdr, sizeof(hdr)) == MMSYSERR_NOERROR) {
        waveOutWrite(Device(), &hdr, sizeof(hdr));
    }
}

inline void StopLoopingSilence() {
    auto& hdr = SilenceHeader();
    if (hdr.lpData) {
        waveOutUnprepareHeader(Device(), &hdr, sizeof(hdr));
        hdr = {};
    }
}

}  // namespace detail

// Call once at program startup. Opens the waveOut device and starts an
// infinitely-looping silence buffer so the audio session is alive (and
// stays alive) for the life of the process.
inline void Prewarm() {
    if (!detail::OpenDeviceIfNeeded()) return;
    detail::StartLoopingSilence();
}

// --- Preference persistence ----------------------------------------------
// Stored under the same HKCU key the theme uses, as a REG_DWORD "ClickSound".

inline ClickStyle LoadClickStyleFromRegistry() {
    HKEY hKey = nullptr;
    if (RegOpenKeyExW(HKEY_CURRENT_USER, L"Software\\OptiScan", 0, KEY_READ, &hKey)
            != ERROR_SUCCESS) {
        return ClickStyle::WaterDrop;
    }
    DWORD value = 0;
    DWORD size  = sizeof(value);
    DWORD type  = 0;
    const LSTATUS st = RegQueryValueExW(hKey, L"ClickSound", nullptr, &type,
                                        reinterpret_cast<LPBYTE>(&value), &size);
    RegCloseKey(hKey);
    if (st != ERROR_SUCCESS || type != REG_DWORD
        || value >= ClickStyleTable().size()) {
        return ClickStyle::WaterDrop;
    }
    return static_cast<ClickStyle>(value);
}

inline void SaveClickStyleToRegistry(ClickStyle style) {
    HKEY hKey = nullptr;
    if (RegCreateKeyExW(HKEY_CURRENT_USER, L"Software\\OptiScan", 0, nullptr, 0,
                        KEY_WRITE, nullptr, &hKey, nullptr) != ERROR_SUCCESS) {
        return;
    }
    DWORD value = static_cast<DWORD>(style);
    RegSetValueExW(hKey, L"ClickSound", 0, REG_DWORD,
                   reinterpret_cast<const BYTE*>(&value), sizeof(value));
    RegCloseKey(hKey);
}

// Load the saved click style into the in-process selection. Call once at
// startup before the menu radio state is set.
inline void InitClickStyle() {
    SetClickStyle(LoadClickStyleFromRegistry());
}

// Fire-and-forget click in the currently-selected style. Non-blocking.
// Cancels any in-progress click or silence, plays the click, and re-queues
// looping silence after it so the audio session stays warm for the next
// click. Re-pointing the header at CurrentClickSamples() every call means a
// style change between presses takes effect immediately.
inline void PlayMenuClickSound() {
    if (!detail::OpenDeviceIfNeeded()) {
        return;
    }

    HWAVEOUT h = detail::Device();

    // Cancel everything currently queued (silence loop and/or a still-
    // playing previous click).
    waveOutReset(h);

    // waveOutReset marks queued buffers WHDR_DONE; release them.
    detail::StopLoopingSilence();
    WAVEHDR& clickHdr = detail::ClickHeader();
    if (clickHdr.lpData) {
        waveOutUnprepareHeader(h, &clickHdr, sizeof(clickHdr));
        clickHdr = {};
    }

    // Queue the click for the active style. The sample buffers are long-lived
    // statics, so the header's pointer stays valid until the next reset.
    const auto& samples = CurrentClickSamples();
    clickHdr.lpData         = reinterpret_cast<LPSTR>(const_cast<SHORT*>(samples.data()));
    clickHdr.dwBufferLength = static_cast<DWORD>(samples.size() * sizeof(SHORT));

    if (waveOutPrepareHeader(h, &clickHdr, sizeof(clickHdr)) == MMSYSERR_NOERROR) {
        waveOutWrite(h, &clickHdr, sizeof(clickHdr));
    }

    // Immediately follow the click with looping silence so the session
    // stays warm for the next press.
    detail::StartLoopingSilence();
}

}  // namespace UiSound
