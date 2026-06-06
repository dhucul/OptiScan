// ============================================================================
// InterruptHandler.h - GUI-shim version
//
// The console-mode SetConsoleCtrlHandler / _kbhit / _getch path is gone.
// Workflows can call SetInterrupted(true) from a Cancel button to request
// graceful cancellation; the IsInterrupted() / CheckEscapeKey() / CheckInterrupt()
// API is preserved so existing call sites keep compiling.
// ============================================================================
#pragma once

#include <windows.h>
#include <atomic>

class InterruptHandler {
public:
    static InterruptHandler& Instance() {
        static InterruptHandler instance;
        return instance;
    }

    void Install() {}  // No-op in GUI build.

    bool IsInterrupted() const { return m_interrupted.load(); }
    void SetInterrupted(bool value) { m_interrupted.store(value); }

    // Used to be _kbhit() / _getch() for ESC; in the GUI build only the
    // explicit interrupt flag is consulted.
    bool CheckEscapeKey() { return m_interrupted.load(); }

    bool CheckInterrupt() { return m_interrupted.load(); }

    static void PrintExitHelp() {}

private:
    InterruptHandler() = default;
    std::atomic<bool> m_interrupted{ false };
};

#define g_interrupt InterruptHandler::Instance()
