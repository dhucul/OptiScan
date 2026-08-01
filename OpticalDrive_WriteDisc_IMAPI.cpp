#define NOMINMAX
#include "OpticalDrive.h"
#include "ConsoleColors.h"
#include "Drive.h"
#include "WriteDiscInternal.h"
#include <comdef.h>
#include <imapi2.h>
#include <imapi2error.h>
#include <fstream>
#include <iostream>
#include <limits>
#include <vector>
#include <windows.h>
#include <wrl/client.h>
#include <iomanip>
#include <ocidl.h>

#pragma comment(lib, "ole32.lib")

using Microsoft::WRL::ComPtr;

// ============================================================================
// Progress sink: receives DDiscFormat2RawCDEvents::Update notifications during
// the synchronous WriteMedia call and renders a live console progress line.
// IMAPI invokes the single dispinterface method (Update) through IDispatch, so
// only IUnknown + IDispatch need real implementations; the work happens in
// Invoke(), which unpacks the IDiscFormat2RawCDEventArgs progress object.
// ============================================================================
class RawCDProgressSink : public DDiscFormat2RawCDEvents {
public:
	RawCDProgressSink() : m_ref(1), m_ftm(nullptr) {
		// Aggregate the free-threaded marshaler so this raw C++ sink is
		// apartment-agile. Without it, a sink advised from an MTA thread can't
		// be marshaled back (no type info), so IMAPI never delivers Update and
		// the burn appears to hang silently.
		const HRESULT marshalHr =
			CoCreateFreeThreadedMarshaler(static_cast<IDispatch*>(this), &m_ftm);
		if (FAILED(marshalHr)) m_ftm = nullptr;
	}
	virtual ~RawCDProgressSink() { if (m_ftm) m_ftm->Release(); }

	// IUnknown
	STDMETHODIMP QueryInterface(REFIID riid, void** ppv) override {
		if (!ppv) return E_POINTER;
		if (riid == IID_IUnknown || riid == IID_IDispatch ||
			riid == __uuidof(DDiscFormat2RawCDEvents)) {
			*ppv = static_cast<DDiscFormat2RawCDEvents*>(this);
			AddRef();
			return S_OK;
		}
		*ppv = nullptr;
		if (riid == IID_IMarshal && m_ftm) {
			return m_ftm->QueryInterface(riid, ppv);
		}
		return E_NOINTERFACE;
	}
	STDMETHODIMP_(ULONG) AddRef() override { return ++m_ref; }
	STDMETHODIMP_(ULONG) Release() override {
		ULONG r = --m_ref;
		if (r == 0) delete this;
		return r;
	}

	// IDispatch (minimal: the event arrives via Invoke)
	STDMETHODIMP GetTypeInfoCount(UINT* pctinfo) override {
		if (pctinfo) *pctinfo = 0;
		return S_OK;
	}
	STDMETHODIMP GetTypeInfo(UINT, LCID, ITypeInfo** ppTI) override {
		if (ppTI) *ppTI = nullptr;
		return E_NOTIMPL;
	}
	STDMETHODIMP GetIDsOfNames(REFIID, LPOLESTR*, UINT, LCID, DISPID*) override {
		return E_NOTIMPL;
	}
	STDMETHODIMP Invoke(DISPID, REFIID, LCID, WORD,
		DISPPARAMS* params, VARIANT*, EXCEPINFO*, UINT*) override {
		// Update(IDispatch* sender, IDispatch* progress); dispatch args arrive in
		// reverse order, so the progress object is the first entry in rgvarg.
		if (params && params->cArgs >= 1) {
			VARIANTARG& v = params->rgvarg[0];
			IDispatch* progress =
				(v.vt == VT_DISPATCH) ? v.pdispVal :
				(v.vt == (VT_DISPATCH | VT_BYREF) && v.ppdispVal) ? *v.ppdispVal :
				nullptr;
			if (progress) Report(progress);
		}
		return S_OK;
	}

