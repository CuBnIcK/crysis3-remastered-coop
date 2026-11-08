// c3coop_launch.exe: Steam launch option wrapper ("<path>\c3coop_launch.exe" %command%) that starts the game and
// loads c3coop.dll from the launcher's folder.
//
// Steam replaces %command% with the game's own command line ("...\Bin64\Crysis3Remastered.exe" <args>), including
// "+connect_lobby <id>" or our rich presence connect string ("+c3coop_join <host> <lobby>") when the game is
// started by an invite or "Join Game". The rest of the launcher's command line is passed on unchanged.
//
// - starts the game with CreateProcessW (current directory = the exe's folder, environment inherited; SteamAppId /
//   SteamGameId are set to 2096610 only if missing, like scripts\c3ctl.py for a directly started exe)
// - waits until the process has its main module and kernel32 loaded, then 1 s more (scripts\c3ctl.py injects 1 s
//   after the process appears), and loads the DLL with CreateRemoteThread(LoadLibraryW)
// - the DLL and its pak are copied to <launcher dir>\inject\c3coop_<pid>_<tick>.dll/.pak first (like c3ctl.py), so
//   the originals stay replaceable while the game runs; dllmain.cpp then writes its logs to <launcher dir>\logs and
//   crash dumps to <launcher dir>\crash (it uses the parent of the DLL's folder). If the copy fails, the original
//   DLL is injected. Old copies are deleted on the next start (loaded ones are skipped).
// - no c3coop.dll next to the launcher, or a different exe: the game is started without the mod
// - stays alive until the game exits and returns its exit code, so Steam keeps seeing the game as running
// Log: c3coop_launch.log next to the launcher.

#include <windows.h>
#include <psapi.h>
#include <shellapi.h>
#include <share.h>

#include <cstdarg>
#include <cstdio>
#include <string>

#pragma comment(lib, "psapi.lib")
#pragma comment(lib, "shell32.lib")
#pragma comment(lib, "user32.lib")

