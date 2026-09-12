#pragma once
#include "ArtifactTransaction.h"

// Encoding and intermediate WAVs stay in private staging. A failed FLAC
// conversion can publish its WAV fallback without touching any existing FLAC.
template<class WriteWav, class EncodeFlac, class Cancelled>
bool SaveTrackArtifact(const std::wstring& base, bool flac,
    WriteWav writeWav, EncodeFlac encodeFlac, Cancelled cancelled,
    std::wstring& actualPath, bool& fallback) {
    actualPath.clear();
    fallback = false;
    if (cancelled()) return false;
    ArtifactTransaction transaction{std::filesystem::path(base)};
    const auto wav = transaction.ScratchFile(L"audio.wav");
    const auto encoded = transaction.ScratchFile(L"audio.flac");
    if (wav.empty() || encoded.empty() || !writeWav(wav.wstring()) || cancelled()) return false;
    bool encodedOk = false;
    if (flac) encodedOk = encodeFlac(wav.wstring(), encoded.wstring());
    if (cancelled()) return false;
    const std::wstring target = base + (encodedOk ? L".flac" : L".wav");
    const auto staged = transaction.Stage(target);
    if (staged.empty()) return false;
    std::error_code ec;
    std::filesystem::rename(encodedOk ? encoded : wav, staged, ec);
    if (ec || cancelled() || !transaction.Commit()) return false;
    actualPath = target;
    fallback = flac && !encodedOk;
    return true;
}
