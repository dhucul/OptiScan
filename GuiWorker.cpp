#define NOMINMAX
#include "GuiWorker.h"
#include "InterruptHandler.h"

#include <atomic>
#include <chrono>
#include <mutex>
#include <thread>
#include <utility>
#include <windows.h>

namespace {
    std::atomic<bool> g_running{ false };
    std::atomic<bool> g_done{ false };       // worker has finished but not yet joined
    std::thread       g_thread;
    std::mutex        g_threadMutex;
}

namespace GuiWorker {

    bool RunAsync(Job job, Job afterCompletion) {
        // Reject if another workflow is in progress.
        if (g_running.exchange(true)) return false;

        // Reset interrupt flag for the new workflow.
        InterruptHandler::Instance().SetInterrupted(false);

        try {
            std::lock_guard<std::mutex> lock(g_threadMutex);
            if (g_thread.joinable()) g_thread.join();
            g_done.store(false);
            g_thread = std::thread([j = std::move(job),
                completion = std::move(afterCompletion)]() {
                try {
                    j();
                } catch (...) {
                    // Swallow exceptions so the worker thread always terminates cleanly.
                }
                g_done.store(true);
                g_running.store(false);
                if (completion) completion();
            });
        }
        catch (...) {
            g_done.store(false);
            g_running.store(false);
            return false;
        }
        return true;
    }

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
        // The worker clears g_running as its very last act (after the job has
        // fully returned), so once it reads false the job is no longer touching
        // any shared state and join() completes immediately.
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
