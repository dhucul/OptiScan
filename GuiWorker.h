// ============================================================================
// GuiWorker.h - Run a single workflow on a background thread so the GUI
// stays responsive during long disc reads / writes.
//
// Only one workflow runs at a time. RunAsync returns false if another is
// already in progress. RequestCancel sets the global interrupt flag that the
// workflow loops poll.
// ============================================================================
#pragma once

#include <functional>

namespace GuiWorker {
    using Job = std::function<void()>;

    // Start `job` on the worker thread. Returns false if a workflow is
    // already running; the caller should ignore the click in that case.
    bool RunAsync(Job job);

    // True while a workflow is in flight.
    bool IsRunning();

    // Set the global interrupt flag. Workflows that periodically check
    // InterruptHandler::IsInterrupted() will exit at the next checkpoint.
    void RequestCancel();

    // Join the worker thread if it is finished. Safe to call from the UI.
    void ReapIfDone();

    // Wait up to timeoutMs for a running workflow to finish, then join it.
    // Returns true if the worker finished and was joined (or nothing was
    // running); false if it was still running when the timeout expired. Used
    // on shutdown so the worker stops touching shared state (g_disc, g_copier,
    // GuiSink, ...) BEFORE those are torn down — joining, not detaching, is
    // what prevents a use-after-free during static destruction.
    bool WaitAndJoin(int timeoutMs);

    // Detach the worker thread (used on app exit to avoid std::terminate).
    // The worker keeps running until the process exits.
    void Detach();
}
