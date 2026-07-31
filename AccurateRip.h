// ============================================================================
// AccurateRip.h - AccurateRip CRC calculation and database lookup
// ============================================================================
#pragma once

#include "DiscTypes.h"
#include <cstdint>

enum class AccurateRipVerificationResult {
    Inconclusive,
    Verified,
    Mismatch
};

class AccurateRip {
public:
    // Calculate both checksum variants. Returns false for empty, malformed,
    // undersized, or otherwise invalid track data.
    static bool CalculateCRCs(const std::vector<std::vector<BYTE>>& sectors,
        int trackNum, int totalTracks, uint32_t& crcV1, uint32_t& crcV2);

    // Disc ID calculations
    static uint32_t CalculateDiscID1(const DiscInfo& disc);
    static uint32_t CalculateDiscID2(const DiscInfo& disc);
    static uint32_t CalculateCDDBID(const DiscInfo& disc);

    // Database lookup — fetches per-track records for ALL pressings
    static bool Lookup(DiscInfo& disc, std::vector<AccurateRipPressing>& pressings);

    // Verify CRCs for all tracks against all pressings. Frame450 probes may
    // locate a shared sample offset, but only a full-track checksum match is
    // accepted as verification.
    static AccurateRipVerificationResult VerifyCRCs(const DiscInfo& disc,
        const std::vector<AccurateRipPressing>& pressings);
};