	// DDiscFormat2RawCDEvents::Update -- in this SDK the dispinterface method is
	// also exposed as a vtable entry, so it must be overridden or the class is
	// abstract. IMAPI may deliver the notification through here or via Invoke;
	// both route to Report().
	STDMETHODIMP Update(IDispatch* /*object*/, IDispatch* progress) override {
		if (progress) Report(progress);
		return S_OK;
	}

private:
	void Report(IDispatch* progress) {
		ComPtr<IDiscFormat2RawCDEventArgs> args;
		if (FAILED(progress->QueryInterface(IID_PPV_ARGS(&args)))) {
			// Callback fired but typed args weren't available -- at least show
			// the burn is alive.
			std::cout << "." << std::flush;
			return;
		}

		// IDiscFormat2RawCDEventArgs inherits IWriteEngine2EventArgs, so the
		// sector getters are callable directly on it.
		LONG startLba = 0, sectorCount = 0, lastWritten = 0;
		LONG elapsed = 0, remaining = 0;
		args->get_StartLba(&startLba);
		args->get_SectorCount(&sectorCount);
		args->get_LastWrittenLba(&lastWritten);
		args->get_ElapsedTime(&elapsed);
		args->get_RemainingTime(&remaining);

		int pct = 0;
		bool finishing = false;
		if (sectorCount > 0) {
			long written = lastWritten - startLba + 1;  // +1: LBAs are 0-based
			if (written < 0) written = 0;
			if (written >= sectorCount) { pct = 100; finishing = true; }
			else pct = static_cast<int>(static_cast<long long>(written) * 100 / sectorCount);
		}

		if (finishing) {
			std::cout << "\r  Burning: 100%  finalizing (writing lead-out, please wait)...   "
				<< std::flush;
		}
		else {
			std::cout << "\r  Burning: " << std::setw(3) << pct << "%"
				<< "  elapsed " << elapsed << "s, remaining " << remaining << "s   "
				<< std::flush;
		}
	}

	ULONG m_ref;
	IUnknown* m_ftm;  // free-threaded marshaler (keeps the sink apartment-agile)
};

// ============================================================================
// Helper: Find IMAPI recorder matching a drive letter
// ============================================================================
static bool FindRecorderForDrive(wchar_t driveLetter,
	ComPtr<IDiscRecorder2>& recorder) {

	ComPtr<IDiscMaster2> master;
	HRESULT hr = CoCreateInstance(__uuidof(MsftDiscMaster2), nullptr,
		CLSCTX_ALL, IID_PPV_ARGS(&master));
	if (FAILED(hr)) return false;

	LONG count = 0;
	master->get_Count(&count);

	for (LONG i = 0; i < count; i++) {
		BSTR uid = nullptr;
		if (FAILED(master->get_Item(i, &uid))) continue;

		ComPtr<IDiscRecorder2> rec;
		hr = CoCreateInstance(__uuidof(MsftDiscRecorder2), nullptr,
			CLSCTX_ALL, IID_PPV_ARGS(&rec));
		if (FAILED(hr)) { SysFreeString(uid); continue; }

		hr = rec->InitializeDiscRecorder(uid);
		SysFreeString(uid);
		if (FAILED(hr)) continue;

		SAFEARRAY* mountPoints = nullptr;
		if (SUCCEEDED(rec->get_VolumePathNames(&mountPoints)) && mountPoints) {
			LONG lb = 0, ub = 0;
			SafeArrayGetLBound(mountPoints, 1, &lb);
			SafeArrayGetUBound(mountPoints, 1, &ub);

			// IDiscRecorder2::get_VolumePathNames returns a SAFEARRAY of VARIANT
			// (VT_BSTR elements), not raw BSTRs. Reading an element directly into
			// a BSTR copies the VARIANT header instead of the string pointer --
			// the vt field (VT_BSTR == 8) then looks like the address 0x8 and
			// faults on dereference. Detect the element type and unwrap correctly.
			VARTYPE elemType = VT_EMPTY;
			SafeArrayGetVartype(mountPoints, &elemType);

			for (LONG j = lb; j <= ub; j++) {
				bool matched = false;

				if (elemType == VT_BSTR) {
					BSTR path = nullptr;
					if (SUCCEEDED(SafeArrayGetElement(mountPoints, &j, &path)) && path) {
						matched = (SysStringLen(path) > 0 &&
							towupper(path[0]) == towupper(driveLetter));
						SysFreeString(path);
					}
				}
				else {
					VARIANT v;
					VariantInit(&v);
					if (SUCCEEDED(SafeArrayGetElement(mountPoints, &j, &v))) {
						if (v.vt == VT_BSTR && v.bstrVal && v.bstrVal[0] != L'\0') {
							matched = (towupper(v.bstrVal[0]) == towupper(driveLetter));
						}
						VariantClear(&v);
					}
				}

				if (matched) {
					SafeArrayDestroy(mountPoints);
					recorder = rec;
					return true;
				}
			}
			SafeArrayDestroy(mountPoints);
		}
	}
	return false;
}

