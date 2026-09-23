#define NOMINMAX
#include "../DiscRotQuality.h"
#include "../DiscRotReadConsistency.h"
#include "../ComprehensiveQuality.h"
#include <iostream>
#include <list>
#include <sstream>

int RunDiscRotTests() {
    int failures = 0;
    auto check = [&](bool condition, const char* message) {
        std::cout << (condition ? "[PASS] " : "[FAIL] ") << message << '\n';
        if (!condition) ++failures;
    };
    auto base = [] {
        DiscRotAnalysis r;
        r.totalRereadTests = 1500;
        r.rotRiskLevel = "NONE";
        return r;
    };
    auto goodComposite = [&] {
        ComprehensiveScanResult c;
        c.rot = base();
        c.bler.hasC1Data = true;
        c.bler.c2PointerDataRecorded = true;
        c.bler.c1Samples = {{0,75,1}, {75,75,1}, {150,75,1}};
        c.bler.c1 = ScanQuality::SummarizeC1(c.bler.c1Samples, true);
        return c;
    };
    // Every limitation must coexist with every kind of observed evidence.
    // A confirmed failure wins the grade; it does not erase the limitation.
    for (int limitation = 0; limitation < 3; ++limitation) {
        for (int evidence = 0; evidence < 5; ++evidence) {
            auto c = goodComposite();
            if (limitation == 0) c.rot.consistencyUnverifiedSamples = 1;
            else if (limitation == 1) c.rot.totalRereadTests = 0;
            else c.rot.pioneerDrive = true;
            if (evidence == 1) c.rot.inconsistentSectors = 1;
            else if (evidence == 2) c.rot.phase1C2Sectors = 1;
            else if (evidence == 3) c.rot.recoveredReadFailures = 1;
            else if (evidence == 4) c.rot.verificationReadFailures = 1;
            DiscRot::Finalize(c.rot);
            ComprehensiveQuality::Finalize(c);
            check(DiscRot::HasLimitedReadConfidence(c.rot) &&
                c.overallRating == (evidence == 4 ? "F" : "INCOMPLETE") && c.overallScore <= 79,
                "Observed errors cannot erase an independent measurement limitation");
            std::ostringstream report;
            DiscRot::PrintReadEvidence(report, c.rot, "# ");
            check(report.str().find("# Read confidence: LIMITED") != std::string::npos &&
                (evidence == 4 || c.summary.find("read confidence") != std::string::npos),
                "Console/log evidence retains limited confidence alongside errors and recovery");
        }
    }
    auto transition = goodComposite();
    transition.rot.consistencyUnverifiedSamples = 1;
    DiscRot::Finalize(transition.rot);
    ComprehensiveQuality::Finalize(transition);
    check(transition.overallRating == "INCOMPLETE", "Unverified rereads start incomplete");
    transition.rot.inconsistentSectors = 1;
    transition.rot.inconsistencyRate = 100.0 / 1500;
    DiscRot::Finalize(transition.rot);
    ComprehensiveQuality::Finalize(transition);
    check(transition.overallRating == "INCOMPLETE" && transition.rot.rotRiskLevel == "LOW",
        "Adding one mismatch cannot upgrade INCOMPLETE to A");
    transition.rot.phase1C2Sectors = 1;
    DiscRot::Finalize(transition.rot);
    ComprehensiveQuality::Finalize(transition);
    check(transition.overallRating == "INCOMPLETE" && transition.rot.rotRiskLevel == "MODERATE",
        "Adding C2 retains incomplete status and raises preservation risk");
    transition.rot.consistencyUnverifiedSamples = 0;
    // Do not update the display status: the composite must use current evidence.
    transition.rot.readabilityStatus = "INCOMPLETE - READ CONFIDENCE LIMITED";
    ComprehensiveQuality::Finalize(transition);
    check(transition.overallRating == "D" &&
        ComprehensiveQuality::MissingMeasurements(transition).empty(),
        "Restoring confidence clears incompleteness without relying on a display string");
    transition.rot.verificationReadFailures = 1;
    ComprehensiveQuality::Finalize(transition);
    check(transition.overallRating == "F", "A new failed verification overrides a previous grade");
    transition.rot.verificationReadFailures = 0;
    ComprehensiveQuality::Finalize(transition);
    check(transition.overallRating == "D", "Replacing verification evidence clears a stale failure grade");

    struct ReadOutcome { bool ok; int c2; BYTE audio = 0; };
    auto phase1Case = [&](bool pioneer, std::vector<ReadOutcome> reads, bool cacheCleared,
        int failed, int recovered, int verification, int c2Sectors,
        const char* risk, const char* grade, const char* expectedCalls) {
        size_t next = 0;
        std::string calls;
        const auto sector = DiscRot::ReadPhase1Sector(999, pioneer,
            [&](DWORD, BYTE* audio, int& c2) {
                calls += 'R';
                const auto& outcome = reads.at(next++);
                c2 = outcome.c2;
                std::memset(audio, outcome.audio, AUDIO_SECTOR_SIZE);
                return outcome.ok;
            }, [&](DWORD) { calls += 'E'; return cacheCleared; }, [] { return false; });
        auto c = goodComposite();
        DiscRot::RecordPhase1Evidence(sector, c.rot);
        c.rot.maxC2InSingleSector = sector.c2Errors;
        DiscRot::Finalize(c.rot);
        ComprehensiveQuality::Finalize(c);
        check(calls == expectedCalls && next == reads.size() &&
            c.rot.totalReadFailures == failed && c.rot.recoveredReadFailures == recovered &&
            c.rot.verificationReadFailures == verification && c.rot.phase1C2Sectors == c2Sectors &&
            sector.HasUnrecoveredFailure() == (failed > 0 || verification > 0) &&
            c.rot.rotRiskLevel == risk && c.overallRating == grade,
            "Production Phase 1 branches retain distinct read, retry and verification outcomes");
        std::ostringstream report;
        DiscRot::PrintReadEvidence(report, c.rot, "# ");
        check(report.str().find("initial failures recovered on retry: " + std::to_string(recovered)) != std::string::npos &&
            report.str().find("failed C2 verification reads: " + std::to_string(verification)) != std::string::npos &&
            (recovered == 0 || c.rot.readabilityStatus != "NO READ PROBLEMS OBSERVED"),
            "Saved Phase 1 evidence preserves recovered and verification failures");
    };
    phase1Case(true, {{true,0}}, true, 0,0,0,0, "NONE", "A", "R");
    phase1Case(true, {{false,0},{true,0}}, true, 0,1,0,0, "LOW", "B", "RER");
    phase1Case(true, {{false,0},{false,0}}, true, 1,0,0,0, "HIGH", "F", "RER");
    phase1Case(true, {{true,1},{false,0}}, true, 0,0,1,1, "HIGH", "F", "RER");
    phase1Case(true, {{false,0},{true,1},{false,0}}, true, 0,1,1,1, "HIGH", "F", "RERER");
    phase1Case(true, {{true,1},{true,0}}, true, 0,0,0,1, "MODERATE", "D", "RER");
    phase1Case(true, {{true,1},{true,0}}, false, 0,0,0,1, "MODERATE", "D", "RER");
    phase1Case(true, {{true,1},{true,0,1}}, true, 0,0,0,1, "MODERATE", "D", "RER");
    phase1Case(false, {{false,0}}, true, 1,0,0,0, "HIGH", "F", "R");
    phase1Case(false, {{true,1}}, true, 0,0,0,1, "MODERATE", "D", "R");
    for (int cancelAt = 0; cancelAt < 4; ++cancelAt) {
        bool cancelled = cancelAt == 0;
        int reads = 0, evictions = 0;
        const auto sector = DiscRot::ReadPhase1Sector(999, true,
            [&](DWORD, BYTE* audio, int& c2) {
                ++reads;
                std::memset(audio, 0, AUDIO_SECTOR_SIZE);
                c2 = 1;
                if ((cancelAt == 1 && reads == 1) || (cancelAt == 3 && reads == 2)) cancelled = true;
                return true;
            }, [&](DWORD) { ++evictions; if (cancelAt == 2) cancelled = true; return true; },
            [&] { return cancelled; });
        auto result = base();
        DiscRot::RecordPhase1Evidence(sector, result);
        check(sector.cancelled && reads == (cancelAt == 0 ? 0 : cancelAt == 3 ? 2 : 1) &&
            evictions == (cancelAt < 2 ? 0 : 1) && result.phase1C2Sectors == 0 &&
            result.totalReadFailures == 0 && result.verificationReadFailures == 0,
            "Cancelled Phase 1 transitions stop subsequent I/O and do not publish partial sector outcomes");
    }
    auto unreadable = base();
    unreadable.totalReadFailures = 100;
    unreadable.zones.middleErrors = 100;
    unreadable.zones.innerSectors = unreadable.zones.middleSectors = unreadable.zones.outerSectors = 100000;
    unreadable.inconsistentSectors = 1;
    unreadable.consistencyReadFailures = 1;
    unreadable.inconsistencyRate = 100.0 / 1500;
    unreadable.rotRiskLevel = DiscRot::AssessPatternRisk(unreadable);
    check(unreadable.rotRiskLevel == "NONE", "Localized read failures need not form a rot pattern");
    DiscRot::Finalize(unreadable);
    check(unreadable.rotRiskLevel == "HIGH" && DiscRot::HasConfirmedFailure(unreadable),
        "100 unreadable middle sectors override the absence of rot patterns");
    auto uniform = base();
    uniform.phase1C2Sectors = 30000;
    uniform.maxC2InSingleSector = 20;
    uniform.zones.innerErrors = uniform.zones.innerSectors = 9900;
    uniform.zones.middleErrors = uniform.zones.middleSectors = 9900;
    uniform.zones.outerErrors = uniform.zones.outerSectors = 10200;
    uniform.rotRiskLevel = DiscRot::AssessPatternRisk(uniform);
    DiscRot::Finalize(uniform);
    check(uniform.rotRiskLevel == "MODERATE" && uniform.readabilityStatus == "C2 ACTIVITY OBSERVED",
        "Uniform C2 activity with matching rereads cannot receive NONE");
    auto singleC2 = base();
    singleC2.phase1C2Sectors = 1;
    DiscRot::Finalize(singleC2);
    check(singleC2.rotRiskLevel == "MODERATE", "A later clean verification read cannot erase observed C2 activity");

    for (const auto* method : {"Plextor Q-Check (0xE9/0xEB)", "LiteOn C1/C2/CU"}) {
        for (bool complete : {false, true}) {
            QCheckResult vendor;
            vendor.scanMethod = method;
            vendor.cuMeasured = true;
            vendor.samples.push_back(QCheckSample{});
            vendor.totalC2 = 7;
            vendor.totalCU = 2;
            auto r = base();
            DiscRot::RecordQualityEvidence(vendor, complete, r);
            // Independent Phase 1 and consistency scans found nothing.
            DiscRot::Finalize(r);
            check(r.qualityC2Count == 7 && r.qualityCUCount == 2 && r.rotRiskLevel == "HIGH" &&
                r.qualityScanComplete == complete, "Vendor CU survives complete/partial scans and later clean reads");
            std::ostringstream report;
            DiscRot::PrintReadEvidence(report, r, "# ");
            check(report.str().find("observed CU count: 2") != std::string::npos &&
                report.str().find(DiscRot::kCaveat) != std::string::npos &&
                (complete || report.str().find("partial") != std::string::npos),
                "Report preserves vendor CU, completion state and the heuristic caveat");
            ComprehensiveScanResult composite;
            composite.rot = r;
            ComprehensiveQuality::Finalize(composite);
            check(composite.overallRating == "F", "Vendor CU reaches the comprehensive assessment");
        }
    }
    QCheckResult onlyC2;
    onlyC2.scanMethod = "LiteOn";
    onlyC2.cuMeasured = false;
    onlyC2.samples.push_back(QCheckSample{});
    onlyC2.totalC2 = 9;
    onlyC2.totalCU = 100; // Unsupported CU must not become evidence.
    auto r = base();
    DiscRot::RecordQualityEvidence(onlyC2, true, r);
    DiscRot::Finalize(r);
    check(r.rotRiskLevel == "MODERATE" && r.qualityCUCount == 0 && !DiscRot::HasConfirmedFailure(r),
        "C2-only vendor observations warn without inventing CU");
    onlyC2.scanMethod = "Pioneer (0x3B/0x3C)";
    r = base();
    DiscRot::RecordQualityEvidence(onlyC2, true, r);
    DiscRot::Finalize(r);
    check(r.qualityC2Count == 0 && r.qualityCUCount == 0 && r.rotRiskLevel == "NONE",
        "Pioneer vendor diagnostics remain separate from verified C2/CU");
    r = base();
    r.pioneerCdCheckRun = true;
    r.pioneerCdCheckC2Bytes = 1;
    r.rotRiskLevel = "CRITICAL";
    DiscRot::Finalize(r);
    check(r.rotRiskLevel == "CRITICAL", "Read-evidence minimums never downgrade CRITICAL");
    r = base();
    r.consistencyReadFailures = 1;
    DiscRot::Finalize(r);
    check(r.rotRiskLevel == "HIGH", "A failed sampled read warns independently of percentage thresholds");
    r = base();
    r.inconsistentSectors = 1;
    DiscRot::Finalize(r);
    check(r.rotRiskLevel == "LOW", "A differing reread warns even below the instability threshold");
    r = base();
    r.consistencyUnverifiedSamples = 1;
    DiscRot::Finalize(r);
    check(r.readabilityStatus.find("INCOMPLETE") == 0 &&
        r.recommendation.find("cached data") != std::string::npos,
        "Failed or unknown cache eviction cannot establish clean rereads");
    ComprehensiveScanResult incomplete;
    incomplete.rot = r;
    incomplete.bler.hasC1Data = true;
    incomplete.bler.c2PointerDataRecorded = true;
    incomplete.bler.c1Samples = {{0,75,1}, {75,75,1}, {150,75,1}};
    incomplete.bler.c1 = ScanQuality::SummarizeC1(incomplete.bler.c1Samples, true);
    ComprehensiveQuality::Finalize(incomplete);
    check(incomplete.overallRating == "INCOMPLETE" &&
        incomplete.summary.find("read confidence") != std::string::npos,
        "Missing reread confidence remains incomplete even with good independent C1/C2");
    incomplete.rot.totalReadFailures = 1;
    ComprehensiveQuality::Finalize(incomplete);
    check(incomplete.overallRating == "F", "Disc rot read failures override otherwise good comprehensive measurements");
    r = base();
    r.pioneerDrive = true;
    DiscRot::Finalize(r);
    check(r.readabilityStatus.find("INCOMPLETE") == 0 && r.recommendation.find("CU/E32 was not measured") != std::string::npos,
        "Unavailable Pioneer CU does not imply a healthy disc");
    r = base();
    DiscRot::Finalize(r);
    check(r.rotRiskLevel == "NONE" && r.readabilityStatus == "NO READ PROBLEMS OBSERVED" &&
        r.recommendation.find("does not establish a healthy disc") != std::string::npos,
        "Clean observations are limited to what was measured");

    auto ranges = DiscRot::NormalizeAudioRanges({{400, 499}, {0, 99}, {50, 199}, {800, 799}});
    check(ranges == DiscRot::AudioRanges({{0, 199}, {400, 499}}), "Cache eviction merges overlaps and preserves data-track gaps");
    auto noCancel = [] { return false; };
    int readCount = 0;
    bool validRanges = true;
    auto readBlock = [&](DWORD lba, DWORD count, BYTE*) {
        readCount += static_cast<int>(count);
        for (DWORD i = 0; i < count; ++i)
            validRanges = validRanges && (lba + i < 200 || (lba + i >= 400 && lba + i < 500)) &&
                !(lba + i >= 75 && lba + i <= 225);
        return true;
    };
    check(DiscRot::EvictAudioCache(150, ranges, 128, readBlock, noCancel) &&
        readCount * AUDIO_SECTOR_SIZE > 128 * 1024 && validRanges,
        "Eviction reads beyond reported cache capacity without touching target or non-audio gaps");
    readCount = 0;
    check(!DiscRot::EvictAudioCache(150, ranges, 0, readBlock, noCancel) && readCount == 0,
        "Unknown cache capacity is explicitly unverified");
    check(!DiscRot::EvictAudioCache(150, ranges, 8192, readBlock, noCancel) && readCount == 0,
        "Short audio ranges cannot pretend to evict a larger cache");
    check(!DiscRot::EvictAudioCache(150, ranges, 8, [](DWORD, DWORD, BYTE*) { return false; }, noCancel),
        "An eviction read failure is not accepted as a cache flush");
    check(!DiscRot::EvictAudioCache(150, ranges, 8, readBlock, [] { return true; }) && readCount == 0,
        "Cache eviction honors cancellation before reading");

    // A finite LRU audio cache with changing media data. Matching cached reads
    // would hide the changes; the production eviction/consistency helpers must
    // expose them, without any Accurate Stream exception.
    std::list<std::pair<DWORD, BYTE>> cache;
    BYTE generation = 0;
    auto cachedRead = [&](DWORD lba, BYTE* data) {
        auto found = std::find_if(cache.begin(), cache.end(), [&](const auto& item) { return item.first == lba; });
        BYTE value;
        if (found != cache.end()) { value = found->second; cache.erase(found); }
        else value = lba == 300 ? ++generation : BYTE{0};
        cache.emplace_back(lba, value);
        if (cache.size() > 3) cache.pop_front(); // 8 KiB buffer holds three audio sectors.
        std::memset(data, value, AUDIO_SECTOR_SIZE);
        return true;
    };
    auto eviction = [&](DWORD target) {
        return DiscRot::EvictAudioCache(target, {{0, 999}}, 8,
            [&](DWORD lba, DWORD count, BYTE* data) {
                for (DWORD i = 0; i < count; ++i) cachedRead(lba + i, data + i * AUDIO_SECTOR_SIZE);
                return true;
            }, noCancel);
    };
    auto checked = DiscRot::CheckReadConsistency(300, 3, cachedRead, eviction, noCancel);
    check(checked.readable && checked.cacheCleared && checked.mismatches == 2,
        "Independent rereads expose changing audio hidden by an Accurate Stream drive's cache");
    checked = DiscRot::CheckReadConsistency(300, 3, cachedRead, [](DWORD) { return false; }, noCancel);
    check(checked.readable && !checked.cacheCleared && checked.mismatches == 0,
        "Matching cached rereads remain unverified after eviction fails");
    checked = DiscRot::CheckReadConsistency(300, 3, [](DWORD, BYTE*) { return false; }, eviction, noCancel);
    check(!checked.readable, "Failed reference reads remain explicit failures");
    int passes = 0;
    checked = DiscRot::CheckReadConsistency(300, 3,
        [&](DWORD, BYTE* data) { std::memset(data, 0, AUDIO_SECTOR_SIZE); return ++passes < 2; },
        [](DWORD) { return true; }, noCancel);
    check(!checked.readable, "Failed comparison reads remain explicit failures");
    checked = DiscRot::CheckReadConsistency(300, 1, cachedRead, eviction, noCancel);
    check(!checked.cacheCleared, "A single read cannot establish reread consistency");
    return failures;
}
