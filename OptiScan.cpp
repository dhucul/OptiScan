// OptiScan.cpp : Defines the entry point for the application.
//

#include "framework.h"
#include "OptiScan.h"
#include "OptiScanUi.h"
#include "OptiScanWorkflowHost.h"

#include "AccessibleAnnounce.h"
#include "FileUtils.h"
#include "GuiInput.h"
#include "GuiSink.h"
#include "GuiWorker.h"
#include "InterruptHandler.h"
#include "MainMenu.h"
#include "Theme.h"
#include "UiSound.h"
#include <commctrl.h>
#include <string>
#include <vector>

// Progress.h (pulled in via OptiScanWorkflowHost.h) does `#undef min` / `#undef max`,
// which would otherwise break the legacy `min(a, b)` / `max(a, b)` calls in
// the GUI layout / paint code (which freely mix int / long / size_t). Restore
// the original Windows-style macros so the existing call sites keep working.
#undef min
#undef max
#define min(a, b) (((a) < (b)) ? (a) : (b))
#define max(a, b) (((a) < (b)) ? (b) : (a))

#pragma comment(lib, "gdiplus.lib")
#pragma comment(lib, "comctl32.lib")

// Global Variables:
HINSTANCE hInst;                                // current instance
WCHAR szTitle[MAX_LOADSTRING];                  // The title bar text
WCHAR szWindowClass[MAX_LOADSTRING];            // the main window class name
static bool          g_isClosing = false;

static void RequestCleanShutdown(HWND hWnd);

// Forward declarations of functions kept in this code module:
ATOM                MyRegisterClass(HINSTANCE hInstance);
BOOL                InitInstance(HINSTANCE, int);
LRESULT CALLBACK    WndProc(HWND, UINT, WPARAM, LPARAM);
INT_PTR CALLBACK    About(HWND, UINT, WPARAM, LPARAM);

// Detect Wine via its own ntdll export. Wine deliberately exposes
// wine_get_version so apps that don't work under it can refuse to run; the
// symbol does not exist in real Windows ntdll, so this is unambiguous.
static bool IsRunningUnderWine()
{
    HMODULE ntdll = GetModuleHandleW(L"ntdll.dll");
    if (!ntdll) return false;
    return GetProcAddress(ntdll, "wine_get_version") != nullptr;
}

// Detect common hypervisors via SMBIOS strings the system already exposed
// through the registry. Avoids the CPUID hypervisor bit, which false-positives
// on bare-metal Windows 11 with Hyper-V / VBS / WSL2 enabled.
static bool IsRunningInVirtualMachine()
{
    HKEY hKey = nullptr;
    if (RegOpenKeyExW(HKEY_LOCAL_MACHINE,
            L"HARDWARE\\DESCRIPTION\\System\\BIOS",
            0, KEY_READ, &hKey) != ERROR_SUCCESS) {
        return false;
    }

    auto readStr = [&](LPCWSTR name) -> std::wstring {
        wchar_t buf[256] = {};
        DWORD cb = sizeof(buf) - sizeof(wchar_t);
        DWORD type = 0;
        if (RegQueryValueExW(hKey, name, nullptr, &type,
                             reinterpret_cast<LPBYTE>(buf), &cb) == ERROR_SUCCESS
            && (type == REG_SZ || type == REG_EXPAND_SZ)) {
            return std::wstring(buf);
        }
        return {};
    };

    std::wstring manufacturer = readStr(L"SystemManufacturer");
    std::wstring product      = readStr(L"SystemProductName");
    std::wstring biosVendor   = readStr(L"BIOSVendor");
    std::wstring baseBoardMfg = readStr(L"BaseBoardManufacturer");
    RegCloseKey(hKey);

    auto toLower = [](std::wstring s) {
        for (auto& c : s) {
            if (c >= L'A' && c <= L'Z') c = static_cast<wchar_t>(c + (L'a' - L'A'));
        }
        return s;
    };
    const std::wstring m  = toLower(manufacturer);
    const std::wstring p  = toLower(product);
    const std::wstring b  = toLower(biosVendor);
    const std::wstring bb = toLower(baseBoardMfg);
    const std::wstring all = m + L"|" + p + L"|" + b + L"|" + bb;

    // Unambiguous vendor markers — none of these appear on real consumer
    // hardware. "innotek gmbh" is VirtualBox's board manufacturer string.
    static const wchar_t* const markers[] = {
        L"vmware",
        L"virtualbox",
        L"innotek gmbh",
        L"qemu",
        L"bochs",
        L"xen",             // safe: "xen" is not a substring of "xeon"
        L"parallels",
        L"kvm",
    };
    for (const wchar_t* marker : markers) {
        if (all.find(marker) != std::wstring::npos) return true;
    }

    // Hyper-V guest: requires BOTH "Microsoft Corporation" manufacturer and
    // "Virtual Machine" product name. The pair, not either token alone — a
    // real Surface lists "Microsoft Corporation" with a real product name.
    if (m.find(L"microsoft corporation") != std::wstring::npos
        && p.find(L"virtual machine") != std::wstring::npos) {
        return true;
    }
    return false;
}

