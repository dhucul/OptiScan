// ============================================================================
// Progress.h - Console progress indicator with slow-read awareness
// ============================================================================
#pragma once

#include "DiscTypes.h"
#include <chrono>
#include <functional>
#include <iomanip>
#include <iostream>
#include <mutex>
#include <sstream>
#include <string>

// After Windows headers
#ifdef max
#undef max
#endif
#ifdef min
#undef min
#endif

class ProgressIndicator {
public:
    explicit ProgressIndicator(int barWidth = 35)
        : m_barWidth(barWidth) {}

    void SetLabel(const std::string& label) { m_label = label; }

    void Start() {
        m_current = 0;
        m_total = 0;
        m_lastPercent = -1;
        m_smoothedSpeed = 0;
        m_instantSpeed = 0;
        m_retryCount = 0;
        m_stallCount = 0;
        m_lastLineLength = 0;

        auto now = std::chrono::steady_clock::now();
        m_startTime = now;
        m_lastUpdateTime = now;
        m_lastSectorTime = now;
    }

    void AddRetries(int count) {
        std::lock_guard<std::mutex> lock(m_outputMutex);  // Thread-safe increment
        m_retryCount += count;
    }

    void Update(int current, int total) {
        if (total <= 0) return;  // Validate input
        m_current = std::min(current, total);  // Clamp to valid range
        m_total = total;

        auto now = std::chrono::steady_clock::now();
        auto sectorElapsedMs = GetElapsedMs(m_lastSectorTime, now);
        m_lastSectorTime = now;

        bool isStalled = sectorElapsedMs > kStallThresholdMs;
        if (isStalled) m_stallCount++;

        UpdateSpeeds(sectorElapsedMs, now);

        if (!ShouldUpdateDisplay(now, isStalled)) return;

        int pct = m_total > 0 ? m_current * 100 / m_total : 0;
        int eta = CalculateEta();

        std::string output = FormatProgressLine(pct, eta, isStalled);
        PrintProgress(output);
    }

    void Finish(bool success, int finalTotal = -1) {
        if (finalTotal > 0) m_total = finalTotal;
        if (success) Update(m_total, m_total);  // Only show 100% on success

        auto totalSeconds = std::chrono::duration_cast<std::chrono::seconds>(
            std::chrono::steady_clock::now() - m_startTime).count();

        std::lock_guard<std::mutex> lock(m_outputMutex);
        std::cout << "\n  " << (success ? "Done" : "Failed");
        if (totalSeconds > 0) {
            std::cout << " in " << FormatTime(static_cast<int>(totalSeconds));
        }
        if (m_retryCount > 0) {
            std::cout << " (" << m_retryCount << " retries)";
        }
        std::cout << "\n";

        m_lastLineLength = 0;
    }

    int GetRetryCount() const { return m_retryCount; }
    int GetStallCount() const { return m_stallCount; }

private:
    // Timing thresholds (milliseconds)
    static constexpr int64_t kStallThresholdMs = 2000;
    static constexpr int64_t kMaxSectorTimeMs = 10000;
    static constexpr int64_t kForceUpdateIntervalMs = 1000;
    static constexpr int64_t kSlowThrottleMs = 50;
    static constexpr int64_t kFastThrottleMs = 100;
    static constexpr double kSlowSpeedThreshold = 500.0 * 1024;

    // Smoothing factors
    static constexpr double kSmoothingFactor = 0.9;
    static constexpr double kInstantSpeedRatio = 0.3;

    int64_t GetElapsedMs(
        const std::chrono::steady_clock::time_point& from,
        const std::chrono::steady_clock::time_point& to) const {
        return std::chrono::duration_cast<std::chrono::milliseconds>(to - from).count();
    }

    void UpdateSpeeds(int64_t sectorElapsedMs,
                      const std::chrono::steady_clock::time_point& now) {
        if (sectorElapsedMs > 0 && sectorElapsedMs < kMaxSectorTimeMs) {
            m_instantSpeed = static_cast<double>(AUDIO_SECTOR_SIZE) / (sectorElapsedMs / 1000.0);
        }

        auto totalElapsedMs = GetElapsedMs(m_startTime, now);
        double avgSpeed = (totalElapsedMs > 0 && m_current > 0)
            ? static_cast<double>(m_current) * AUDIO_SECTOR_SIZE / (totalElapsedMs / 1000.0)
            : 0;

        m_smoothedSpeed = (m_smoothedSpeed == 0)
            ? avgSpeed
            : m_smoothedSpeed * kSmoothingFactor + avgSpeed * (1.0 - kSmoothingFactor);
    }

