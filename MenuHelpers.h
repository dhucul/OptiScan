// ============================================================================
// MenuHelpers.h - GUI input helpers
//
// GetMenuChoice() now pops up a small modal dialog (via GuiInput::PromptInt)
// instead of reading from std::cin.
// ============================================================================
#pragma once

#include "DiscTypes.h"
#include "GuiInput.h"
#include "InterruptHandler.h"
#include <iomanip>
#include <iostream>
#include <string>

// Title + message variant. The modal box often covers the console, so callers
// that print a "=== Heading ===" + numbered option list to the console should
// repeat that here: pass the heading as `title` and the option list as
// `message` so the dialog is self-describing without scrolling the console.
// Same return / Cancel contract as the short form below.
inline int GetMenuChoice(const char* title, const char* message,
                         int minChoice, int maxChoice, int defaultChoice = 1,
                         bool* outOk = nullptr) {
    auto cancelResult = [&]() {
        if (outOk) *outOk = false;
        // When the caller didn't ask for a Cancel signal, fall through to
        // minChoice so the dominant "0 = back" sub-menu pattern naturally
        // backs out. When they did ask, return defaultChoice and let them
        // decide via outOk.
        return outOk ? defaultChoice : minChoice;
    };

    if (g_interrupt.IsInterrupted()) {
        return cancelResult();
    }
    bool ok = false;
    int v = GuiInput::PromptInt(title, message,
                                minChoice, maxChoice, defaultChoice, &ok);
    std::cout << "\n";
    if (!ok) return cancelResult();
    if (outOk) *outOk = true;
    return v;
}

// Returns a number in [minChoice, maxChoice]. On Cancel:
//   - if `outOk` is non-null, sets *outOk = false and returns defaultChoice.
//   - otherwise returns minChoice (so the dominant "0 = back" pattern in
//     sub-menus naturally backs out).
// Callers driving destructive flows (write/erase) should pass an outOk and
// abort the workflow when it comes back false.
inline int GetMenuChoice(int minChoice, int maxChoice, int defaultChoice = 1,
                         bool* outOk = nullptr) {
    return GetMenuChoice("Choose an option",
                         "Enter a number for the desired option:",
                         minChoice, maxChoice, defaultChoice, outOk);
}

inline void PrintDiscInfo(const DiscInfo& disc) {
    std::cout << "\n=== Disc Info ===\n";
    if (!disc.cdText.albumTitle.empty())
        std::cout << "Album: " << disc.cdText.albumTitle << "\n";
    if (!disc.cdText.albumArtist.empty())
        std::cout << "Artist: " << disc.cdText.albumArtist << "\n";
    std::cout << "Sessions: " << disc.sessionCount << "\n";
    std::cout << "Tracks: " << disc.tracks.size();
    if (disc.tocRepaired)
        std::cout << "  (reconstructed from Q subchannel)";
    std::cout << "\n";
    std::cout << "Lead-out: LBA " << disc.leadOutLBA << "\n";

    int audioCount = 0, dataCount = 0;
    for (const auto& t : disc.tracks) {
        if (t.isAudio) audioCount++;
        else dataCount++;
    }
    if (dataCount > 0)
        std::cout << "  (" << audioCount << " audio, " << dataCount << " data)\n";

    std::cout << "\n";

    for (size_t i = 0; i < disc.tracks.size(); i++) {
        const auto& t = disc.tracks[i];
        DWORD sectors = (t.endLBA >= t.startLBA) ? (t.endLBA - t.startLBA + 1) : 0;
        int sec = static_cast<int>(sectors / 75);

        int pregapFrames = 0;
        if (t.startLBA == 0) {
            if (t.index01LBA > 0)
                pregapFrames = static_cast<int>(t.index01LBA);
        }
        else if (t.pregapLBA < t.startLBA) {
            pregapFrames = static_cast<int>(t.startLBA - t.pregapLBA);
        }

        bool hasGapBefore = false;
        if (i > 0) {
            DWORD prevEnd = disc.tracks[i - 1].endLBA;
            hasGapBefore = (t.startLBA > prevEnd + 225);
        }

        std::cout << "  Track " << std::setw(2) << t.trackNumber
            << " [S" << t.session << "]: "
            << (t.isAudio ? "Audio" : "Data ") << " "
            << sec / 60 << ":"
            << std::setfill('0') << std::setw(2) << sec % 60
            << std::setfill(' ')
            << " (" << std::setw(3) << sec << "s)"
            << "  LBA " << std::setw(6) << t.startLBA
            << " - " << std::setw(6) << t.endLBA;

        if (pregapFrames > 0) {
            int pgSec = pregapFrames / 75;
            int pgFrm = pregapFrames % 75;
            std::cout << "  Pregap "
                << std::setw(2) << pgSec << "s "
                << std::setw(2) << pgFrm << "f";
        }

        if (t.hasPreemphasis) std::cout << "  [Pre-emphasis]";
        if (!t.isrc.empty())  std::cout << "  ISRC:" << t.isrc;
        if (hasGapBefore)     std::cout << "  [GAP]";

        std::cout << "\n";
    }
}