// ============================================================================
// Helper: Create IStream by streaming file chunks (avoids full RAM copy)
// ============================================================================
static HRESULT CreateStreamFromFileRange(const std::wstring& filePath,
	long long offset, DWORD length, IStream** ppStream) {

	HRESULT hr = CreateStreamOnHGlobal(nullptr, TRUE, ppStream);
	if (FAILED(hr)) return hr;

	std::ifstream file(filePath, std::ios::binary);
	if (!file.is_open()) return E_FAIL;

	file.seekg(offset);
	if (!file.good()) return E_INVALIDARG;

	constexpr DWORD CHUNK = 256 * 1024;
	std::vector<BYTE> buf(CHUNK);
	DWORD remaining = length;
	while (remaining > 0 && file.good()) {
		DWORD toRead = (remaining < CHUNK) ? remaining : CHUNK;
		file.read(reinterpret_cast<char*>(buf.data()), toRead);
		DWORD got = static_cast<DWORD>(file.gcount());
		if (got == 0) break;

		ULONG written = 0;
		hr = (*ppStream)->Write(buf.data(), got, &written);
		if (FAILED(hr) || written != got) return STG_E_WRITEFAULT;
		remaining -= got;
	}
	if (remaining != 0) return HRESULT_FROM_WIN32(ERROR_HANDLE_EOF);

	LARGE_INTEGER zero = {};
	(*ppStream)->Seek(zero, STREAM_SEEK_SET, nullptr);
	return S_OK;
}