    bool ShouldUpdateDisplay(const std::chrono::steady_clock::time_point& now, bool isStalled) {
        auto displayElapsedMs = GetElapsedMs(m_lastUpdateTime, now);
        int pct = m_total > 0 ? m_current * 100 / m_total : 0;

        bool forceUpdate = (pct != m_lastPercent) || isStalled || (displayElapsedMs > kForceUpdateIntervalMs);
        int64_t throttleMs = (m_smoothedSpeed < kSlowSpeedThreshold) ? kSlowThrottleMs : kFastThrottleMs;

        if (!forceUpdate && displayElapsedMs < throttleMs) return false;

        m_lastUpdateTime = now;
        m_lastPercent = pct;
        return true;
    }

    int CalculateEta() const {
        if (m_smoothedSpeed <= 0 || m_current >= m_total) return -1;
        return static_cast<int>((m_total - m_current) * AUDIO_SECTOR_SIZE / m_smoothedSpeed);
    }

    std::string FormatTime(int seconds) const {
        std::ostringstream ss;
        if (seconds >= 3600) {
            ss << seconds / 3600 << "h "
               << std::setfill('0') << std::setw(2) << (seconds % 3600) / 60 << "m "
               << std::setfill('0') << std::setw(2) << seconds % 60 << "s";
        } else if (seconds >= 60) {
            ss << seconds / 60 << "m "
               << std::setfill('0') << std::setw(2) << seconds % 60 << "s";
        } else {
            ss << seconds << "s";
        }
        return ss.str();
    }

    std::string FormatProgressLine(int pct, int eta, bool isStalled) const {
        std::ostringstream ss;

        // Progress bar with Unicode blocks
        ss << "\r" << m_label << " [";
        int filled = pct * m_barWidth / 100;
        for (int i = 0; i < m_barWidth; i++) {
            ss << (i < filled ? "\xe2\x96\x88" : "\xe2\x96\x91"); // █ or ░
        }
        ss << "] " << std::setw(3) << pct << "%";

        // Sector count — right-align current to match total's digit count
        int totalDigits = static_cast<int>(std::to_string(m_total).size());
        ss << " " << std::setw(totalDigits) << m_current << "/" << m_total;

        // Speed display — always occupy a fixed-width field
        double displaySpeed = m_smoothedSpeed;
        if (m_instantSpeed > 0 && m_instantSpeed < m_smoothedSpeed * kInstantSpeedRatio) {
            displaySpeed = m_instantSpeed;
        }
        if (displaySpeed > 0) {
            ss << " " << std::setw(4) << static_cast<int>(displaySpeed / 1024) << "KB/s";
        } else {
            ss << std::string(9, ' ');
        }

        // ETA — fixed-width field for consistent column alignment
        if (eta >= 0) {
            std::string etaStr = "ETA:" + FormatTime(eta);
            ss << " " << std::left << std::setw(10) << etaStr << std::right;
        } else {
            ss << std::string(11, ' ');
        }

        // Retry/stall indicators
        if (m_retryCount > 0 || m_stallCount > 0) {
            ss << " [";
            if (m_retryCount > 0) ss << "R:" << m_retryCount;
            if (m_retryCount > 0 && m_stallCount > 0) ss << "/";
            if (m_stallCount > 0) ss << "S:" << m_stallCount;
            ss << "]";
        }

        if (isStalled) {
            ss << " <SLOW>";
        }

        return ss.str();
    }

    void PrintProgress(const std::string& output) {
        std::string padded = output;
        if (static_cast<int>(output.size()) < m_lastLineLength) {
            padded.append(m_lastLineLength - output.size(), ' ');
        }

        {
            std::lock_guard<std::mutex> lock(m_outputMutex);
            std::cout << padded << std::flush;
        }

        m_lastLineLength = static_cast<int>(padded.size());
    }

    // Configuration
    int m_barWidth;
    std::string m_label;

    // Progress state
    int m_total = 0;
    int m_current = 0;
    int m_lastPercent = -1;

    // Speed tracking
    double m_smoothedSpeed = 0;
    double m_instantSpeed = 0;

    // Statistics
    int m_retryCount = 0;
    int m_stallCount = 0;

    // Timing
    std::chrono::steady_clock::time_point m_startTime;
    std::chrono::steady_clock::time_point m_lastUpdateTime;
    std::chrono::steady_clock::time_point m_lastSectorTime;

    // Output management
    std::mutex m_outputMutex;
    int m_lastLineLength = 0;
};

inline std::function<void(int, int)> MakeProgressCallback(ProgressIndicator* p) {
    return [p](int c, int t) { p->Update(c, t); };
}