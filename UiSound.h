#pragma once

#include <windows.h>
#include <mmsystem.h>
#include <cstdint>
#include <vector>

#pragma comment(lib, "winmm.lib")

namespace UiSound {

// 16-bit mono PCM samples for a short menu click. Synthesized in-process,
// so there is no dependency on any Windows system .wav file - the click
// ships with the program.
//
// Pure broadband noise burst, low-pass filtered to ~2.8 kHz and shaped by
// a quadratic decay envelope. No tonal layer - any pitched component
// would read as a beep. The LP filter removes the harsh high-frequency
// hiss that made raw white noise sound "scratchy", leaving a mid-range
// "thwack" with no clear fundamental.
inline const std::vector<SHORT>& ClickSamples() {
    static const std::vector<SHORT> samples = [] {
        constexpr DWORD sampleCount = 2646;  // 60 ms at 44.1 kHz.
        constexpr int   peak        = 32000;

        std::vector<SHORT> out;
        out.reserve(sampleCount);

        uint32_t seed = 0xC001D00Du;
        int filt = 0;

        // Pre-warm the LP filter so the very first audible sample is at
        // full amplitude rather than ramping up over the first few ms.
        for (int w = 0; w < 16; ++w) {
            seed = seed * 1664525u + 1013904223u;
            const int n = static_cast<int>(static_cast<int16_t>(seed >> 16));
            filt += (n - filt) * 4 / 10;
        }

        for (DWORD i = 0; i < sampleCount; ++i) {
            seed = seed * 1664525u + 1013904223u;
            const int n = static_cast<int>(static_cast<int16_t>(seed >> 16));
            // 1-pole LP, alpha = 0.4 -> cutoff ~2.8 kHz.
            filt += (n - filt) * 4 / 10;

            // env(t) = peak * (1 - t/T)^2.
            const long long rem    = static_cast<long long>(sampleCount - i);
            const long long envN   = rem * rem;
            const long long envD   = static_cast<long long>(sampleCount) * sampleCount;
            const long long envAmp = (static_cast<long long>(peak) * envN) / envD;

            const long long s = (static_cast<long long>(filt) * envAmp) / 32768;
            out.push_back(static_cast<SHORT>(s));
        }
        return out;
    }();
    return samples;
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

} // namespace detail

// Call once at program startup. Opens the waveOut device and starts an
// infinitely-looping silence buffer so the audio session is alive (and
// stays alive) for the life of the process.
inline void Prewarm() {
    if (!detail::OpenDeviceIfNeeded()) return;
    detail::StartLoopingSilence();
}

// Fire-and-forget click. Non-blocking. Cancels any in-progress click or
// silence, plays the click, and re-queues looping silence after it so
// the audio session stays warm for the next click.
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

    // Queue the click.
    const auto& samples = ClickSamples();
    clickHdr.lpData         = reinterpret_cast<LPSTR>(const_cast<SHORT*>(samples.data()));
    clickHdr.dwBufferLength = static_cast<DWORD>(samples.size() * sizeof(SHORT));

    if (waveOutPrepareHeader(h, &clickHdr, sizeof(clickHdr)) == MMSYSERR_NOERROR) {
        waveOutWrite(h, &clickHdr, sizeof(clickHdr));
    }

    // Immediately follow the click with looping silence so the session
    // stays warm for the next press.
    detail::StartLoopingSilence();
}

} // namespace UiSound
