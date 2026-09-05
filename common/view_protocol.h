// Copyright 2026, DisplayXR
// SPDX-License-Identifier: BSL-1.0
/*!
 * @file
 * @brief  The `displayxr-view:` OS protocol handler side of the launch
 *         contract (Win32, header-only): per-user registration, sibling-viewer
 *         forwarding, and single-instance hand-off.
 *
 * ## Why the viewer registers itself (not the installer)
 *
 * The NSIS installers run elevated, so an `HKCU` write from inside them lands
 * in the ELEVATING admin's hive, which is not necessarily the logged-in user.
 * A per-user protocol association belongs in the user's own hive, so the
 * viewer writes it on launch — idempotent, no admin, self-healing after the
 * sibling that previously owned the scheme is uninstalled.
 *
 * ## One scheme, several viewers
 *
 * Every viewer registers the SAME scheme, last writer wins. A viewer that
 * receives a URL whose `type=` is not its own looks the sibling up by its
 * install breadcrumb (`HKLM\Software\DisplayXR\Demos\<Key>\InstallPath`) and
 * re-launches it with the identical URL via `CreateProcessW` (argv, never a
 * shell string), then exits. One scheme means the browser asks the user ONCE.
 */

#pragma once

#if defined(_WIN32)

#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <windows.h>

#include <string>
#include <string_view>