namespace {

constexpr wchar_t kGameExe[] = L"Crysis3Remastered.exe";
constexpr wchar_t kAppId[] = L"2096610";

std::wstring g_dir; // launcher folder, with trailing backslash
FILE* g_log;

void Log(const wchar_t* fmt, ...) {
    if (!g_log)
        return;
    SYSTEMTIME t;
    GetLocalTime(&t);
    fwprintf(g_log, L"%04u-%02u-%02u %02u:%02u:%02u.%03u ", t.wYear, t.wMonth, t.wDay, t.wHour, t.wMinute, t.wSecond,
             t.wMilliseconds);
    va_list ap;
    va_start(ap, fmt);
    vfwprintf(g_log, fmt, ap);
    va_end(ap);
    fputwc(L'\n', g_log);
    fflush(g_log);
}

void OpenLog() {
    std::wstring path = g_dir + L"c3coop_launch.log";
    WIN32_FILE_ATTRIBUTE_DATA attr;
    bool big = GetFileAttributesExW(path.c_str(), GetFileExInfoStandard, &attr) &&
               (attr.nFileSizeHigh || attr.nFileSizeLow > 1024 * 1024);
    g_log = _wfsopen(path.c_str(), big ? L"w, ccs=UTF-8" : L"a, ccs=UTF-8", _SH_DENYNO);
}

// Rest of the command line after argv[0], exactly as written (keeps the game's own quoting).
const wchar_t* SkipFirstArg(const wchar_t* p) {
    while (*p == L' ' || *p == L'\t')
        ++p;
    if (*p == L'"') {
        ++p;
        while (*p && *p != L'"')
            ++p;
        if (*p)
            ++p;
    } else {
        while (*p && *p != L' ' && *p != L'\t')
            ++p;
    }
    while (*p == L' ' || *p == L'\t')
        ++p;
    return p;
}

bool EndsWithNoCase(const std::wstring& s, const wchar_t* suffix) {
    size_t n = wcslen(suffix);
    return s.size() >= n && _wcsicmp(s.c_str() + s.size() - n, suffix) == 0;
}

bool FileExists(const std::wstring& path) {
    DWORD a = GetFileAttributesW(path.c_str());
    return a != INVALID_FILE_ATTRIBUTES && !(a & FILE_ATTRIBUTE_DIRECTORY);
}

// Copies c3coop.dll (+ c3coop.pak) to inject\c3coop_<pid>_<tick>.*; returns the DLL path to inject.
std::wstring PrepareDll() {
    std::wstring dll = g_dir + L"c3coop.dll";
    std::wstring pak = g_dir + L"c3coop.pak";
    if (!FileExists(dll))
        return {};
    if (!FileExists(pak))
        Log(L"warning: %s is missing, the game scripts will not be patched", pak.c_str());
    std::wstring injectDir = g_dir + L"inject\\";
    CreateDirectoryW(injectDir.c_str(), nullptr);
    WIN32_FIND_DATAW fd;
    HANDLE find = FindFirstFileW((injectDir + L"c3coop_*").c_str(), &fd);
    if (find != INVALID_HANDLE_VALUE) {
        do {
            DeleteFileW((injectDir + fd.cFileName).c_str()); // fails for copies a running game has loaded
        } while (FindNextFileW(find, &fd));
        FindClose(find);
    }
    wchar_t name[64];
    swprintf_s(name, L"c3coop_%lu_%llu", GetCurrentProcessId(), GetTickCount64());
    std::wstring copy = injectDir + name + L".dll";
    if (!CopyFileW(dll.c_str(), copy.c_str(), FALSE)) {
        Log(L"cannot copy the DLL to %s (error %lu), injecting the original", copy.c_str(), GetLastError());
        return dll;
    }
    if (FileExists(pak) && !CopyFileW(pak.c_str(), (injectDir + name + L".pak").c_str(), FALSE))
        Log(L"cannot copy %s (error %lu)", pak.c_str(), GetLastError());
    return copy;
}

// Main module and kernel32 present in the target (the PEB loader list is readable).
bool ModulesReady(HANDLE process) {
    HMODULE mods[64];
    DWORD needed = 0;
    if (!EnumProcessModulesEx(process, mods, sizeof(mods), &needed, LIST_MODULES_64BIT))
        return false;
    DWORD count = needed / sizeof(HMODULE);
    if (count > 64)
        count = 64;
    for (DWORD i = 1; i < count; ++i) {
        wchar_t base[MAX_PATH];
        if (GetModuleBaseNameW(process, mods[i], base, MAX_PATH) && _wcsicmp(base, L"kernel32.dll") == 0)
            return true;
    }
    return false;
}

bool Inject(HANDLE process, const std::wstring& dll) {
    size_t size = (dll.size() + 1) * sizeof(wchar_t);
    void* remote = VirtualAllocEx(process, nullptr, size, MEM_COMMIT | MEM_RESERVE, PAGE_READWRITE);
    if (!remote) {
        Log(L"VirtualAllocEx failed (error %lu)", GetLastError());
        return false;
    }
    bool ok = false;
    if (WriteProcessMemory(process, remote, dll.c_str(), size, nullptr)) {
        // kernel32 is mapped at the same address in every process of a boot session
        auto load = reinterpret_cast<LPTHREAD_START_ROUTINE>(
            GetProcAddress(GetModuleHandleW(L"kernel32.dll"), "LoadLibraryW"));
        HANDLE thread = CreateRemoteThread(process, nullptr, 0, load, remote, 0, nullptr);
        if (thread) {
            DWORD wait = WaitForSingleObject(thread, 30000);
            DWORD code = 0;
            GetExitCodeThread(thread, &code);
            CloseHandle(thread);
            ok = wait == WAIT_OBJECT_0 && code != 0;
            Log(L"LoadLibraryW in the game: %s (module handle low 32 bits %08lX)",
                wait != WAIT_OBJECT_0 ? L"timed out" : ok ? L"ok" : L"FAILED", code);
            if (wait != WAIT_OBJECT_0)
                return false; // the thread may still read the path: keep the memory
        } else {
            Log(L"CreateRemoteThread failed (error %lu)", GetLastError());
        }
    } else {
        Log(L"WriteProcessMemory failed (error %lu)", GetLastError());
    }
    VirtualFreeEx(process, remote, 0, MEM_RELEASE);
    return ok;
}

} // namespace

