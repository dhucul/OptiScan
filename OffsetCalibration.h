// ============================================================================
// OffsetCalibration.h - Automatic offset detection via AccurateRip verification
// ============================================================================
#pragma once
#include "ScsiDrive.h"
#include <vector>
#include <functional>

struct CalibrationResult {
    int detectedOffset = 0;
    int confidence = 0;          // 0-100%
    int matchingTracks = 0;
    int totalTracks = 0;
    std::string discTitle;
    bool success = false;
};

class OffsetCalibration {
public:
    explicit OffsetCalibration(ScsiDrive& drive) : m_drive(drive) {}
    
    // Calibrate using a disc in the AccurateRip database
    // Tests offsets from -2000 to +2000 samples
    CalibrationResult CalibrateWithDisc(
        std::function<void(int progress, const std::string& status)> progressCallback = nullptr);
    
    // Quick calibration testing common offsets only
    CalibrationResult QuickCalibrate(
        std::function<void(int progress, const std::string& status)> progressCallback = nullptr);
    
private:
    ScsiDrive& m_drive;
    
    // Common drive offsets to test first (covers ~90% of drives)
    static constexpr int CommonOffsets[] = {
        0, +6, +12, +30, +48, +97, +99, +102, +103, +116, +120,
        +667, +679, +685, +688, +690, +691, +694, +702, +733, +738,
        -24, -472, -491, -582, -1164
    };
    
    uint32_t CalculateAccurateRipCRC(const std::vector<int16_t>& samples, 
                                      int trackNumber, int totalTracks);
    std::vector<uint32_t> FetchAccurateRipChecksums(const std::string& discId);
    std::string CalculateDiscId();
    uint32_t ReadTrackAndCalculateCRC(int trackNumber, int offsetSamples);
};