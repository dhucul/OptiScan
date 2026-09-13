#pragma once
#include "MediaIdentity.h"

// One instance per batch. Readiness alone cannot establish disc identity.
class BatchPrescan {
    wchar_t scannedDrive = 0;
public:
    void RecordFreshScan(wchar_t drive) { scannedDrive = drive; }

    template<class Query, class Refresh>
    bool Prepare(bool freshlyScanned, wchar_t& drive, bool& hasTOC,
                 const std::optional<MediaIdentity>& scanIdentity,
                 Query query, Refresh refresh) {
        const auto current = query();
        const bool sameMedia = scanIdentity && current && *scanIdentity == *current;
        const bool usableFreshScan = freshlyScanned && (!scanIdentity || sameMedia);
        if (!usableFreshScan &&
            !(scannedDrive != 0 && scannedDrive == drive && hasTOC && sameMedia)) {
            scannedDrive = 0;
            if (!refresh()) return false;
            // Refresh can select another drive and replace the referenced stamp.
            const auto refreshed = query();
            if (scanIdentity && (!refreshed || *scanIdentity != *refreshed)) return false;
        }
        if (!hasTOC || drive == 0) { scannedDrive = 0; return false; }
        RecordFreshScan(drive);
        return true;
    }

    // Stable dispatcher IDs. A completed explicit rescan supplies the next
    // step's source, including a recovered TOC-less layout.
    void FinishStep(int operation, wchar_t drive, bool hasTOC, bool completed) {
        if (!completed || !hasTOC) { scannedDrive = 0; return; }
        switch (operation) {
        case 25:
            RecordFreshScan(drive);
            break;
        case 1: case 3: case 4: case 11: case 30: case 31: case 33:
            scannedDrive = 0;
            break;
        default:
            if (drive != scannedDrive) scannedDrive = 0;
            break;
        }
    }
};