// Honour an override so a false positive (or a developer testing in a VM)
// isn't a dead-end without a code change.
static bool VmCheckOverridden()
{
    wchar_t buf[8] = {};
    DWORD n = GetEnvironmentVariableW(L"OPTISCAN_ALLOW_VM", buf, _countof(buf));
    return n > 0 && n < _countof(buf) && buf[0] == L'1';
}

int APIENTRY wWinMain(_In_ HINSTANCE hInstance,
                     _In_opt_ HINSTANCE hPrevInstance,
                     _In_ LPWSTR    lpCmdLine,
                     _In_ int       nCmdShow)
{
    UNREFERENCED_PARAMETER(hPrevInstance);
    UNREFERENCED_PARAMETER(lpCmdLine);

    // OptiScan drives physical optical hardware via raw SCSI / MMC commands.
    // Wine's CD-ROM layer and hypervisor SCSI passthrough do not forward these
    // reliably, so quality readings (C2/BLER/jitter/offset/subchannel) would
    // be meaningless or misleading. Refuse to run unless explicitly overridden.
    if (!VmCheckOverridden()) {
        const bool wine = IsRunningUnderWine();
        const bool vm   = !wine && IsRunningInVirtualMachine();
        if (wine || vm) {
            LPCWSTR body = wine
                ? L"OptiScan is disabled under Wine.\n\n"
                  L"This tool issues raw SCSI / MMC commands to physical optical "
                  L"drives. Wine's CD-ROM layer does not forward these reliably, "
                  L"so quality scans, C2/BLER readings, offsets and subchannel "
                  L"results would be inaccurate or misleading."
                : L"OptiScan is disabled inside virtual machines.\n\n"
                  L"This tool issues raw SCSI / MMC commands to physical optical "
                  L"drives. Hypervisor SCSI passthrough does not forward these "
                  L"reliably, so quality scans, C2/BLER readings, offsets and "
                  L"subchannel results would be inaccurate or misleading.\n\n"
                  L"If this is a false positive, set OPTISCAN_ALLOW_VM=1 in the "
                  L"environment to bypass this check.";
            MessageBoxW(nullptr, body, L"OptiScan",
                        MB_OK | MB_ICONERROR | MB_SETFOREGROUND);
            return 0;
        }
    }

    SetProcessDpiAwarenessContext(DPI_AWARENESS_CONTEXT_PER_MONITOR_AWARE_V2);

    INITCOMMONCONTROLSEX commonControls{};
    commonControls.dwSize = sizeof(commonControls);
    commonControls.dwICC = ICC_PROGRESS_CLASS;
    InitCommonControlsEx(&commonControls);
    LoadLibraryW(L"Msftedit.dll");

    InitializeUiResources();

    // Apply the persisted theme before any window paints (defaults to
    // Apple Light when nothing is saved).
    InitializeTheme();

    // Initialize global strings
    LoadStringW(hInstance, IDS_APP_TITLE, szTitle, MAX_LOADSTRING);
    LoadStringW(hInstance, IDC_OPTISCAN, szWindowClass, MAX_LOADSTRING);
    MyRegisterClass(hInstance);
    RegisterUiClasses(hInstance);

    // Perform application initialization:
    if (!InitInstance (hInstance, nCmdShow))
    {
        return FALSE;
    }

    HACCEL hAccelTable = LoadAccelerators(hInstance, MAKEINTRESOURCE(IDC_OPTISCAN));

    MSG msg;

    // Main message loop:
    while (GetMessage(&msg, nullptr, 0, 0))
    {
        // Accelerators win first, then the dialog manager so Tab / Shift+Tab
        // move keyboard focus between the buttons and the output controls.
        // Without IsDialogMessage a plain window does no tab navigation, so
        // none of the controls (including the accessible output EDIT) would
        // be reachable from the keyboard.
        //
        // Translate against the top-level window, not msg.hwnd: the generated
        // WM_COMMAND must reach the main WndProc even when a child control
        // (a button, or the accessible EDIT) currently holds focus. Otherwise
        // Ctrl+Shift+A would be swallowed by the focused child.
        HWND accelTarget = g_hMainWnd ? g_hMainWnd : msg.hwnd;
        if (TranslateAccelerator(accelTarget, hAccelTable, &msg))
        {
            continue;
        }
        // Enter activates the focused command button. The dialog manager
        // reserves Enter for a default button, of which the main window has
        // none, so it would otherwise be swallowed. Ignore auto-repeat (bit 30)
        // so a held key doesn't fire the command repeatedly.
        if (msg.message == WM_KEYDOWN && msg.wParam == VK_RETURN && g_hMainWnd &&
            (msg.lParam & 0x40000000) == 0)
        {
            HWND focused = GetFocus();
            if (focused && IsChild(g_hMainWnd, focused))
            {
                const int ctrlId = GetDlgCtrlID(focused);
                if (ctrlId >= IDC_INFO_BUTTON1 && ctrlId <= IDC_INFO_BUTTON1 + COMMAND_BUTTON_COUNT - 1)
                {
                    SendMessageW(g_hMainWnd, WM_COMMAND,
                                 MAKEWPARAM(ctrlId, BN_CLICKED), (LPARAM)focused);
                    continue;
                }
            }
        }
        // Esc must never close the program. IsDialogMessageW (below) treats this
        // window like a dialog box: when a command button (or the main window)
        // holds focus, it translates Esc into WM_CLOSE / WM_COMMAND(IDCANCEL),
        // and our WM_CLOSE handler runs RequestCleanShutdown - so the app quits.
        // Mouse users rarely have a button focused when pressing Esc, but
        // keyboard and screen-reader users (NVDA auto-enables accessible mode)
        // routinely Tab focus onto a button and press Esc expecting "go back" /
        // "do nothing", which would silently exit. Swallow Esc here, before the
        // dialog manager sees it, whenever focus is inside the main window. Modal
        // prompts and the About box run their own message loops, so their
        // Esc = Cancel behaviour is unaffected.
        if (msg.message == WM_KEYDOWN && msg.wParam == VK_ESCAPE && g_hMainWnd)
        {
            HWND focused = GetFocus();
            if (focused && (focused == g_hMainWnd || IsChild(g_hMainWnd, focused)))
            {
                continue;
            }
        }
        if (g_hMainWnd && IsDialogMessageW(g_hMainWnd, &msg))
        {
            continue;
        }
        TranslateMessage(&msg);
        DispatchMessage(&msg);
    }

    const int result = (int)msg.wParam;

    DestroyUiResources();

    return result;
}