// ============================================================================
// WriteDiscIMAPI - IMAPI2 fallback when raw SCSI CUE SHEET is rejected
// ============================================================================
bool OpticalDrive::WriteDiscIMAPI(const std::wstring& binFile,
	const std::vector<TrackWriteInfo>& tracks,
	DWORD totalSectors, int speed) {

	Console::BoxHeading("IMAPI2 Fallback Write");
	Console::Info("Using Microsoft IMAPI2 API (drive rejected raw SCSI layout)\n");

	// Per-track LBAs from the parsed cue drive the layout; the overall sector
	// count is no longer needed here.
	(void)totalSectors;
	if (tracks.empty()) {
		Console::Error("IMAPI2 write has no tracks to write.\n");
		return false;
	}

	// ── Resolve drive letter from m_drive before closing it ─────────
	// The selected letter is remembered by ScsiDrive::Open. Re-discovering it
	// by vendor/model can target the first of two identical drives and burn the
	// wrong recorder.
	wchar_t driveLetter = m_drive.GetDriveLetter();

	if (driveLetter == L'\0') {
		Console::Error("Cannot identify drive letter for IMAPI2\n");
		return false;
	}

	PrintDriveIdentity(driveLetter);

	// Close SCSI handle so IMAPI2 can get exclusive access
	m_drive.Close();

	// Initialize COM
	HRESULT hr = CoInitializeEx(nullptr, COINIT_MULTITHREADED);
	bool comOwner = SUCCEEDED(hr);
	if (hr == RPC_E_CHANGED_MODE) {
		hr = S_OK;
	}
	if (FAILED(hr)) {
		Console::Error("COM initialization failed\n");
		m_drive.Open(driveLetter);
		return false;
	}

	auto cleanup = [&](bool reopenDrive) -> bool {
		if (comOwner) CoUninitialize();
		if (!reopenDrive) return true;
		if (!m_drive.Open(driveLetter)) {
			Console::Error("Could not reopen the selected drive after IMAPI2 released it.\n");
			return false;
		}
		return true;
	};

	ComPtr<IDiscRecorder2> recorder;
	if (!FindRecorderForDrive(driveLetter, recorder)) {
		Console::Error("IMAPI2 cannot find disc recorder for drive ");
		std::wcout << driveLetter << L":\n";
		cleanup(true);
		return false;
	}

	// ── Attempt 1: True DAO via IRawCDImageCreator (proper multi-track TOC) ─
	// The legacy single-stream RawCD approach collapsed the whole disc into
	// one track. IRawCDImageCreator builds a correct multi-track DAO image
	// (lead-in TOC, per-track boundaries, track-1 pregap) and writes it in a
	// single session via IDiscFormat2RawCD -- the path this drive supports.
	Console::Info("Building Disc-At-Once image (IMAPI2 IRawCDImageCreator)...\n");
	{
		// Flip to true only for albums that must play seamlessly. IMAPI cannot
		// reproduce arbitrary per-track pregap lengths from the .bin; standard
		// mode inserts the Red Book 2-second gap, gapless bleeds the last 2s of
		// each track into the next track's pregap.
		const VARIANT_BOOL kGaplessAudio = VARIANT_FALSE;

		ComPtr<IRawCDImageCreator> creator;
		hr = CoCreateInstance(__uuidof(MsftRawCDImageCreator), nullptr,
			CLSCTX_ALL, IID_PPV_ARGS(&creator));

		if (SUCCEEDED(hr) && !tracks.empty()) {
			creator->put_DisableGaplessAudio(kGaplessAudio == VARIANT_TRUE
				? VARIANT_TRUE : VARIANT_FALSE);
			creator->put_StartingTrackNumber(tracks[0].trackNumber);

			bool addOk = true;
			for (size_t i = 0; i < tracks.size(); i++) {
				const auto& t = tracks[i];
				if (t.endLBA < t.startLBA) { addOk = false; break; }

				DWORD trackSectors = t.endLBA - t.startLBA + 1;
				long long fileOffset = static_cast<long long>(t.startLBA) * AUDIO_SECTOR_SIZE;
				const unsigned long long trackByteCount =
					static_cast<unsigned long long>(trackSectors) * AUDIO_SECTOR_SIZE;
				if (trackByteCount > (std::numeric_limits<DWORD>::max)()) {
					Console::Warning("Track is too large for the IMAPI stream interface\n");
					addOk = false;
					break;
				}
				DWORD trackBytes = static_cast<DWORD>(trackByteCount);

				ComPtr<IStream> trackStream;
				hr = CreateStreamFromFileRange(binFile, fileOffset, trackBytes, &trackStream);
				if (FAILED(hr)) { addOk = false; break; }

				LONG trackIndex = 0;
				hr = creator->AddTrack(IMAPI_CD_SECTOR_AUDIO, trackStream.Get(), &trackIndex);
				if (FAILED(hr)) {
					Console::Warning("AddTrack failed for track ");
					std::cout << t.trackNumber << " (HRESULT: "
						<< std::hex << hr << std::dec << ")\n";
					addOk = false;
					break;
				}
			}

			ComPtr<IStream> image;
			if (addOk && SUCCEEDED(creator->CreateResultImage(&image))) {
				ComPtr<IDiscFormat2RawCD> rawCD;
				hr = CoCreateInstance(__uuidof(MsftDiscFormat2RawCD), nullptr,
					CLSCTX_ALL, IID_PPV_ARGS(&rawCD));

				if (SUCCEEDED(hr)) {
					VARIANT_BOOL supported = VARIANT_FALSE;
					rawCD->IsCurrentMediaSupported(recorder.Get(), &supported);

					if (supported == VARIANT_TRUE &&
						SUCCEEDED(rawCD->put_Recorder(recorder.Get()))) {

						// IMAPI requires a non-empty client name before the media
						// can be prepared, else PrepareMedia fails with
						// E_IMAPI_DF2RAW_CLIENT_NAME_IS_NOT_VALID (0xC0AA0604).
						BSTR clientName = SysAllocString(L"OptiScan");
						rawCD->put_ClientName(clientName);
						SysFreeString(clientName);

						rawCD->put_RequestedSectorType(
							IMAPI_FORMAT2_RAW_CD_SUBCODE_IS_COOKED);
						LONG sectorsPerSecond = static_cast<LONG>(speed) * 75;
						rawCD->SetWriteSpeed(sectorsPerSecond, VARIANT_FALSE);

						hr = rawCD->PrepareMedia();
						if (FAILED(hr)) {
							Console::Warning("IMAPI2 DAO PrepareMedia failed (HRESULT: ");
							std::cout << std::hex << hr << std::dec << ")\n";
						}
						else {
							Console::Info("Writing disc via IMAPI2 DAO (");
							std::cout << tracks.size() << " tracks)...\n";

							// Hook progress notifications so the synchronous
							// WriteMedia call reports a live percentage instead of
							// a silent multi-minute wait.
							DWORD adviseCookie = 0;
							ComPtr<IConnectionPoint> connPoint;
							RawCDProgressSink* sink = new RawCDProgressSink();
							{
								ComPtr<IConnectionPointContainer> cpc;
								HRESULT hrAdvise = rawCD.As(&cpc);
								if (SUCCEEDED(hrAdvise))
									hrAdvise = cpc->FindConnectionPoint(
										__uuidof(DDiscFormat2RawCDEvents), &connPoint);
								if (SUCCEEDED(hrAdvise))
									hrAdvise = connPoint->Advise(sink, &adviseCookie);
								if (FAILED(hrAdvise)) {
									Console::Warning("Progress events unavailable (HRESULT: ");
									std::cout << std::hex << hrAdvise << std::dec
										<< ") - burn runs without a progress readout\n";
								}
							}

							// CreateResultImage() produces an image starting at
							// MSF 95:00:00, which is exactly what WriteMedia expects.
							hr = rawCD->WriteMedia(image.Get());

							if (connPoint && adviseCookie)
								connPoint->Unadvise(adviseCookie);
							sink->Release();
							std::cout << "\n";

							rawCD->ReleaseMedia();

							if (SUCCEEDED(hr)) {
								Console::Success("IMAPI2 DAO write completed\n");
								if (kGaplessAudio == VARIANT_FALSE) {
									Console::Warning("Inter-track pregaps normalized to 2 seconds (drive limitation)\n");
								}
								return cleanup(true);
							}
							Console::Warning("IMAPI2 DAO WriteMedia failed (HRESULT: ");
							std::cout << std::hex << hr << std::dec << ")\n";
						}
					}
				}
			}
		}
		Console::Info("DAO image path unavailable - trying TAO...\n");
	}

	// ── Attempt 2: TAO fallback (functional but adds 2-sec gaps) ────
	Console::Warning("Track-At-Once mode: inter-track gaps will be 2 seconds\n");
	Console::Warning("This will NOT produce a 1:1 copy of the original disc\n");

	ComPtr<IDiscFormat2TrackAtOnce> tao;
	hr = CoCreateInstance(__uuidof(MsftDiscFormat2TrackAtOnce), nullptr,
		CLSCTX_ALL, IID_PPV_ARGS(&tao));
	if (FAILED(hr)) {
		Console::Error("Cannot create IMAPI2 TAO writer (HRESULT: ");
		std::cout << std::hex << hr << std::dec << ")\n";
		cleanup(true);
		return false;
	}

	VARIANT_BOOL supported = VARIANT_FALSE;
	hr = tao->IsCurrentMediaSupported(recorder.Get(), &supported);
	if (FAILED(hr) || supported == VARIANT_FALSE) {
		Console::Error("Current media not supported by IMAPI2\n");
		cleanup(true);
		return false;
	}

	hr = tao->put_Recorder(recorder.Get());
	if (FAILED(hr)) {
		Console::Error("Cannot assign recorder (HRESULT: ");
		std::cout << std::hex << hr << std::dec << ")\n";
		cleanup(true);
		return false;
	}

	// TAO writer also requires a client name before PrepareMedia, else it
	// fails with E_IMAPI_DF2TAO_CLIENT_NAME_IS_NOT_VALID (0xC0AA050F).
	BSTR taoClientName = SysAllocString(L"OptiScan");
	tao->put_ClientName(taoClientName);
	SysFreeString(taoClientName);

	LONG sectorsPerSecond = static_cast<LONG>(speed) * 75;
	tao->SetWriteSpeed(sectorsPerSecond, VARIANT_FALSE);

	hr = tao->PrepareMedia();
	if (FAILED(hr)) {
		Console::Error("IMAPI2 TAO PrepareMedia failed (HRESULT: ");
		std::cout << std::hex << hr << std::dec << ")\n";
		cleanup(true);
		return false;
	}

	Console::Info("Writing ");
	std::cout << tracks.size() << " tracks via IMAPI2 Track-At-Once...\n";

	for (size_t i = 0; i < tracks.size(); i++) {
		const auto& t = tracks[i];
		if (t.endLBA < t.startLBA) {
			Console::Error("Invalid track range supplied to IMAPI2\n");
			tao->ReleaseMedia();
			cleanup(true);
			return false;
		}
		DWORD trackSectors = t.endLBA - t.startLBA + 1;
		const unsigned long long trackByteCount =
			static_cast<unsigned long long>(trackSectors) * AUDIO_SECTOR_SIZE;
		if (trackByteCount > (std::numeric_limits<DWORD>::max)()) {
			Console::Error("Track is too large for the IMAPI stream interface\n");
			tao->ReleaseMedia();
			cleanup(true);
			return false;
		}
		DWORD trackBytes = static_cast<DWORD>(trackByteCount);
		long long fileOffset = static_cast<long long>(t.startLBA) * AUDIO_SECTOR_SIZE;

		Console::Info("  Track ");
		std::cout << t.trackNumber << " (" << trackSectors << " sectors, "
			<< (trackBytes / (1024 * 1024)) << " MB)...\n";

		ComPtr<IStream> stream;
		hr = CreateStreamFromFileRange(binFile, fileOffset, trackBytes, &stream);
		if (FAILED(hr)) {
			Console::Error("Cannot create stream for track ");
			std::cout << t.trackNumber << "\n";
			tao->ReleaseMedia();
			cleanup(true);
			return false;
		}

		hr = tao->AddAudioTrack(stream.Get());
		if (FAILED(hr)) {
			Console::Error("IMAPI2 AddAudioTrack failed for track ");
			std::cout << t.trackNumber << " (HRESULT: "
				<< std::hex << hr << std::dec << ")\n";
			tao->ReleaseMedia();
			cleanup(true);
			return false;
		}

		Console::Success("  Track ");
		std::cout << t.trackNumber << " written\n";
	}

	hr = tao->ReleaseMedia();
	if (FAILED(hr)) {
		Console::Warning("IMAPI2 ReleaseMedia warning (HRESULT: ");
		std::cout << std::hex << hr << std::dec << ")\n";
	}

	Console::Success("IMAPI2 TAO write completed successfully\n");
	Console::Warning("Note: inter-track gaps are 2 seconds (not original layout)\n");
	return cleanup(true);
}
