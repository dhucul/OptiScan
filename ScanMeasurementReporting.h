#pragma once
#include "ScanResults.h"
#include <ostream>

// Explain a limitation of the selected method without declaring that the
// drive lacks a capability that a different backend may successfully expose.
inline void PrintMethodC1Summary(std::ostream& out, const BlerResult& result,
    const char* indent = "  ") {
    if (!result.HasC1Observations()) {
        ScanQuality::PrintWrapped(out, "C1 was not measured by this scan method. "
            "The hardware quality scan (option 7) uses a separate method and may report C1 on this drive.", indent);
    } else {
        if (!result.hasC1Data || !result.c1.verified)
            ScanQuality::PrintWrapped(out, "C1 readings were recorded, but their measurement is unverified.", indent);
        ScanQuality::PrintC1Summary(out, result.c1, result.totalSectors, indent);
    }
}

inline void PrintUnverifiedC2Summary(std::ostream& out, const BlerResult& result,
    const char* indent = "  ", bool includeStatus = true) {
    if (includeStatus) out << indent << "Status: " << result.C2MeasurementLabel() << "\n";
    if (result.c2PointerDataRecorded) {
        out << indent << "C2 activity reported: " << result.totalC2Errors << "\n";
        out << indent << "Recovered C2 activity reported: " << result.recoveredC2Errors << "\n";
        ScanQuality::PrintWrapped(out, "These are readings returned by this method. "
            "OptiScan has not independently validated its C2 error detection. "
            "A zero reading alone cannot establish that the disc is error-free.", indent);
    } else {
        ScanQuality::PrintWrapped(out, "No C2 pointer readings were recorded in this scan. "
            "C2 totals and rates are unavailable.", indent);
    }
    out << indent << "Read failures: " << result.totalReadFailures << "\n";
    if (result.HasConfirmedFailure())
        out << indent << "Assessment: BAD - confirmed read failure or data loss.\n";
    ScanQuality::PrintWrapped(out, "Hardware quality scan (option 7) uses a separate method. "
        "C1/C2/CU availability can differ between methods on the same drive.", indent);
    ScanQuality::PrintWrapped(out, "Missing or unverified C2 readings cannot establish a clean-disc result.", indent);
}