namespace dxr {

constexpr const wchar_t* kViewProtocolScheme = L"displayxr-view";

namespace view_protocol_detail {

inline std::wstring
ReadRegSz(HKEY root, const std::wstring& subkey, const wchar_t* value)
{
    HKEY k = nullptr;
    if (RegOpenKeyExW(root, subkey.c_str(), 0, KEY_READ | KEY_WOW64_64KEY, &k) != ERROR_SUCCESS)
        return {};
    wchar_t buf[1024] = {};
    DWORD cb = sizeof(buf), type = 0;
    std::wstring out;
    if (RegQueryValueExW(k, value, nullptr, &type, reinterpret_cast<LPBYTE>(buf), &cb) == ERROR_SUCCESS &&
        (type == REG_SZ || type == REG_EXPAND_SZ)) {
        out.assign(buf, wcsnlen(buf, cb / sizeof(wchar_t)));
    }
    RegCloseKey(k);
    return out;
}

inline bool
WriteRegSz(HKEY root, const std::wstring& subkey, const wchar_t* value, const std::wstring& data)
{
    HKEY k = nullptr;
    if (RegCreateKeyExW(root, subkey.c_str(), 0, nullptr, 0, KEY_WRITE, nullptr, &k, nullptr) !=
        ERROR_SUCCESS)
        return false;
    const LSTATUS st = RegSetValueExW(k, value, 0, REG_SZ, reinterpret_cast<const BYTE*>(data.c_str()),
                                      static_cast<DWORD>((data.size() + 1) * sizeof(wchar_t)));
    RegCloseKey(k);
    return st == ERROR_SUCCESS;
}

//! `"C:\x\a.exe" "%1"` -> `C:\x\a.exe`
inline std::wstring
ExeFromCommand(const std::wstring& cmd)
{
    if (cmd.size() >= 2 && cmd[0] == L'"') {
        const size_t close = cmd.find(L'"', 1);
        if (close != std::wstring::npos) return cmd.substr(1, close - 1);
    }
    const size_t sp = cmd.find(L' ');
    return cmd.substr(0, sp);
}

inline bool
FileExistsW(const std::wstring& p)
{
    const DWORD a = GetFileAttributesW(p.c_str());
    return a != INVALID_FILE_ATTRIBUTES && !(a & FILE_ATTRIBUTE_DIRECTORY);
}

inline std::wstring
CommandFor(const std::wstring& exe)
{
    return L"\"" + exe + L"\" \"%1\"";
}

} // namespace view_protocol_detail

inline std::wstring
ThisExePath()
{
    wchar_t buf[MAX_PATH * 2] = {};
    GetModuleFileNameW(nullptr, buf, static_cast<DWORD>(sizeof(buf) / sizeof(buf[0])));
    return buf;
}

/*!
 * Ensure `HKCU\Software\Classes\<scheme>` routes to `exe`. Writes only when
 * the key is missing, malformed, or points at an executable that no longer
 * exists — a LIVE sibling that owns the scheme is left alone (type
 * forwarding covers that case). Returns true when the association is usable
 * afterwards (ours or a live sibling's).
 */
inline bool
EnsureViewProtocolRegistered(const std::wstring& exe, const wchar_t* friendlyName,
                             const wchar_t* scheme = kViewProtocolScheme)
{
    using namespace view_protocol_detail;
    const std::wstring root = std::wstring(L"Software\\Classes\\") + scheme;
    const std::wstring cmdKey = root + L"\\shell\\open\\command";
    const std::wstring current = ReadRegSz(HKEY_CURRENT_USER, cmdKey, nullptr);
    if (!current.empty()) {
        const std::wstring owner = ExeFromCommand(current);
        if (!owner.empty() && FileExistsW(owner)) return true; // live owner (us or a sibling)
    }
    bool ok = WriteRegSz(HKEY_CURRENT_USER, root, nullptr,
                         std::wstring(L"URL:") + friendlyName + L" Protocol");
    ok = WriteRegSz(HKEY_CURRENT_USER, root, L"URL Protocol", L"") && ok;
    ok = WriteRegSz(HKEY_CURRENT_USER, root + L"\\DefaultIcon", nullptr, exe + L",0") && ok;
    ok = WriteRegSz(HKEY_CURRENT_USER, cmdKey, nullptr, CommandFor(exe)) && ok;
    return ok;
}

/*!
 * Locate a sibling viewer by its install breadcrumb
 * (`HKLM\Software\DisplayXR\Demos\<demoKey>\InstallPath` + `exeName`). Empty
 * when not installed.
 */
inline std::wstring
FindSiblingViewer(const wchar_t* demoKey, const wchar_t* exeName)
{
    using namespace view_protocol_detail;
    const std::wstring key = std::wstring(L"Software\\DisplayXR\\Demos\\") + demoKey;
    std::wstring dir = ReadRegSz(HKEY_LOCAL_MACHINE, key, L"InstallPath");
    if (dir.empty()) return {};
    if (dir.back() != L'\\') dir.push_back(L'\\');
    const std::wstring exe = dir + exeName;
    return FileExistsW(exe) ? exe : std::wstring{};
}

/*!
 * Launch `exe` with a single argument (the protocol URL) via CreateProcessW.
 * The argument is quoted as a whole; a URL that survived launch_args.h has no
 * quotes or whitespace, so no further escaping is needed.
 */
inline bool
LaunchViewerWithUrl(const std::wstring& exe, const std::wstring& url)
{
    std::wstring cmd = L"\"" + exe + L"\" \"" + url + L"\"";
    STARTUPINFOW si{};
    si.cb = sizeof(si);
    PROCESS_INFORMATION pi{};
    // CreateProcessW may modify the buffer; give it a writable copy.
    const BOOL ok = CreateProcessW(exe.c_str(), cmd.data(), nullptr, nullptr, FALSE, 0, nullptr,
                                   nullptr, &si, &pi);
    if (ok) {
        CloseHandle(pi.hThread);
        CloseHandle(pi.hProcess);
    }
    return ok == TRUE;
}

/*!
 * Single-instance hand-off. Acquire a named mutex; if it already exists,
 * find the running instance's window by class and post the URL to it with
 * WM_COPYDATA (dwData = `kViewProtocolCopyDataId`, lpData = UTF-8 URL, NUL
 * terminated). Returns the mutex handle when THIS process is the instance
 * (keep it open for the process lifetime), or NULL when the URL was handed
 * to a running instance (caller should exit).
 */
constexpr ULONG_PTR kViewProtocolCopyDataId = 0x44585256; // 'DXRV'

inline HANDLE
AcquireSingleInstanceOrForward(const wchar_t* mutexName, const wchar_t* windowClass,
                               const std::string& utf8Url)
{
    HANDLE m = CreateMutexW(nullptr, FALSE, mutexName);
    if (m && GetLastError() != ERROR_ALREADY_EXISTS) return m;
    if (m) CloseHandle(m);
    // Give the first instance time to create its window.
    HWND target = nullptr;
    for (int i = 0; i < 50 && !target; ++i) {
        target = FindWindowW(windowClass, nullptr);
        if (!target) Sleep(100);
    }
    if (target) {
        COPYDATASTRUCT cds{};
        cds.dwData = kViewProtocolCopyDataId;
        cds.cbData = static_cast<DWORD>(utf8Url.size() + 1);
        cds.lpData = const_cast<char*>(utf8Url.c_str());
        SendMessageTimeoutW(target, WM_COPYDATA, 0, reinterpret_cast<LPARAM>(&cds),
                            SMTO_ABORTIFHUNG | SMTO_BLOCK, 3000, nullptr);
    }
    return nullptr;
}

} // namespace dxr

#endif // _WIN32