//
//  FUNCTION: MyRegisterClass()
//
//  PURPOSE: Registers the window class.
//
ATOM MyRegisterClass(HINSTANCE hInstance)
{
    WNDCLASSEXW wcex;

    wcex.cbSize = sizeof(WNDCLASSEX);

    wcex.style          = CS_HREDRAW | CS_VREDRAW;
    wcex.lpfnWndProc    = WndProc;
    wcex.cbClsExtra     = 0;
    wcex.cbWndExtra     = 0;
    wcex.hInstance      = hInstance;
    wcex.hIcon = LoadIcon(hInstance, MAKEINTRESOURCE(IDI_OPTISCAN));
    wcex.hCursor        = LoadCursor(nullptr, IDC_ARROW);
    wcex.hbrBackground  = (HBRUSH)GetStockObject(BLACK_BRUSH);
    wcex.lpszMenuName = MAKEINTRESOURCEW(IDC_OPTISCAN);
    wcex.lpszClassName  = szWindowClass;
    wcex.hIconSm        = LoadIcon(wcex.hInstance, MAKEINTRESOURCE(IDI_SMALL));

    return RegisterClassExW(&wcex);
}

//
//   FUNCTION: InitInstance(HINSTANCE, int)
//
//   PURPOSE: Saves instance handle and creates main window
//
//   COMMENTS:
//
//        In this function, we save the instance handle in a global variable and
//        create and display the main program window.
//
BOOL InitInstance(HINSTANCE hInstance, int nCmdShow)
{
   hInst = hInstance; // Store instance handle in our global variable

   // Start in accessible mode automatically if a screen reader is running, so
   // NVDA/JAWS users get the readable EDIT mirror without hunting for a toggle.
   // They can still flip it with View > Accessible output (Ctrl+Shift+A).
   BOOL screenReaderRunning = FALSE;
   SystemParametersInfoW(SPI_GETSCREENREADER, 0, &screenReaderRunning, 0);
   SetInitialAccessibleMode(screenReaderRunning != FALSE);

   RECT workArea;
   SystemParametersInfoW(SPI_GETWORKAREA, 0, &workArea, 0);

   const int workWidth = workArea.right - workArea.left;
   const int workHeight = workArea.bottom - workArea.top;
   SetUiScale(ComputeUiScale(GetDpiForSystem(), workArea));
   const int desiredWidth = ScalePx(3200);
   const int desiredHeight = ScalePx(1800);
   const int windowWidth = min(desiredWidth, max(ScalePx(1200), workWidth - ScalePx(80)));
   const int windowHeight = min(desiredHeight, max(ScalePx(780), workHeight - ScalePx(80)));
   const int windowX = workArea.left + max(0, (workWidth - windowWidth) / 2);
   const int windowY = workArea.top + max(0, (workHeight - windowHeight) / 2);

   HWND hWnd = CreateWindowW(szWindowClass, szTitle, WS_OVERLAPPEDWINDOW,
      windowX, windowY, windowWidth, windowHeight, nullptr, nullptr, hInstance, nullptr);

   if (!hWnd)
   {
      return FALSE;
   }

   g_hMainWnd = hWnd;
   AccessibleAnnounce::Init(hWnd);

   // Mirror every command button into a categorised Operations menu, so all
   // options are reachable by keyboard (Alt, arrows, first-letter jump, Enter)
   // without tabbing through the whole grid.
   BuildOperationsMenu(hWnd);

   // Reflect the active theme in the View > Theme radio group.
   if (HMENU bar = GetMenu(hWnd))
   {
       CheckMenuRadioItem(bar, IDM_THEME_GRAPHITE, IDM_THEME_APPLELIGHT,
                          IDM_THEME_GRAPHITE + static_cast<int>(CurrentThemeId()),
                          MF_BYCOMMAND);
   }

   ShowWindow(hWnd, nCmdShow);
   UpdateWindow(hWnd);

   BindGuiSinksToMainWindow(hWnd);

   return TRUE;
}

