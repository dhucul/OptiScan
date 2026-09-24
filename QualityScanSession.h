#pragma once
#include <functional>
#include <utility>

// A failed stop leaves the session open and retryable. Callers must stop
// successfully before rating a result or entering another measurement phase.
class QualityScanSession {
	std::function<bool()> stop_;
	bool stopped_ = false;
	bool abandoned_ = false;
public:
	explicit QualityScanSession(std::function<bool()> stop) : stop_(std::move(stop)) {}
	QualityScanSession(const QualityScanSession&) = delete;
	QualityScanSession& operator=(const QualityScanSession&) = delete;
	~QualityScanSession() { try { Stop(); } catch (...) {} }
	bool Stop() {
		if (abandoned_) return false;
		if (stopped_) return true;
		for (int attempt = 0; attempt < 2; ++attempt)
			if (stop_()) { stopped_ = true; return true; }
		return false;
	}
	// Closing the handle is terminal for this session. Do not retry vendor
	// commands from a later Stop call or the destructor after that transition.
	template<class Close>
	bool StopOrClose(Close&& close) {
		if (abandoned_) return false;
		if (Stop()) return true;
		abandoned_ = true;
		close();
		return false;
	}
};