int WINAPI wWinMain(HINSTANCE, HINSTANCE, PWSTR, int) {
    wchar_t self[MAX_PATH];
    GetModuleFileNameW(nullptr, self, MAX_PATH);
    g_dir = self;
    g_dir.resize(g_dir.find_last_of(L'\\') + 1);
    OpenLog();
    Log(L"---- %s", GetCommandLineW());

    int argc = 0;
    LPWSTR* argv = CommandLineToArgvW(GetCommandLineW(), &argc);
    if (!argv || argc < 2) {
        Log(L"no game command line: set the Steam launch option to \"%sc3coop_launch.exe\" %%command%%", g_dir.c_str());
        std::wstring text = L"c3coop_launch.exe is a Steam launch option wrapper.\n\nCrysis 3 Remastered > Properties > "
                            L"Launch options:\n\n\"" + g_dir + L"c3coop_launch.exe\" %command%";
        MessageBoxW(nullptr, text.c_str(), L"c3coop", MB_ICONINFORMATION);
        return 1;
    }
    wchar_t exe[MAX_PATH];
    if (!GetFullPathNameW(argv[1], MAX_PATH, exe, nullptr))
        wcscpy_s(exe, argv[1]);
    LocalFree(argv);
    std::wstring exePath = exe;
    std::wstring workDir = exePath.substr(0, exePath.find_last_of(L'\\') + 1);
    std::wstring cmdLine = SkipFirstArg(GetCommandLineW()); // "<exe>" <args...>, as Steam wrote it

    bool isGame = EndsWithNoCase(exePath, kGameExe);
    std::wstring dll = isGame ? PrepareDll() : std::wstring();
    if (!isGame)
        Log(L"%s is not %s, starting it without the mod", exePath.c_str(), kGameExe);
    else if (dll.empty())
        Log(L"%sc3coop.dll not found, starting the game without the mod", g_dir.c_str());

    wchar_t probe[8];
    if (!GetEnvironmentVariableW(L"SteamAppId", probe, 8)) {
        SetEnvironmentVariableW(L"SteamAppId", kAppId);
        SetEnvironmentVariableW(L"SteamGameId", kAppId);
        Log(L"SteamAppId was not set (not started by Steam?), set to %s", kAppId);
    }

    STARTUPINFOW si{};
    si.cb = sizeof(si);
    PROCESS_INFORMATION pi{};
    // -devmode: on this install the plain start dies during startup (exit-path AV 0x38E94FAA, also without the mod),
    // with -devmode it runs; it also turns the single-instance block into a prompt. C3COOP_NODEVMODE=1 skips it.
    if (isGame && !GetEnvironmentVariableW(L"C3COOP_NODEVMODE", probe, 8) && cmdLine.find(L"-devmode") == std::wstring::npos) {
        cmdLine += L" -devmode";
        Log(L"added -devmode");
    }
    // g_skipIntro (game cvar "Skip all the intro videos"): no Crytek/CryEngine logo movies. C3COOP_INTRO=1 keeps them.
    if (isGame && !GetEnvironmentVariableW(L"C3COOP_INTRO", probe, 8) && cmdLine.find(L"g_skipIntro") == std::wstring::npos) {
        cmdLine += L" +g_skipIntro 1";
        Log(L"added +g_skipIntro 1");
    }
    std::wstring mutableCmd = cmdLine;
    if (!CreateProcessW(exePath.c_str(), mutableCmd.data(), nullptr, nullptr, FALSE, 0, nullptr,
                        workDir.empty() ? nullptr : workDir.c_str(), &si, &pi)) {
        DWORD err = GetLastError();
        Log(L"CreateProcessW(%s) failed (error %lu)", exePath.c_str(), err);
        std::wstring text = L"Cannot start\n" + exePath + L"\nerror " + std::to_wstring(err);
        MessageBoxW(nullptr, text.c_str(), L"c3coop", MB_ICONERROR);
        return 1;
    }
    Log(L"started pid %lu: %s (cwd %s)", pi.dwProcessId, cmdLine.c_str(), workDir.c_str());
    CloseHandle(pi.hThread);

    if (!dll.empty()) {
        bool ready = false;
        for (int i = 0; i < 200 && !ready; ++i) { // up to 10 s
            if (WaitForSingleObject(pi.hProcess, 50) == WAIT_OBJECT_0)
                break;
            ready = ModulesReady(pi.hProcess);
        }
        if (!ready) {
            Log(L"the game's modules never became readable (or it exited), not injecting");
        } else if (WaitForSingleObject(pi.hProcess, 1000) == WAIT_OBJECT_0) {
            Log(L"the game exited within 1 s, not injecting");
        } else {
            Log(L"injecting %s", dll.c_str());
            Inject(pi.hProcess, dll);
        }
    }

    WaitForSingleObject(pi.hProcess, INFINITE);
    DWORD code = 0;
    GetExitCodeProcess(pi.hProcess, &code);
    CloseHandle(pi.hProcess);
    Log(L"game exited with code %lu (0x%08lX)", code, code);
    if (g_log)
        fclose(g_log);
    return static_cast<int>(code);
}