// Apply a theme chosen from the View > Theme menu: switch + persist, update the
// radio check, and note it in the log. Existing explicitly-coloured output
// keeps its colours; heuristic-coloured lines and new output re-theme on the
// repaint SetActiveTheme triggers.
static void ApplyThemeFromMenu(HWND hWnd, ThemeId id)
{
    if (id == CurrentThemeId()) return;
    ApplyThemeAndPersist(id);
    if (HMENU bar = GetMenu(hWnd))
    {
        CheckMenuRadioItem(bar, IDM_THEME_GRAPHITE, IDM_THEME_APPLELIGHT,
                           IDM_THEME_GRAPHITE + static_cast<int>(id), MF_BYCOMMAND);
    }
    WCHAR line[192];
    wsprintfW(line, L"\r\n>>> Theme: %s\r\n", ThemeName(id));
    AppendInfoText(hInfoEdit, line);
}

//
//  FUNCTION: WndProc(HWND, UINT, WPARAM, LPARAM)
//
//  PURPOSE: Processes messages for the main window.
//
//  WM_COMMAND  - process the application menu
//  WM_PAINT    - Paint the main window
//  WM_DESTROY  - post a quit message and return
//
//
LRESULT CALLBACK WndProc(HWND hWnd, UINT message, WPARAM wParam, LPARAM lParam)
{
    switch (message)
    {
    case WM_CREATE:
        UpdateUiScale(hWnd, GetDpiForWindow(hWnd));
        CreateMainControls(hWnd);
        break;
    case WM_GETOBJECT:
        {
            // Provide the UIA root provider (for screen-reader notification
            // events). Any other object id falls through to the default
            // MSAA/UIA handling so the standard controls stay accessible.
            bool handled = false;
            LRESULT result = AccessibleAnnounce::HandleGetObject(hWnd, wParam, lParam, handled);
            if (handled) return result;
            return DefWindowProc(hWnd, message, wParam, lParam);
        }
    case WM_DPICHANGED:
        {
            RECT* suggested = (RECT*)lParam;
            if (suggested)
            {
                SetWindowPos(hWnd, nullptr,
                    suggested->left,
                    suggested->top,
                    suggested->right - suggested->left,
                    suggested->bottom - suggested->top,
                    SWP_NOZORDER | SWP_NOACTIVATE);
            }

            const UINT dpi = HIWORD(wParam) ? HIWORD(wParam) : LOWORD(wParam);
            if (UpdateUiScale(hWnd, dpi))
            {
                CreateUiFonts();
                ApplyUiFonts();
                ApplyUiVisualTone();
            }
            LayoutMainControls(hWnd);
            InvalidateRect(hWnd, nullptr, TRUE);
        }
        return 0;
    case WM_ERASEBKGND:
        return 1;
    case WM_LBUTTONDOWN:
        if (HandleSidebarClick(hWnd, (short)LOWORD(lParam), (short)HIWORD(lParam)))
            return 0;
        break;
    case WM_SIZE:
        LayoutMainControls(hWnd);
        InvalidateRect(hWnd, nullptr, TRUE);
        break;
    case WM_WINDOWPOSCHANGED:
        {
            LRESULT result = DefWindowProc(hWnd, message, wParam, lParam);
            HMONITOR monitor = GetNearestMonitor(hWnd);
            if (monitor && monitor != gUiMonitor)
            {
                if (UpdateUiScale(hWnd, GetDpiForWindow(hWnd)))
                {
                    CreateUiFonts();
                    ApplyUiFonts();
                    ApplyUiVisualTone();
                }
                LayoutMainControls(hWnd);
                InvalidateRect(hWnd, nullptr, TRUE);
            }
            return result;
        }
    case WM_CTLCOLORSTATIC:
        return HandleControlColorStatic(hWnd, (HDC)wParam, (HWND)lParam);
    case WM_CTLCOLOREDIT:
        return HandleControlColorEdit((HDC)wParam);
    case WM_DRAWITEM:
        DrawCommandButton((const DRAWITEMSTRUCT*)lParam);
        return TRUE;
    case WM_COMMAND:
        {
            int wmId = LOWORD(wParam);
            // Parse the menu selections:
            switch (wmId)
            {
            case IDC_INFO_BUTTON1:
            case IDC_INFO_BUTTON2:
            case IDC_INFO_BUTTON3:
            case IDC_INFO_BUTTON4:
            case IDC_INFO_BUTTON5:
            case IDC_INFO_BUTTON6:
            case IDC_INFO_BUTTON7:
            case IDC_INFO_BUTTON8:
            case IDC_INFO_BUTTON9:
            case IDC_INFO_BUTTON10:
            case IDC_INFO_BUTTON11:
            case IDC_INFO_BUTTON12:
            case IDC_INFO_BUTTON13:
            case IDC_INFO_BUTTON14:
            case IDC_INFO_BUTTON15:
            case IDC_INFO_BUTTON16:
            case IDC_INFO_BUTTON17:
            case IDC_INFO_BUTTON18:
            case IDC_INFO_BUTTON19:
            case IDC_INFO_BUTTON20:
            case IDC_INFO_BUTTON21:
            case IDC_INFO_BUTTON22:
            case IDC_INFO_BUTTON23:
            case IDC_INFO_BUTTON24:
            case IDC_INFO_BUTTON25:
            case IDC_INFO_BUTTON26:
            case IDC_INFO_BUTTON27:
            case IDC_INFO_BUTTON28:
            case IDC_INFO_BUTTON29:
            case IDC_INFO_BUTTON30:
            case IDC_INFO_BUTTON31:
            case IDC_INFO_BUTTON32:
            case IDC_INFO_BUTTON33:
            case IDC_INFO_BUTTON34:
            case IDC_INFO_BUTTON35:
                if (HIWORD(wParam) == BN_CLICKED)
                {
                    // The screen reader already announces the button press, so
                    // the synthesized click is redundant noise in that mode.
                    if (!IsAccessibleMode()) UiSound::PlayMenuClickSound();
                    const int commandIndex = wmId - IDC_INFO_BUTTON1;
                    if (commandIndex == kClearButtonIndex)
                    {
                        // While a workflow is running, the Clear button acts
                        // as a Cancel; otherwise it clears the output panel.
                        if (GuiWorker::IsRunning())
                        {
                            GuiWorker::RequestCancel();
                            AppendInfoText(hInfoEdit,
                L"(Cancellation requested - will stop at next checkpoint.)\r\n");
                        }
                        else
                        {
                            GuiSink::ClearOutput();
                        }
                    }
                    else if (commandIndex == kExitButtonIndex)
                    {
                        Sleep(90); // let the click sound finish before teardown
                        RequestCleanShutdown(hWnd);
                    }
                    else if (commandIndex == kBatchButtonIndex)
                    {
                        // Batch run: prompt for a list of menu numbers, then
                        // run EnsureDriveOpen + one Prescan, then dispatch each
                        // chosen op in turn without re-prescanning.
                        if (GuiWorker::IsRunning())
                        {
                            AppendInfoText(hInfoEdit,
                                L"(Another operation is already running. Click "
                                L"'Clear info box' to request cancellation.)\r\n");
                        }
                        else
                        {
                            WCHAR line[256];
                            wsprintfW(line, L"\r\n>>> %s\r\n",
                                     CommandLabels[commandIndex]);
                            AppendInfoText(hInfoEdit, line);
                            AccessibleAnnounce::Announce(L"Batch run started.");

                            HWND hWndCopy = hWnd;
                            SetMenuButtonsEnabled(false);
                            bool started = GuiWorker::RunAsync(
                                [hWndCopy]() {
                                    struct PostDone {
                                        HWND h;
                                        ~PostDone() {
                                            PostMessageW(h, WM_APP_WORKER_DONE, 0, 0);
                                        }
                                    } guard{ hWndCopy };

                                    bool ok = false;
                                    std::string raw = GuiInput::PromptString(
                                        "Batch run",
                                        "Enter menu numbers to run, separated by spaces or commas\n"
                                        "(valid range: 1-30; duplicates ignored; example: 6 7 8 9)",
                                        std::string(), &ok);
                                    if (!ok) {
                                        Console::Info("Batch cancelled.\n");
                                        return;
                                    }
                                    std::vector<int> choices = ParseBatchChoices(raw);
                                    if (choices.empty()) {
                                        Console::Error("No valid menu numbers in input. Batch cancelled.\n");
                                        return;
                                    }

                                    {
                                        std::string summary = "Batch will run, in order: ";
                                        for (size_t i = 0; i < choices.size(); i++) {
                                            if (i > 0) summary += ", ";
                                            summary += std::to_string(choices[i]);
                                        }
                                        summary += "\n";
                                        Console::Info(summary.c_str());
                                    }

                                    // Audio disc required if ANY chosen op needs it. Write Disc
                                    // (choice 3) is the only one that doesn't.
                                    bool needsAudio = false;
                                    bool needsDrive = false;
                                    for (int c : choices) {
                                        if (ButtonNeedsAudioDisc(c - 1)) needsAudio = true;
                                        if (ButtonNeedsDrive(c - 1)) needsDrive = true;
                                    }
                                    bool freshlyScanned = false;
                                    if (needsDrive) {
                                        if (!EnsureDriveOpen(hWndCopy, &freshlyScanned, needsAudio)) {
                                            return;
                                        }
                                        // Single shared prescan up front — workflows in the loop
                                        // see the same TOC/pregap/CD-Text data.
                                        if (!freshlyScanned) {
                                            Console::Info("Running shared pre-scan for all batched ops...\n");
                                            Prescan();
                                        }
                                    }

                                    for (size_t i = 0; i < choices.size(); i++) {
                                        if (g_interrupt.IsInterrupted()) {
                                            Console::Warning("Batch cancelled by user.\n");
                                            break;
                                        }
                                        int choice = choices[i];
                                        WCHAR header[160];
                                        wsprintfW(header,
                                            L"\r\n=== Batch step %u/%u: option %d ===\r\n",
                                            static_cast<unsigned>(i + 1),
                                            static_cast<unsigned>(choices.size()),
                                            choice);
                                        AppendInfoText(hInfoEdit, header);
                                        // `choice` is the displayed button number; map it to
                                        // the stable op id the dispatcher expects.
                                        const int batchOpId = ButtonToMenuChoice(choice - 1);
                                        // Copy disc (1) and Rip tracks (2) force a full
                                        // close/reopen refresh so a disc swapped in mid-batch is
                                        // picked up — matching their single-click behaviour. The
                                        // shared up-front Prescan only re-reads the existing
                                        // handle, which can report a stale TOC after a swap. Skip
                                        // only on the very first step when the drive was just
                                        // opened with a fresh TOC (nothing could have changed yet).
                                        if (batchOpId == 1 || batchOpId == 2) {
                                            if (!(i == 0 && freshlyScanned)) {
                                                RefreshDisc();
                                                if (g_interrupt.IsInterrupted()) {
                                                    Console::Warning("Batch cancelled by user.\n");
                                                    break;
                                                }
                                            }
                                        }
                                        DispatchMenuChoice(g_copier, g_disc, g_workDir,
                                                           g_audioDrive, g_hasTOC,
                                                           batchOpId);
                                    }
                                    Console::Success("\nBatch complete.\n");
                                });
                            if (!started) {
                                SetMenuButtonsEnabled(true);
                            }
                        }
                    }
                    else
                    {
                        if (GuiWorker::IsRunning())
                        {
                            AppendInfoText(hInfoEdit,
                                L"(Another operation is already running. Click "
                                L"'Clear info box' to request cancellation.)\r\n");
                        }
                        else
                        {
                            WCHAR line[256];
                            wsprintfW(line, L"\r\n>>> %s\r\n",
                                     CommandLabels[commandIndex]);
                            AppendInfoText(hInfoEdit, line);
                            AccessibleAnnounce::Announce(
                                std::wstring(L"Started: ") + CommandLabels[commandIndex]);

                            HWND hWndCopy = hWnd;
                            int choice = ButtonToMenuChoice(commandIndex);
                            bool needsPrescan = ButtonNeedsPrescan(commandIndex);
                            bool needsAudioDisc = ButtonNeedsAudioDisc(commandIndex);
                            bool needsDrive = ButtonNeedsDrive(commandIndex);
                            bool disablesMenu = ButtonDisablesMenu(commandIndex);
                            // Disable every menu button except Cancel for the
                            // duration of the workflow. Re-enabled in WM_APP_WORKER_DONE.
                            if (disablesMenu) SetMenuButtonsEnabled(false);
                            // Drive open + TOC read run on the worker so the UI stays
                            // responsive even if WaitForDisc polls for the full timeout.
                            bool started = GuiWorker::RunAsync(
                                [hWndCopy, choice, needsPrescan, needsAudioDisc, needsDrive]() {
                                    // RAII guard: WORKER_DONE is posted on every exit
                                    // path, including exceptions thrown by the workflow.
                                    // Without this, an unhandled throw would skip the
                                    // notification and leave the UI buttons disabled.
                                    struct PostDone {
                                        HWND h;
                                        ~PostDone() {
                                            PostMessageW(h, WM_APP_WORKER_DONE, 0, 0);
                                        }
                                    } guard{ hWndCopy };

                                    if (!needsDrive) {
                                        // Help / Check-updates: no drive interaction.
                                        DispatchMenuChoice(g_copier, g_disc, g_workDir,
                                                           g_audioDrive, g_hasTOC, choice);
                                        return;
                                    }

                                    // Rescan disc (op id 25) already does a full drive
                                    // scan, drive selection, open, TOC read and
                                    // AccurateRip lookup — exactly what EnsureDriveOpen
                                    // does on a first open. Routing it through
                                    // EnsureDriveOpen first scanned/selected/opened the
                                    // drive and hit AccurateRip a second time, so the
                                    // disc appeared to rescan twice. Let the rescan be
                                    // the sole open, then mirror EnsureDriveOpen's
                                    // post-open state (g_driveOpen/g_workDir) so later
                                    // ops don't trigger another open-time scan — the
                                    // rescan can't touch these GUI-local globals itself.
                                    if (choice == 25) {
                                        DispatchMenuChoice(g_copier, g_disc, g_workDir,
                                                           g_audioDrive, g_hasTOC, choice);
                                        if (g_audioDrive) {
                                            g_driveOpen = true;
                                            if (g_workDir.empty()) g_workDir = GetWorkingDirectory();
                                        }
                                        return;
                                    }

                                    bool freshlyScanned = false;
                                    // Erase CD-RW (op id 31) needs no disc contents, so skip the
                                    // open-time TOC + pre-gap scan — the disc is about to be wiped.
                                    const bool readTocOnOpen = (choice != 31);
                                    if (EnsureDriveOpen(hWndCopy, &freshlyScanned, needsAudioDisc, readTocOnOpen)) {
                                        // Bail if cancellation was requested (Cancel button or
                                        // window close) during the drive-open + TOC/pregap scan,
                                        // rather than pressing on into the prescan and workflow.
                                        if (g_interrupt.IsInterrupted()) return;
                                        // For asterisked operations, re-run the TOC + pregap
                                        // probe so the workflow sees fresh data — unless
                                        // EnsureDriveOpen just did it.
                                        //
                                        // Every asterisked op that reads from the source disc
                                        // (Copy disc, Rip tracks, Recovery rip, and all the
                                        // quality scans / disc-info probes) re-offers the drive
                                        // selector — when more than one drive holds an audio CD —
                                        // and then does a full close/reopen refresh, so a disc in
                                        // a *different* drive than the one opened first is picked
                                        // up. The lighter Prescan() can't see past a stale handle
                                        // or switch drives.
                                        //
                                        // Write disc (3) and Write tracks (4) are the exception:
                                        // they read the source TOC on the current handle and then
                                        // run their own SelectWriterDrive picker internally, so
                                        // they keep the same-handle Prescan().
                                        if (needsPrescan && !freshlyScanned) {
                                            if (choice == 3 || choice == 4) {
                                                Prescan();
                                            }
                                            else {
                                                // Decide the source drive, then refresh the disc
                                                // on it back-to-back so g_audioDrive and the open
                                                // handle never diverge. RefreshDisc has its own
                                                // interrupt checks during the TOC retry loop.
                                                ReselectSourceDriveIfMultiple();
                                                RefreshDisc();
                                            }
                                            if (g_interrupt.IsInterrupted()) return;
                                        }
                                        DispatchMenuChoice(g_copier, g_disc, g_workDir,
                                                           g_audioDrive, g_hasTOC, choice);
                                    }
                                });
                            if (!started) {
                                // Couldn't claim the worker (shouldn't happen on the
                                // UI thread, but recover so we don't strand the UI).
                                SetMenuButtonsEnabled(true);
                            }
                        }
                    }
                }
                break;
            case IDM_TOGGLE_ACCESSIBLE:
                ApplyAccessibleMode(hWnd, !IsAccessibleMode(), /*focusEdit=*/true);
                break;
            case IDM_THEME_GRAPHITE:
                ApplyThemeFromMenu(hWnd, ThemeId::Graphite);
                break;
            case IDM_THEME_CATPPUCCIN:
                ApplyThemeFromMenu(hWnd, ThemeId::CatppuccinFrappe);
                break;
            case IDM_THEME_NORD:
                ApplyThemeFromMenu(hWnd, ThemeId::Nord);
                break;
            case IDM_THEME_ARCDARK:
                ApplyThemeFromMenu(hWnd, ThemeId::ArcDark);
                break;
            case IDM_THEME_APPLELIGHT:
                ApplyThemeFromMenu(hWnd, ThemeId::AppleLight);
                break;
            case IDM_ABOUT:
                DialogBox(hInst, MAKEINTRESOURCE(IDD_ABOUTBOX), hWnd, About);
                break;
            case IDM_EXIT:
                RequestCleanShutdown(hWnd);
                break;
            default:
                return DefWindowProc(hWnd, message, wParam, lParam);
            }
        }
        break;
    case WM_PAINT:
        {
            PAINTSTRUCT ps;
            HDC hdc = BeginPaint(hWnd, &ps);

            // Double-buffer: GDI+ background paint is expensive (antialias,
            // bicubic, ClearType, gradient brushes, DrawTechAccents loops).
            // Rendering straight to the screen DC produces a visible
            // top-to-bottom sweep on each repaint. Paint into a memory DC
            // sized to the dirty rect and BitBlt once.
            RECT clientRc;
            GetClientRect(hWnd, &clientRc);
            const int bufWidth  = max(1, clientRc.right - clientRc.left);
            const int bufHeight = max(1, clientRc.bottom - clientRc.top);

            HDC memDC = CreateCompatibleDC(hdc);
            HBITMAP memBmp = CreateCompatibleBitmap(hdc, bufWidth, bufHeight);
            if (memDC && memBmp)
            {
                HBITMAP oldBmp = (HBITMAP)SelectObject(memDC, memBmp);
                DrawMainBackground(hWnd, memDC);
                BitBlt(hdc, 0, 0, bufWidth, bufHeight, memDC, 0, 0, SRCCOPY);
                SelectObject(memDC, oldBmp);
            }
            else
            {
                DrawMainBackground(hWnd, hdc);
            }

            if (memBmp) DeleteObject(memBmp);
            if (memDC)  DeleteDC(memDC);

            EndPaint(hWnd, &ps);
        }
        break;
    case WM_APP_DRAIN_OUTPUT:
        GuiSink::DrainOutputQueue();
        return 0;
    case WM_APP_WORKER_DONE:
        if (g_isClosing) return 0;
        // Make sure any final output is visible before joining the worker.
        GuiSink::DrainOutputQueue();
        GuiWorker::ReapIfDone();
        SetMenuButtonsEnabled(true);
        AccessibleAnnounce::Announce(L"Operation finished.");
        return 0;
    case WM_CLOSE:
        RequestCleanShutdown(hWnd);
        return 0;
    case WM_DESTROY:
    {
        g_isClosing = true;
        // Invalidate cross-thread references FIRST so the worker's final output
        // calls become no-ops and can't block on (or touch) the UI thread while
        // we wait for it just below.
        GuiInput::SetOwnerWindow(nullptr);
        GuiSink::SetDrainTarget(nullptr, 0);
        GuiSink::SetProgressWindows(nullptr, nullptr);
        GuiSink::SetOutputWindow(nullptr);

        // Then stop the worker and WAIT for it to actually exit before this
        // returns. The worker reads/writes g_disc and g_copier — static globals
        // destroyed during CRT teardown after wWinMain returns — plus GuiSink /
        // InterruptHandler statics. The old path DETACHED it, so a prescan still
        // in flight kept touching g_disc.tracks as static destruction freed it:
        // a write-access violation on a freed vector. Joining makes teardown
        // ordered; the worker honours the cancel flag at its next checkpoint.
        constexpr int SHUTDOWN_JOIN_TIMEOUT_MS = 5000;
        if (GuiWorker::IsRunning()) {
            GuiWorker::RequestCancel();
            if (!GuiWorker::WaitAndJoin(SHUTDOWN_JOIN_TIMEOUT_MS)) {
                // Worker won't stop in time (e.g. a wedged SCSI call or an open
                // modal prompt). Force-exit now: skipping static destruction
                // means the still-running worker can't fault on freed globals,
                // and the OS reclaims everything. Safer than detaching into a
                // use-after-free.
                TerminateProcess(GetCurrentProcess(), 0);
            }
        }
        else {
            GuiWorker::ReapIfDone();   // join an already-finished worker
        }

        AccessibleAnnounce::Shutdown();
        PostQuitMessage(0);
        break;
    }
    default:
        return DefWindowProc(hWnd, message, wParam, lParam);
    }
    return 0;
}

static void RequestCleanShutdown(HWND hWnd) {
    if (g_isClosing) return;
    g_isClosing = true;

    if (GuiWorker::IsRunning()) {
        GuiWorker::RequestCancel();
    }

    DestroyWindow(hWnd);
}

// Message handler for about box.
INT_PTR CALLBACK About(HWND hDlg, UINT message, WPARAM wParam, LPARAM lParam)
{
    UNREFERENCED_PARAMETER(lParam);
    switch (message)
    {
    case WM_INITDIALOG:
        return (INT_PTR)TRUE;

    case WM_COMMAND:
        if (LOWORD(wParam) == IDOK || LOWORD(wParam) == IDCANCEL)
        {
            EndDialog(hDlg, LOWORD(wParam));
            return (INT_PTR)TRUE;
        }
        break;
    }
    return (INT_PTR)FALSE;
}
