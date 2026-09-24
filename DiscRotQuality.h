#pragma once
#include "ScanResults.h"
#include "DiscRotReadConsistency.h"
#include <algorithm>
#include <ostream>

namespace DiscRot {
inline constexpr const char* kCaveat =
    "Error patterns are heuristic observations, not a diagnosis of chemical disc rot, "
    "pitting or remaining life. Scratches, dirt and drive behaviour can produce similar results.";

inline void RecordQualityEvidence(const QCheckResult& scan, bool complete, DiscRotAnalysis& result) {
    result.qualityScanMethod = scan.scanMethod;
    result.qualityStartup=scan.startup;
    result.qualityStartupCacheCleared=scan.startupCacheCleared;
    result.qualitySamples = scan.samples;
    result.qualityScanComplete = complete && !scan.samples.empty();
    result.qualityCountersRecorded = !scan.samples.empty();
    result.qualityCuMeasured = scan.cuMeasured && result.qualityCountersRecorded;
    const bool countersActive = std::any_of(scan.samples.begin(), scan.samples.end(),
        [](const QCheckSample& sample) {
            return sample.c1 > 0 || sample.c2 > 0 || sample.cu > 0 || sample.pioneerE22 > 0;
        });
    // Raw counts and measured coverage survive a failed phase. Completion and
    // decoder activity gate rates/ratings, never whether observations are kept.
    result.c1 = ScanQuality::SummarizeC1(C1Intervals(scan.samples),
        result.qualityScanComplete && !scan.c1Unverified && countersActive);
    result.c1RequestedSectors = scan.totalSectors;
    // Retain observed failures even when a later poll fails or a later pass is clean.
    const bool hasC2 = result.qualityCountersRecorded && scan.scanMethod.find("Pioneer") == std::string::npos;
    result.qualityC2Count = hasC2 ? scan.totalC2 : 0;
    result.qualityCUCount = hasC2 && result.qualityCuMeasured ? scan.totalCU : 0;
}

inline bool HasConfirmedFailure(const DiscRotAnalysis& result) {
    return result.totalReadFailures > 0 || result.verificationReadFailures > 0 || result.consistencyReadFailures > 0 ||
        result.qualityCUCount > 0 ||
        HasPioneerCdCheckLoss(result);
}

// Completeness is independent of observed errors and of display strings.
// Derive it from current evidence so refinalizing cannot retain a stale state.
inline bool HasLimitedReadConfidence(const DiscRotAnalysis& result) {
    return result.totalRereadTests == 0 || result.consistencyUnverifiedSamples > 0 ||
        (result.pioneerDrive && !result.pioneerCdCheckRun);
}

inline void RecordPhase1Evidence(const Phase1SectorResult& sector, DiscRotAnalysis& result) {
    if (sector.cancelled) return;
    if (!sector.readable) ++result.totalReadFailures;
    if (sector.recoveredReadFailure) ++result.recoveredReadFailures;
    if (sector.verificationReadFailure) ++result.verificationReadFailures;
    if (sector.observedC2) ++result.phase1C2Sectors;
}

inline std::string AssessPatternRisk(const DiscRotAnalysis& analysis) {
    int score = 0;
    if (analysis.edgeConcentration) score += 25;
    if (analysis.progressivePattern) score += 25;
    if (analysis.pinholePattern) score += 15;
    if (analysis.readInstability) score += 20;
    if (analysis.inconsistencyRate > 10.0) score += 15;
    if (analysis.maxC2InSingleSector >= 100) score += 20;
    else if (analysis.maxC2InSingleSector >= 50) score += 10;
    if (score >= 75) return "CRITICAL";
    if (score >= 50) return "HIGH";
    if (score >= 30) return "MODERATE";
    if (score >= 10) return "LOW";
    return "NONE";
}

inline int RiskRank(const std::string& risk) {
    if (risk == "CRITICAL") return 4;
    if (risk == "HIGH") return 3;
    if (risk == "MODERATE") return 2;
    if (risk == "LOW") return 1;
    return 0;
}

inline void Finalize(DiscRotAnalysis& result) {
    auto atLeast = [&](const char* risk) {
        if (RiskRank(result.rotRiskLevel) < RiskRank(risk)) result.rotRiskLevel = risk;
    };
    if (result.rotRiskLevel.empty()) result.rotRiskLevel = "NONE";
    if (HasConfirmedFailure(result)) {
        atLeast("HIGH");
        result.readabilityStatus = "READ FAILURE OR UNCORRECTABLE ERRORS OBSERVED";
        result.recommendation = "Back up immediately using secure extraction and independent verification. "
            "Failed reads or uncorrectable errors were observed; the physical cause is unconfirmed.";
    }
    else if (result.phase1C2Sectors > 0 || result.qualityC2Count > 0 ||
        result.maxC2InSingleSector > 0) {
        atLeast("MODERATE");
        result.readabilityStatus = "C2 ACTIVITY OBSERVED";
        result.recommendation = "Back up promptly and verify the extraction independently. "
            "C2 activity was observed even if subsequent reads matched; chemical rot is not established.";
    }
    else if (result.inconsistentSectors > 0) {
        atLeast("LOW");
        result.readabilityStatus = "INCONSISTENT READS OBSERVED";
        result.recommendation = "Back up promptly using secure extraction and independent verification. "
            "Some rereads differed; the cause is unconfirmed.";
    }
    else if (result.recoveredReadFailures > 0) {
        atLeast("LOW");
        result.readabilityStatus = "RECOVERED READ FAILURES OBSERVED";
        result.recommendation = "Some initial reads failed but succeeded on retry. Make a verified backup; "
            "recovery does not erase the earlier read problems.";
    }
    else {
        result.readabilityStatus = HasLimitedReadConfidence(result) ? "INCOMPLETE - READ CONFIDENCE LIMITED" : "NO READ PROBLEMS OBSERVED";
        if (RiskRank(result.rotRiskLevel) >= 2)
            result.recommendation = "Elevated error patterns were observed. Back up promptly and verify independently; the cause is unconfirmed.";
        else if (result.rotRiskLevel == "LOW")
            result.recommendation = "Minor error patterns were observed. Make a verified backup and monitor changes.";
        else
            result.recommendation = "No degradation pattern was detected in measured data. This does not establish a healthy disc or predict remaining life.";
    }
    if (result.totalRereadTests == 0 || result.consistencyUnverifiedSamples > 0)
        result.recommendation += " Independent rereads were not established for all samples; matching cached data cannot prove consistency.";
    if (result.pioneerDrive && !result.pioneerCdCheckRun)
        result.recommendation += result.pioneerCdCheckPartial
            ? " Pioneer CD Check coverage was incomplete; recorded evidence is retained. Verify any rip independently."
            : " Pioneer CU/E32 was not measured; verify any rip independently.";
}

inline void PrintReadEvidence(std::ostream& out, const DiscRotAnalysis& result, const char* prefix = "") {
    out << prefix << "Read assessment: " << result.readabilityStatus << '\n'
        << prefix << "Read confidence: " << (HasLimitedReadConfidence(result) ? "LIMITED" : "REQUIRED READ CHECKS AVAILABLE") << '\n'
        << prefix << "Phase 1 primary reads still failed after retry: " << result.totalReadFailures << '\n'
        << prefix << "Phase 1 initial failures recovered on retry: " << result.recoveredReadFailures << '\n'
        << prefix << "Phase 1 failed C2 verification reads: " << result.verificationReadFailures << '\n'
        << prefix << "Phase 1 sectors with observed C2: " << result.phase1C2Sectors << '\n'
        << prefix << "Reread samples with failed reads: " << result.consistencyReadFailures << '\n'
        << prefix << "Reread samples without established cache eviction: " << result.consistencyUnverifiedSamples
        << " / " << result.totalRereadTests << '\n';
    if (result.qualityCountersRecorded) {
        out << prefix << "Phase 0 method: " << result.qualityScanMethod
            << (result.qualityScanComplete ? " (complete)" : " (partial; positive error evidence retained)") << '\n';
        if (result.qualityScanMethod.find("Pioneer") == std::string::npos) {
            out << prefix << "Phase 0 observed C2 count: " << result.qualityC2Count << '\n'
                << prefix << "Phase 0 observed CU count: ";
            if (result.qualityCuMeasured) out << result.qualityCUCount << '\n';
            else out << "NOT MEASURED\n";
        }
    }
    out << prefix << kCaveat << '\n';
}
} // namespace DiscRot
