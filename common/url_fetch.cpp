// Copyright 2026, DisplayXR
// SPDX-License-Identifier: BSL-1.0
/*!
 * @file
 * @brief  url_fetch.h implementation: pure helpers + the WinHTTP fetcher.
 */

#include "url_fetch.h"

#include <algorithm>
#include <cctype>

namespace dxr {

namespace {

std::string
Lower(std::string_view s)
{
    std::string out(s);
    for (char& c : out) c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
    return out;
}

bool
Allowed(const std::string& ext, const std::vector<std::string>& allowed)
{
    return std::find(allowed.begin(), allowed.end(), ext) != allowed.end();
}

} // namespace

std::string
UrlPathExtension(std::string_view url)
{
    const size_t cut = url.find_first_of("?#");
    std::string_view path = url.substr(0, cut);
    const size_t slash = path.find_last_of('/');
    std::string_view last = slash == std::string_view::npos ? path : path.substr(slash + 1);
    const size_t dot = last.find_last_of('.');
    if (dot == std::string_view::npos || dot + 1 >= last.size()) return {};
    std::string ext = Lower(last.substr(dot));
    for (char c : ext.substr(1)) {
        if (!std::isalnum(static_cast<unsigned char>(c))) return {};
    }
    return ext;
}

std::string
ResolveAssetExtension(std::string_view url, std::string_view contentType,
                      const std::vector<uint8_t>& head, const std::vector<std::string>& allowed)
{
    // 1. URL path extension.
    const std::string fromPath = UrlPathExtension(url);
    if (!fromPath.empty() && Allowed(fromPath, allowed)) return fromPath;

    // 2. Content-Type (parameters such as ";charset=" stripped).
    std::string ct = Lower(contentType);
    const size_t semi = ct.find(';');
    if (semi != std::string::npos) ct.resize(semi);
    while (!ct.empty() && ct.back() == ' ') ct.pop_back();
    struct Map {
        const char* type;
        const char* ext;
    };
    static const Map kMap[] = {
        {"model/gltf-binary", ".glb"},
        {"model/gltf+json", ".gltf"},
        {"model/stl", ".stl"},
        {"model/obj", ".obj"},
        {"model/vnd.usdz+zip", ".usdz"},
        {"application/x-ply", ".ply"},
        {"application/x-spz", ".spz"},
    };
    for (const Map& m : kMap) {
        if (ct == m.type && Allowed(m.ext, allowed)) return m.ext;
    }

    // 3. Magic bytes.
    if (head.size() >= 4) {
        if (head[0] == 'g' && head[1] == 'l' && head[2] == 'T' && head[3] == 'F' &&
            Allowed(".glb", allowed))
            return ".glb";
        if (head[0] == 'p' && head[1] == 'l' && head[2] == 'y' && (head[3] == '\n' || head[3] == '\r') &&
            Allowed(".ply", allowed))
            return ".ply";
        if (head[0] == 0x1f && head[1] == 0x8b && Allowed(".spz", allowed)) return ".spz"; // gzip
        if (head[0] == 'P' && head[1] == 'K' && Allowed(".usdz", allowed)) return ".usdz"; // zip
    }
    if (!head.empty()) {
        size_t i = 0;
        while (i < head.size() && (head[i] == ' ' || head[i] == '\n' || head[i] == '\r' || head[i] == '\t'))
            ++i;
        if (i < head.size() && head[i] == '{' && Allowed(".gltf", allowed)) return ".gltf";
    }
    return {};
}

} // namespace dxr

#if defined(_WIN32)

#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#include <winhttp.h>
#include <bcrypt.h>
#include <shlobj.h>

#include <cstdio>
#include <memory>

#pragma comment(lib, "winhttp.lib")
#pragma comment(lib, "bcrypt.lib")
#pragma comment(lib, "shell32.lib")

