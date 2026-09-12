#pragma once
#include "InterruptHandler.h"
#include <imapi2.h>
#include <wrl/client.h>
#include <condition_variable>
#include <mutex>
#include <thread>

template<class Writer>
class ImapiMediaSession {
    Writer* writer;
    bool prepared = true;
public:
    explicit ImapiMediaSession(Writer* value) : writer(value) {}
    ImapiMediaSession(const ImapiMediaSession&) = delete;
    ImapiMediaSession& operator=(const ImapiMediaSession&) = delete;
    HRESULT Release() {
        if (!prepared) return S_OK;
        prepared = false;
        return writer->ReleaseMedia();
    }
    ~ImapiMediaSession() { Release(); }
};

// The synchronous IMAPI write must remain on its owning workflow thread.
// A COM-marshaled cancellation interface stays available even if progress
// notifications fail or the writer has already buffered the source stream.
class ImapiCancelMonitor {
    std::mutex mutex;
    std::condition_variable changed;
    std::thread thread;
    bool ready = false, usable = false, stopped = false, streamConsumed = false;
    static void ReleaseMarshaled(IStream* stream) {
        if (FAILED(CoReleaseMarshalData(stream)))
            OutputDebugStringA("Could not release unused IMAPI marshaling data.\n");
        stream->Release();
    }
public:
    ImapiCancelMonitor(IUnknown* writer, bool raw) {
        IStream* marshaled = nullptr;
        if (FAILED(CoMarshalInterThreadInterfaceInStream(IID_IUnknown, writer, &marshaled))) return;
        try {
            thread = std::thread([this, marshaled, raw] {
                const HRESULT initialized = CoInitializeEx(nullptr, COINIT_MULTITHREADED);
                Microsoft::WRL::ComPtr<IUnknown> target;
                HRESULT hr = initialized;
                if (SUCCEEDED(initialized))
                    hr = CoGetInterfaceAndReleaseStream(marshaled, IID_PPV_ARGS(&target));
                Microsoft::WRL::ComPtr<IDiscFormat2RawCD> rawWriter;
                Microsoft::WRL::ComPtr<IDiscFormat2TrackAtOnce> taoWriter;
                if (SUCCEEDED(hr)) hr = raw ? target.As(&rawWriter) : target.As(&taoWriter);
                {
                    std::lock_guard lock(mutex);
                    streamConsumed = SUCCEEDED(initialized);
                    usable = SUCCEEDED(hr);
                    ready = true;
                }
                changed.notify_all();
                if (SUCCEEDED(hr)) {
                    std::unique_lock lock(mutex);
                    while (!stopped) {
                        if (g_interrupt.IsInterrupted()) {
                            lock.unlock();
                            if (raw) rawWriter->CancelWrite();
                            else taoWriter->CancelAddTrack();
                            lock.lock();
                        }
                        changed.wait_for(lock, std::chrono::milliseconds(100), [this] { return stopped; });
                    }
                }
                taoWriter.Reset(); rawWriter.Reset(); target.Reset();
                if (SUCCEEDED(initialized)) CoUninitialize();
            });
        } catch (...) {
            ReleaseMarshaled(marshaled);
            return;
        }
        std::unique_lock lock(mutex);
        changed.wait(lock, [this] { return ready; });
        const bool releaseUnused = !streamConsumed;
        lock.unlock();
        // If COM initialization failed in the helper, release the unused
        // packet in the originating, still-initialized apartment.
        if (releaseUnused) ReleaseMarshaled(marshaled);
    }
    ImapiCancelMonitor(const ImapiCancelMonitor&) = delete;
    ImapiCancelMonitor& operator=(const ImapiCancelMonitor&) = delete;
    bool Ready() const { return usable; }
    ~ImapiCancelMonitor() {
        { std::lock_guard lock(mutex); stopped = true; }
        changed.notify_all();
        if (thread.joinable()) thread.join();
    }
};
