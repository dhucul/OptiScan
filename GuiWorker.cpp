#define NOMINMAX
#include "GuiWorker.h"
#include "InterruptHandler.h"

#include <atomic>
#include <chrono>
#include <mutex>
#include <thread>
#include <utility>
#include <exception>
#include <iostream>
#include <windows.h>

namespace {
    std::atomic<bool> g_running{ false };
    std::atomic<bool> g_done{ false };       // worker has finished but not yet joined
    std::atomic<uint64_t> g_jobId{0};
    thread_local int g_outcome = 1;
    std::thread       g_thread;
    std::mutex        g_threadMutex;
}

namespace GuiWorker {

    bool RunAsync(Job job, Completion afterCompletion) {
        // Reject if another workflow is in progress.
        if (g_running.exchange(true)) return false;

        // Reset interrupt flag for the new workflow.
        InterruptHandler::Instance().SetInterrupted(false);

        try {
            std::lock_guard<std::mutex> lock(g_threadMutex);
            if (g_thread.joinable()) g_thread.join();
            g_done.store(false);
            const auto id = g_jobId.fetch_add(1) + 1;
            g_thread = std::thread([id, j = std::move(job),
                completion = std::move(afterCompletion)]() {
                g_outcome = 1;
                try {
                    j();
                } catch (const std::exception& error) {
                    g_outcome = 1;
                    std::cerr << "\nOperation failed unexpectedly: " << error.what() << "\n";
                } catch (...) {
                    g_outcome = 1;
                    std::cerr << "\nOperation failed with an unexpected exception.\n";
                }
                const int outcome = InterruptHandler::Instance().IsInterrupted() ? 2 : g_outcome;
                g_done.store(true);
                g_running.store(false);
                // Completion callbacks post only; the generation travels with the
                // result so delayed notifications cannot finish a subsequent job.
                try { if (completion) completion(id, outcome); }
                catch (...) { std::cerr << "Could not deliver operation completion.\n"; }
            });
        }
        catch (...) {
            g_done.store(false);
            g_running.store(false);
            return false;
        }
        return true;
    }

    void SetOutcome(int outcome) { g_outcome = outcome; }
    uint64_t CurrentJobId() { return g_jobId.load(); }

    bool IsRunning() { return g_running.load(); }

    void RequestCancel() {
        InterruptHandler::Instance().SetInterrupted(true);
    }

    void ReapIfDone() {
        if (!g_done.exchange(false)) return;
        std::lock_guard<std::mutex> lock(g_threadMutex);
        if (g_thread.joinable()) g_thread.join();
    }

    bool WaitAndJoin(int timeoutMs) {
        // The workflow has returned before g_running clears. The remaining
        // completion callback only posts a notification and must not block;
        // join also waits for that callback before releasing worker resources.
        auto deadline = std::chrono::steady_clock::now()
            + std::chrono::milliseconds(timeoutMs);
        while (g_running.load()) {
            if (std::chrono::steady_clock::now() >= deadline) break;
            MsgWaitForMultipleObjectsEx(0, nullptr, 10, QS_ALLINPUT,
                MWMO_INPUTAVAILABLE | MWMO_ALERTABLE);
            MSG msg{};
            while (PeekMessageW(&msg, nullptr, 0, 0, PM_REMOVE)) {
                if (msg.message == WM_QUIT) {
                    PostQuitMessage(static_cast<int>(msg.wParam));
                    break;
                }
                TranslateMessage(&msg);
                DispatchMessageW(&msg);
            }
        }
        std::lock_guard<std::mutex> lock(g_threadMutex);
        if (g_running.load()) return false;   // timed out — job still executing
        if (g_thread.joinable()) g_thread.join();
        g_done.store(false);
        return true;
    }

    void Detach() {
        std::lock_guard<std::mutex> lock(g_threadMutex);
        if (g_thread.joinable()) g_thread.detach();
    }
}