namespace dxr {

namespace {

std::wstring
Widen(std::string_view s)
{
    if (s.empty()) return {};
    const int n = MultiByteToWideChar(CP_UTF8, 0, s.data(), static_cast<int>(s.size()), nullptr, 0);
    std::wstring w(static_cast<size_t>(n > 0 ? n : 0), L'\0');
    if (n > 0) MultiByteToWideChar(CP_UTF8, 0, s.data(), static_cast<int>(s.size()), w.data(), n);
    return w;
}

std::string
Narrow(std::wstring_view w)
{
    if (w.empty()) return {};
    const int n = WideCharToMultiByte(CP_UTF8, 0, w.data(), static_cast<int>(w.size()), nullptr, 0,
                                      nullptr, nullptr);
    std::string s(static_cast<size_t>(n > 0 ? n : 0), '\0');
    if (n > 0)
        WideCharToMultiByte(CP_UTF8, 0, w.data(), static_cast<int>(w.size()), s.data(), n, nullptr,
                            nullptr);
    return s;
}

//! Hex SHA-1 of `data` via CNG. Empty string on failure.
std::wstring
Sha1Hex(std::string_view data)
{
    BCRYPT_ALG_HANDLE alg = nullptr;
    if (!BCRYPT_SUCCESS(BCryptOpenAlgorithmProvider(&alg, BCRYPT_SHA1_ALGORITHM, nullptr, 0)))
        return {};
    std::wstring hex;
    BCRYPT_HASH_HANDLE hash = nullptr;
    DWORD objLen = 0, cb = 0, hashLen = 0;
    if (BCRYPT_SUCCESS(BCryptGetProperty(alg, BCRYPT_OBJECT_LENGTH, reinterpret_cast<PUCHAR>(&objLen),
                                         sizeof(objLen), &cb, 0)) &&
        BCRYPT_SUCCESS(BCryptGetProperty(alg, BCRYPT_HASH_LENGTH, reinterpret_cast<PUCHAR>(&hashLen),
                                         sizeof(hashLen), &cb, 0))) {
        std::unique_ptr<UCHAR[]> obj(new UCHAR[objLen]);
        std::unique_ptr<UCHAR[]> digest(new UCHAR[hashLen]);
        if (BCRYPT_SUCCESS(BCryptCreateHash(alg, &hash, obj.get(), objLen, nullptr, 0, 0)) &&
            BCRYPT_SUCCESS(BCryptHashData(hash, reinterpret_cast<PUCHAR>(const_cast<char*>(data.data())),
                                          static_cast<ULONG>(data.size()), 0)) &&
            BCRYPT_SUCCESS(BCryptFinishHash(hash, digest.get(), hashLen, 0))) {
            static const wchar_t* kHex = L"0123456789abcdef";
            for (DWORD i = 0; i < hashLen; ++i) {
                hex.push_back(kHex[digest[i] >> 4]);
                hex.push_back(kHex[digest[i] & 0xf]);
            }
        }
        if (hash) BCryptDestroyHash(hash);
    }
    BCryptCloseAlgorithmProvider(alg, 0);
    return hex;
}

struct HInternetCloser {
    void operator()(HINTERNET h) const
    {
        if (h) WinHttpCloseHandle(h);
    }
};
using HInternetPtr = std::unique_ptr<std::remove_pointer_t<HINTERNET>, HInternetCloser>;

bool
FileExists(const std::wstring& p)
{
    const DWORD attr = GetFileAttributesW(p.c_str());
    return attr != INVALID_FILE_ATTRIBUTES && !(attr & FILE_ATTRIBUTE_DIRECTORY);
}

std::string
LastErrorText(const char* what)
{
    const DWORD e = GetLastError();
    char buf[128];
    std::snprintf(buf, sizeof(buf), "%s failed (error %lu)", what, static_cast<unsigned long>(e));
    return buf;
}

} // namespace

std::wstring
DefaultCacheDir(const wchar_t* appDirName)
{
    PWSTR base = nullptr;
    std::wstring dir;
    if (SUCCEEDED(SHGetKnownFolderPath(FOLDERID_LocalAppData, 0, nullptr, &base)) && base) {
        dir = base;
        dir += L"\\DisplayXR\\";
        dir += appDirName;
        dir += L"\\cache";
    }
    if (base) CoTaskMemFree(base);
    return dir;
}

UrlFetchResult
FetchUrlToCache(const std::string& url, const UrlFetchOptions& opts)
{
    UrlFetchResult r;
    if (opts.cacheDir.empty()) {
        r.error = "no cache directory";
        return r;
    }
    if (opts.allowedExtensions.empty()) {
        r.error = "no allowed extensions";
        return r;
    }

    // Cache directory (best effort, nested).
    {
        std::wstring p;
        for (size_t i = 0; i < opts.cacheDir.size(); ++i) {
            p.push_back(opts.cacheDir[i]);
            if ((opts.cacheDir[i] == L'\\' || i + 1 == opts.cacheDir.size()) && p.size() > 3)
                CreateDirectoryW(p.c_str(), nullptr);
        }
    }

    const std::wstring key = Sha1Hex(url);
    if (key.empty()) {
        r.error = "hashing failed";
        return r;
    }
    const std::wstring base = opts.cacheDir + L"\\" + key;

    // 1. Cache hit: any allowed extension already present for this key.
    if (!opts.noCache) {
        for (const std::string& ext : opts.allowedExtensions) {
            const std::wstring candidate = base + Widen(ext);
            if (FileExists(candidate)) {
                r.ok = true;
                r.fromCache = true;
                r.path = candidate;
                if (opts.progress) opts.progress(1, 1);
                return r;
            }
        }
    }

    // 2. Crack the URL.
    const std::wstring wurl = Widen(url);
    URL_COMPONENTS uc{};
    uc.dwStructSize = sizeof(uc);
    wchar_t host[256] = {}, path[2048] = {};
    uc.lpszHostName = host;
    uc.dwHostNameLength = 256;
    uc.lpszUrlPath = path;
    uc.dwUrlPathLength = 2048;
    if (!WinHttpCrackUrl(wurl.c_str(), 0, 0, &uc)) {
        r.error = "malformed URL";
        return r;
    }
    const bool https = uc.nScheme == INTERNET_SCHEME_HTTPS;

    HInternetPtr session(WinHttpOpen(L"DisplayXR-Viewer/1.0", WINHTTP_ACCESS_TYPE_AUTOMATIC_PROXY,
                                     WINHTTP_NO_PROXY_NAME, WINHTTP_NO_PROXY_BYPASS, 0));
    if (!session) {
        r.error = LastErrorText("WinHttpOpen");
        return r;
    }
    WinHttpSetTimeouts(session.get(), static_cast<int>(opts.connectTimeoutMs),
                       static_cast<int>(opts.connectTimeoutMs), static_cast<int>(opts.receiveTimeoutMs),
                       static_cast<int>(opts.receiveTimeoutMs));

    HInternetPtr conn(WinHttpConnect(session.get(), host, uc.nPort, 0));
    if (!conn) {
        r.error = LastErrorText("WinHttpConnect");
        return r;
    }
    HInternetPtr req(WinHttpOpenRequest(conn.get(), L"GET", path, nullptr, WINHTTP_NO_REFERER,
                                        WINHTTP_DEFAULT_ACCEPT_TYPES,
                                        https ? WINHTTP_FLAG_SECURE : 0));
    if (!req) {
        r.error = LastErrorText("WinHttpOpenRequest");
        return r;
    }
    // Never follow a downgrade redirect; the final URL is re-checked below too.
    DWORD policy = WINHTTP_OPTION_REDIRECT_POLICY_DISALLOW_HTTPS_TO_HTTP;
    WinHttpSetOption(req.get(), WINHTTP_OPTION_REDIRECT_POLICY, &policy, sizeof(policy));

    if (!WinHttpSendRequest(req.get(), WINHTTP_NO_ADDITIONAL_HEADERS, 0, WINHTTP_NO_REQUEST_DATA, 0, 0,
                            0) ||
        !WinHttpReceiveResponse(req.get(), nullptr)) {
        r.error = LastErrorText("request");
        return r;
    }

    // Final URL after redirects.
    {
        DWORD len = 0;
        WinHttpQueryOption(req.get(), WINHTTP_OPTION_URL, nullptr, &len);
        if (len > 0) {
            std::wstring fin(len / sizeof(wchar_t) + 1, L'\0');
            if (WinHttpQueryOption(req.get(), WINHTTP_OPTION_URL, fin.data(), &len)) {
                fin.resize(wcslen(fin.c_str()));
                r.finalUrl = Narrow(fin);
            }
        }
    }
    if (r.finalUrl.empty()) r.finalUrl = url;
    if (opts.urlAllowed && !opts.urlAllowed(r.finalUrl)) {
        r.error = "redirected to a URL the policy does not allow";
        return r;
    }

    DWORD status = 0, cb = sizeof(status);
    WinHttpQueryHeaders(req.get(), WINHTTP_QUERY_STATUS_CODE | WINHTTP_QUERY_FLAG_NUMBER,
                        WINHTTP_HEADER_NAME_BY_INDEX, &status, &cb, WINHTTP_NO_HEADER_INDEX);
    if (status != 200) {
        char buf[64];
        std::snprintf(buf, sizeof(buf), "HTTP %lu", static_cast<unsigned long>(status));
        r.error = buf;
        return r;
    }

    uint64_t total = 0;
    {
        wchar_t clen[32] = {};
        DWORD l = sizeof(clen);
        if (WinHttpQueryHeaders(req.get(), WINHTTP_QUERY_CONTENT_LENGTH, WINHTTP_HEADER_NAME_BY_INDEX,
                                clen, &l, WINHTTP_NO_HEADER_INDEX)) {
            total = _wcstoui64(clen, nullptr, 10);
            if (total > opts.maxBytes) {
                r.error = "asset larger than the download cap";
                return r;
            }
        }
    }
    std::string contentType;
    {
        wchar_t ct[128] = {};
        DWORD l = sizeof(ct);
        if (WinHttpQueryHeaders(req.get(), WINHTTP_QUERY_CONTENT_TYPE, WINHTTP_HEADER_NAME_BY_INDEX, ct,
                                &l, WINHTTP_NO_HEADER_INDEX))
            contentType = Narrow(ct);
    }

    // 3. Stream to <key>.part, capped.
    const std::wstring part = base + L".part";
    HANDLE file = CreateFileW(part.c_str(), GENERIC_WRITE, 0, nullptr, CREATE_ALWAYS,
                              FILE_ATTRIBUTE_NORMAL, nullptr);
    if (file == INVALID_HANDLE_VALUE) {
        r.error = LastErrorText("CreateFile");
        return r;
    }
    std::vector<uint8_t> head;
    std::vector<uint8_t> buf(64 * 1024);
    uint64_t done = 0;
    bool failed = false;
    for (;;) {
        DWORD avail = 0;
        if (!WinHttpQueryDataAvailable(req.get(), &avail)) {
            r.error = LastErrorText("WinHttpQueryDataAvailable");
            failed = true;
            break;
        }
        if (avail == 0) break;
        DWORD got = 0;
        const DWORD want = static_cast<DWORD>(std::min<size_t>(buf.size(), avail));
        if (!WinHttpReadData(req.get(), buf.data(), want, &got)) {
            r.error = LastErrorText("WinHttpReadData");
            failed = true;
            break;
        }
        if (got == 0) break;
        done += got;
        if (done > opts.maxBytes) {
            r.error = "asset larger than the download cap";
            failed = true;
            break;
        }
        if (head.size() < 16) head.insert(head.end(), buf.begin(), buf.begin() + std::min<size_t>(got, 16 - head.size()));
        DWORD written = 0;
        if (!WriteFile(file, buf.data(), got, &written, nullptr) || written != got) {
            r.error = LastErrorText("WriteFile");
            failed = true;
            break;
        }
        if (opts.progress) opts.progress(done, total);
    }
    CloseHandle(file);
    if (failed) {
        DeleteFileW(part.c_str());
        return r;
    }
    if (done == 0) {
        DeleteFileW(part.c_str());
        r.error = "empty response";
        return r;
    }

    const std::string ext = ResolveAssetExtension(r.finalUrl, contentType, head, opts.allowedExtensions);
    if (ext.empty()) {
        DeleteFileW(part.c_str());
        r.error = "could not determine the asset type";
        return r;
    }
    const std::wstring finalPath = base + Widen(ext);
    if (!MoveFileExW(part.c_str(), finalPath.c_str(), MOVEFILE_REPLACE_EXISTING)) {
        r.error = LastErrorText("MoveFileEx");
        DeleteFileW(part.c_str());
        return r;
    }
    r.ok = true;
    r.path = finalPath;
    r.bytes = done;
    if (opts.progress) opts.progress(done, done);
    return r;
}

} // namespace dxr

#endif // _WIN32
