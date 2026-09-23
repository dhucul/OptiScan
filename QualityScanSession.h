#pragma once
#include <functional>
#include <utility>

// A failed stop leaves the session open and retryable. Callers must stop
// successfully before rating a result or entering another measurement phase.
class QualityScanSession {
	std::function<bool()> stop_;
	bool stopped_ = false;
public:
	explicit QualityScanSession(std::function<bool()> stop) : stop_(std::move(stop)) {}
	QualityScanSession(const QualityScanSession&) = delete;
	QualityScanSession& operator=(const QualityScanSession&) = delete;
	~QualityScanSession() { try { Stop(); } catch (...) {} }
	bool Stop() {
		if (stopped_) return true;
		for (int attempt = 0; attempt < 2; ++attempt)
			if (stop_()) { stopped_ = true; return true; }
		return false;
	}
};
