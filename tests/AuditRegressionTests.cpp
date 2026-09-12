#define NOMINMAX
#include "../ArtifactTransaction.h"
#include "../WorkflowChecks.h"
#include "../ImageSource.h"
#include "../TrackFileOutput.h"
#include "../NumericInput.h"
#include "../OpticalDrive.h"
#include "../GuiWorker.h"
#include "../InterruptHandler.h"
#include <atomic>
#include <fstream>
#include <sstream>
#include <stdexcept>
#include <map>

// These tests never open a drive. The only ScsiDrive entry referenced by the
// file-output tests is destruction of the default, unopened OpticalDrive.
void ScsiDrive::Close() {}

namespace {
int failed = 0;
void Check(bool value, const char* name) {
    std::cout << (value ? "[PASS] " : "[FAIL] ") << name << "\n";
    if (!value) ++failed;
}
void Put(const std::filesystem::path& path, const std::string& text) {
    std::ofstream file(path, std::ios::binary); file << text;
}
std::string Get(const std::filesystem::path& path) {
    std::ifstream file(path, std::ios::binary);
    return {std::istreambuf_iterator<char>(file), std::istreambuf_iterator<char>()};
}
}

int RunAuditRegressionTests() {
    {
        int parsed = 99;
        Check(!ParseBoundedInteger(L"erase", 1, 3, parsed) && parsed == 99,
            "Malformed numeric input cannot select the default action");
        Check(!ParseBoundedInteger(L"2xyz", 1, 3, parsed), "Numeric input rejects trailing junk");
        Check(!ParseBoundedInteger(L"4", 1, 3, parsed), "Out-of-range input is rejected instead of clamped");
        Check(!ParseBoundedInteger(L"99999999999999999", 1, 3, parsed), "Numeric overflow is rejected");
        Check(!ParseBoundedInteger(L"", 1, 3, parsed), "Empty numeric input is rejected");
        Check(ParseBoundedInteger(L"  -667 \t", -10000, 10000, parsed) && parsed == -667,
            "Valid signed offsets retain their exact value");
        Check(ParseBoundedInteger(L"0", -4, 4, parsed) && parsed == 0,
            "Zero remains valid for numeric settings that allow it");
    }
    const auto temp = std::filesystem::temp_directory_path() /
        (L"OptiScanAuditTests-" + std::to_wstring(GetCurrentProcessId()) + L"-" + std::to_wstring(GetTickCount64()));
    std::filesystem::create_directory(temp);
    {
        std::istringstream source("ABCDEFGHIJKL");
        char batch[4]{};
        Check(WorkflowChecks::RewindWriteSource(source, 1, 4), "Write batch positioned at destination coordinate");
        source.read(batch, 4);
        Check(std::string(batch, 4) == "EFGH", "Pending write payload is the expected source sector");
        // A not-ready timeout consumed the payload without committing it.
        Check(WorkflowChecks::RewindWriteSource(source, 1, 4), "Failed write can restore the pending source cursor");
        source.read(batch, 4);
        Check(std::string(batch, 4) == "EFGH", "Retry repeats identical data instead of consuming the next batch");
        source.setstate(std::ios::failbit);
        Check(WorkflowChecks::RewindWriteSource(source, 2, 4), "Retry resets stale stream failure state");
    }
    {
        const auto base = temp / L"previous";
        const auto bin = temp / L"previous.bin", cue = temp / L"previous.cue";
        Put(bin, "original-bin"); Put(cue, "original-cue");
        {
            ArtifactTransaction transaction(base);
            Put(transaction.Stage(bin), "new-bin");
            transaction.Stage(cue); // failure before the CUE was created
            Check(!transaction.Commit(), "Incomplete staged set cannot replace previous artifacts");
        }
        Check(Get(bin) == "original-bin" && Get(cue) == "original-cue", "Failed staging preserves existing BIN and CUE");
        {
            ArtifactTransaction transaction(base);
            Put(transaction.Stage(bin), "new-bin"); Put(transaction.Stage(cue), "new-cue");
            HANDLE lock = CreateFileW(cue.c_str(), GENERIC_READ, FILE_SHARE_READ, nullptr, OPEN_EXISTING, 0, nullptr);
            Check(lock != INVALID_HANDLE_VALUE, "Locked destination fault is established");
            Check(!transaction.Commit(), "Publication fails when a later destination is locked");
            if (lock != INVALID_HANDLE_VALUE) CloseHandle(lock);
        }
        Check(Get(bin) == "original-bin" && Get(cue) == "original-cue", "Failed multi-file publication rolls back already replaced files");
        {
            ArtifactTransaction transaction(base);
            Put(transaction.Stage(bin), "new-bin"); Put(transaction.Stage(cue), "new-cue");
            Check(transaction.Commit(), "Complete artifact set can be published");
        }
        Check(Get(bin) == "new-bin" && Get(cue) == "new-cue", "Successful publication replaces the complete set");

        OpticalDrive drive;
        DiscInfo disc;
        TrackInfo track{}; track.trackNumber = 1; track.isAudio = true;
        track.startLBA = track.pregapLBA = track.endLBA = 0;
        disc.tracks.push_back(track); disc.leadOutLBA = 1;
        disc.pregapMode = PregapMode::Include;
        disc.includeSubchannel = false;
        Check(!drive.SaveToFile(disc, base.wstring()), "Actual SaveToFile rejects a missing source sector");
        Check(Get(bin) == "new-bin" && Get(cue) == "new-cue", "Actual SaveToFile failure preserves existing output files");
        disc.rawSectors.emplace_back(static_cast<size_t>(AUDIO_SECTOR_SIZE), static_cast<BYTE>(0x12));
        Check(drive.SaveToFile(disc, base.wstring()), "Actual SaveToFile publishes a complete image");
        Check(Get(bin).size() == AUDIO_SECTOR_SIZE && Get(cue).find("previous.bin") != std::string::npos,
            "Staged CUE references final image name, not the temporary staging file");
    }
    {
        Put(temp / L"album.bin", "audio"); Put(temp / L"unrelated.bin", "other");
        Put(temp / L"album.cue", "FILE \"album.bin\" BINARY\n  TRACK 01 AUDIO\n    INDEX 01 00:00:00\n");
        std::wstring image; std::string error;
        Check(ResolveCueImage((temp / L"album.cue").wstring(), image, error) &&
            std::filesystem::equivalent(image, temp / L"album.bin"), "CUE FILE reference selects its own image in an ambiguous folder");
        Put(temp / L"bad.cue", "FILE \"album.bin\" BINARY\nFILE \"unrelated.bin\" BINARY\n");
        Check(!ResolveCueImage((temp / L"bad.cue").wstring(), image, error), "Image writer rejects multi-file CUE input");
        Put(temp / L"bad.cue", "FILE \"absent.bin\" BINARY\n");
        Check(!ResolveCueImage((temp / L"bad.cue").wstring(), image, error), "Missing referenced image fails before media preparation");
        Put(temp / L"bad.cue", "FILE \"album.bin\" WAVE\n");
        Check(!ResolveCueImage((temp / L"bad.cue").wstring(), image, error), "Audio-file CUE cannot be silently treated as raw binary sectors");
    }
    {
        const auto base = (temp / L"track").wstring();
        const auto wav = temp / L"track.wav", flac = temp / L"track.flac";
        Put(wav, "previous-wav"); Put(flac, "previous-flac");
        std::wstring actual; bool fallback = false, cancelled = false;
        auto stop = [&] { return cancelled; };
        auto write = [&](const std::wstring& path) { Put(path, "new-pcm"); return true; };
        auto encode = [&](const std::wstring&, const std::wstring& path) { Put(path, "new-flac"); return true; };
        auto failWrite = [&](const std::wstring& path) { Put(path, "partial"); return false; };
        Check(!SaveTrackArtifact(base, false, failWrite, encode, stop, actual, fallback),
            "Failed track write cannot publish partial WAV output");
        Check(Get(wav) == "previous-wav" && Get(flac) == "previous-flac" && actual.empty(),
            "Track write failure preserves previous WAV and FLAC");
        auto failEncode = [&](const std::wstring&, const std::wstring& path) { Put(path, "partial-flac"); return false; };
        Check(SaveTrackArtifact(base, true, write, failEncode, stop, actual, fallback) && fallback && actual == wav.wstring(),
            "Failed FLAC conversion publishes its complete WAV fallback");
        Check(Get(wav) == "new-pcm" && Get(flac) == "previous-flac",
            "FLAC fallback does not overwrite a previous FLAC with partial encoded data");
        Check(SaveTrackArtifact(base, true, write, encode, stop, actual, fallback) && !fallback && actual == flac.wstring(),
            "Successful FLAC conversion publishes the encoded file");
        Check(Get(wav) == "new-pcm" && Get(flac) == "new-flac",
            "Successful FLAC publication preserves the unrelated existing WAV");
        auto cancelEncode = [&](const std::wstring&, const std::wstring& path) {
            Put(path, "cancelled-output"); cancelled = true; return true;
        };
        Check(!SaveTrackArtifact(base, true, write, cancelEncode, stop, actual, fallback),
            "Cancellation after encoding prevents publication");
        Check(Get(wav) == "new-pcm" && Get(flac) == "new-flac" && actual.empty(),
            "Cancelled encoding preserves both previous output files");
    }
    {
        int commands = 0;
        g_interrupt.SetInterrupted(true);
        Check(!WorkflowChecks::RunMediaAction([&] { ++commands; return true; }) && commands == 0,
            "Already-cancelled media action issues no destructive command");
        g_interrupt.SetInterrupted(false);
        Check(WorkflowChecks::RunMediaAction([&] { ++commands; return true; }) && commands == 1,
            "Uncancelled media action executes normally");
        WorkflowChecks::RunMediaAction([&] { ++commands; g_interrupt.SetInterrupted(true); return false; });
        Check(!WorkflowChecks::RunMediaAction([&] { ++commands; return true; }) && commands == 2,
            "Cancellation during an attempt prevents its recovery command");
        g_interrupt.SetInterrupted(false);
    }
    Check(!WorkflowChecks::MediaStateKnown(false, 0xFF, 0), "Transport failure is not confirmed media presence");
    Check(!WorkflowChecks::MediaStateKnown(false, 0, 0), "Failed command with no sense is not confirmed media presence");
    Check(WorkflowChecks::MediaStateKnown(false, 2, 0x3A), "Explicit medium-not-present remains a known state");
    Check(WorkflowChecks::MediaStateKnown(true, 0, 0), "Successful readiness is a known media state");
    Check(!WorkflowChecks::HasUniqueQuorum(std::map<int,int>{{0,2},{1,2}}, 2),
        "Competing quorum-sized byte values remain unconfirmed");
    Check(WorkflowChecks::HasUniqueQuorum(std::map<int,int>{{0,3},{1,2}}, 2),
        "A unique quorum winner may be confirmed");
    Check(!WorkflowChecks::HasUniqueQuorum(std::map<int,int>{{0,1}}, 2),
        "A unique value below quorum remains unconfirmed");
    BYTE information[4]{0, 2, 0x0E, 0};
    Check(WorkflowChecks::DiscSessionComplete(information, sizeof(information)), "Closed disc and last session confirm finalization");
    information[2] = 0;
    Check(!WorkflowChecks::DiscSessionComplete(information, sizeof(information)), "Blank but ready disc does not confirm finalization");
    information[2] = 0x05;
    Check(!WorkflowChecks::DiscSessionComplete(information, sizeof(information)), "Incomplete session cannot be reported finalized");
    {
        const auto start = std::chrono::steady_clock::now();
        WorkflowChecks::ScanProgressWatch watch(start);
        Check(!watch.Stalled(0, false, start), "Empty startup response starts a bounded progress window");
        Check(watch.Stalled(0, false, start + std::chrono::seconds(30)), "Repeated empty startup responses time out after 30 seconds");
        Check(!watch.Stalled(75, false, start + std::chrono::seconds(31)), "Forward movement resets stall deadline");
        Check(watch.Stalled(0, false, start + std::chrono::seconds(61)), "Backward or alternating positions cannot reset the stall deadline");
        Check(!watch.Stalled(75, true, start + std::chrono::seconds(62)), "Completed scans do not report a false stall");
    }
    {
        std::atomic<int> outcome{-1}; std::atomic<uint64_t> notified{0};
        Check(GuiWorker::RunAsync([] { throw std::runtime_error("expected regression-test exception"); },
            [&](uint64_t id, int status) { notified = id; outcome = status; }), "Exception test worker starts");
        Check(GuiWorker::WaitAndJoin(5000) && outcome == 1 && notified == GuiWorker::CurrentJobId(),
            "Worker exception delivers failure and the matching job identity");
        const uint64_t previous = notified;
        std::atomic<bool> entered{false};
        Check(GuiWorker::RunAsync([&] {
            entered = true;
            while (!g_interrupt.IsInterrupted()) Sleep(1);
            GuiWorker::SetOutcome(0);
        }, [&](uint64_t id, int status) { notified = id; outcome = status; }), "Cancellation test worker starts");
        Check(!GuiWorker::RunAsync([] {}), "Second workflow cannot overlap an active job");
        GuiWorker::RequestCancel();
        Check(GuiWorker::WaitAndJoin(5000) && outcome == 2 && notified > previous,
            "Cancellation overrides a normal return and preserves generation ordering");
        g_interrupt.SetInterrupted(false);
    }
    std::filesystem::remove_all(temp);
    return failed;
}
