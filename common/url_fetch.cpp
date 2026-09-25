// Copyright 2026, DisplayXR
// SPDX-License-Identifier: BSL-1.0
/*!
 * @file
 * @brief  url_fetch.h implementation: pure helpers, the WinHTTP fetcher and
 *         the desktop-Linux libcurl (dlopen) fetcher.
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
Sha1Hex(std::string_view data)
{
    uint32_t h[5] = {0x67452301u, 0xEFCDAB89u, 0x98BADCFEu, 0x10325476u, 0xC3D2E1F0u};
    auto rol = [](uint32_t v, int n) { return (v << n) | (v >> (32 - n)); };
    auto block = [&](const uint8_t* p) {
        uint32_t w[80];
        for (int i = 0; i < 16; ++i)
            w[i] = (uint32_t)p[4 * i] << 24 | (uint32_t)p[4 * i + 1] << 16 | (uint32_t)p[4 * i + 2] << 8 |
                   (uint32_t)p[4 * i + 3];
        for (int i = 16; i < 80; ++i) w[i] = rol(w[i - 3] ^ w[i - 8] ^ w[i - 14] ^ w[i - 16], 1);
        uint32_t a = h[0], b = h[1], c = h[2], d = h[3], e = h[4];
        for (int i = 0; i < 80; ++i) {
            uint32_t f, k;
            if (i < 20) { f = (b & c) | (~b & d); k = 0x5A827999u; }
            else if (i < 40) { f = b ^ c ^ d; k = 0x6ED9EBA1u; }
            else if (i < 60) { f = (b & c) | (b & d) | (c & d); k = 0x8F1BBCDCu; }
            else { f = b ^ c ^ d; k = 0xCA62C1D6u; }
            const uint32_t t = rol(a, 5) + f + e + k + w[i];
            e = d; d = c; c = rol(b, 30); b = a; a = t;
        }
        h[0] += a; h[1] += b; h[2] += c; h[3] += d; h[4] += e;
    };
    const uint64_t bitLen = (uint64_t)data.size() * 8u;
    size_t off = 0;
    for (; off + 64 <= data.size(); off += 64) block(reinterpret_cast<const uint8_t*>(data.data()) + off);
    uint8_t tail[128] = {};
    const size_t rem = (data.size() - off) & 63; // < 64 by the loop above; the mask says so to the compiler
    for (size_t i = 0; i < rem; ++i) tail[i] = static_cast<uint8_t>(data[off + i]);
    tail[rem] = 0x80;
    const size_t tailLen = rem + 1 + 8 <= 64 ? 64 : 128;
    for (int i = 0; i < 8; ++i) tail[tailLen - 1 - i] = static_cast<uint8_t>(bitLen >> (8 * i));
    block(tail);
    if (tailLen == 128) block(tail + 64);
    static const char* kHex = "0123456789abcdef";
    std::string out;
    out.reserve(40);
    for (uint32_t v : h)
        for (int i = 7; i >= 0; --i) out.push_back(kHex[(v >> (4 * i)) & 0xf]);
    return out;
}

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
        if (head[0] == 'N' && head[1] == 'G' && head[2] == 'S' && head[3] == 'P' &&
            Allowed(".spz", allowed))
            return ".spz"; // raw (un-gzipped) spz payload, e.g. splat-transform output
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

//! Hex SHA-1 of `data` via CNG. Empty string on failure. (Same digest as the
//! portable dxr::Sha1Hex; kept on CNG so the Windows path is unchanged.)
std::wstring
Sha1HexCng(std::string_view data)
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

    const std::wstring key = Sha1HexCng(url);
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

#if defined(__linux__) && !defined(__ANDROID__)

/*
 * Desktop Linux: libcurl through dlopen. Only the ABI-stable easy interface is
 * used, and its constants are spelled here (values from curl.h, unchanged
 * since they were introduced) so the build needs no libcurl headers either.
 */

#include <dlfcn.h>
#include <errno.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>

#include <mutex>

namespace dxr {

namespace {

// ---- the slice of curl.h this uses -----------------------------------------
using CURL = void;
using CURLcode = int;
enum : int {
    kCurleOk = 0,
    kCurleWriteError = 23,
    kCurleOperationTimedout = 28,
    kCurleTooManyRedirects = 47,
    kCurleUnknownOption = 48,
    kCurleFilesizeExceeded = 63,
};
enum : long {
    kOptWriteData = 10001,
    kOptUrl = 10002,
    kOptUserAgent = 10018,
    kOptWriteFunction = 20011,
    kOptLowSpeedLimit = 19,
    kOptLowSpeedTime = 20,
    kOptNoProgress = 43,
    kOptFollowLocation = 52,
    kOptMaxRedirs = 68,
    kOptNoSignal = 99,
    kOptConnectTimeoutMs = 156,
    kOptProtocols = 181,
    kOptRedirProtocols = 182,
    kOptProtocolsStr = 10318,      // 7.85+
    kOptRedirProtocolsStr = 10319, // 7.85+
    kOptMaxFileSizeLarge = 30117,
};
enum : long {
    kInfoEffectiveUrl = 0x100001,
    kInfoContentType = 0x100012,
    kInfoResponseCode = 0x200002,
    kInfoContentLengthDownloadT = 0x60000F,
};
constexpr long kCurlGlobalDefault = 3;
constexpr long kProtoHttp = 1, kProtoHttps = 2;

struct CurlApi {
    CURLcode (*global_init)(long) = nullptr;
    CURL* (*easy_init)() = nullptr;
    CURLcode (*easy_setopt)(CURL*, long, ...) = nullptr;
    CURLcode (*easy_perform)(CURL*) = nullptr;
    CURLcode (*easy_getinfo)(CURL*, long, ...) = nullptr;
    void (*easy_cleanup)(CURL*) = nullptr;
    const char* (*easy_strerror)(CURLcode) = nullptr;
    const char* soname = nullptr;
    bool ok = false;
};

//! Loaded once per process; never unloaded (curl keeps global state).
const CurlApi&
Curl()
{
    static CurlApi api;
    static std::once_flag once;
    std::call_once(once, [] {
        static const char* kNames[] = {"libcurl.so.4", "libcurl-gnutls.so.4"};
        void* h = nullptr;
        for (const char* n : kNames) {
            h = dlopen(n, RTLD_NOW | RTLD_LOCAL);
            if (h != nullptr) {
                api.soname = n;
                break;
            }
        }
        if (h == nullptr) return;
        api.global_init = reinterpret_cast<CURLcode (*)(long)>(dlsym(h, "curl_global_init"));
        api.easy_init = reinterpret_cast<CURL* (*)()>(dlsym(h, "curl_easy_init"));
        api.easy_setopt = reinterpret_cast<CURLcode (*)(CURL*, long, ...)>(dlsym(h, "curl_easy_setopt"));
        api.easy_perform = reinterpret_cast<CURLcode (*)(CURL*)>(dlsym(h, "curl_easy_perform"));
        api.easy_getinfo = reinterpret_cast<CURLcode (*)(CURL*, long, ...)>(dlsym(h, "curl_easy_getinfo"));
        api.easy_cleanup = reinterpret_cast<void (*)(CURL*)>(dlsym(h, "curl_easy_cleanup"));
        api.easy_strerror = reinterpret_cast<const char* (*)(CURLcode)>(dlsym(h, "curl_easy_strerror"));
        api.ok = api.global_init && api.easy_init && api.easy_setopt && api.easy_perform && api.easy_getinfo &&
                 api.easy_cleanup && api.easy_strerror;
        // curl_global_init is not thread-safe; call_once serialises it.
        if (api.ok && api.global_init(kCurlGlobalDefault) != kCurleOk) api.ok = false;
    });
    return api;
}

bool
FileExistsPosix(const std::string& p)
{
    struct stat st;
    return stat(p.c_str(), &st) == 0 && S_ISREG(st.st_mode);
}

//! mkdir -p; best effort, like the Windows cache-dir creation.
void
MakeDirs(const std::string& dir)
{
    std::string p;
    for (size_t i = 0; i < dir.size(); ++i) {
        p.push_back(dir[i]);
        if ((dir[i] == '/' || i + 1 == dir.size()) && p.size() > 1) mkdir(p.c_str(), 0700);
    }
}

std::string
ErrnoText(const char* what)
{
    char buf[160];
    std::snprintf(buf, sizeof(buf), "%s failed (%s)", what, strerror(errno));
    return buf;
}

//! Per-transfer state the write callback works on.
struct Transfer {
    const CurlApi* api = nullptr;
    CURL* curl = nullptr;
    const UrlFetchOptions* opts = nullptr;
    FILE* file = nullptr;
    bool checked = false; // the first-body checks ran
    uint64_t total = 0;
    uint64_t done = 0;
    std::vector<uint8_t> head;
    std::string contentType;
    std::string finalUrl;
    std::string abortReason; // non-empty = the callback aborted the transfer
};

/*
 * The body. The FIRST call runs what WinHTTP checks between receiving the
 * response and reading it: the redirect chain has ended (curl hands redirect
 * bodies to nobody), so the final URL is re-checked against the policy, the
 * status must be 200 and a declared Content-Length must fit the cap — all
 * before a byte reaches the disk.
 */
size_t
WriteCb(char* ptr, size_t size, size_t nmemb, void* ud)
{
    Transfer* t = static_cast<Transfer*>(ud);
    const size_t got = size * nmemb;
    if (!t->checked) {
        t->checked = true;
        char* eff = nullptr;
        if (t->api->easy_getinfo(t->curl, kInfoEffectiveUrl, &eff) == kCurleOk && eff != nullptr)
            t->finalUrl = eff;
        if (t->opts->urlAllowed && !t->opts->urlAllowed(t->finalUrl)) {
            t->abortReason = "redirected to a URL the policy does not allow";
            return 0;
        }
        long status = 0;
        t->api->easy_getinfo(t->curl, kInfoResponseCode, &status);
        if (status != 200) {
            char buf[64];
            std::snprintf(buf, sizeof(buf), "HTTP %ld", status);
            t->abortReason = buf;
            return 0;
        }
        int64_t clen = -1;
        if (t->api->easy_getinfo(t->curl, kInfoContentLengthDownloadT, &clen) == kCurleOk && clen > 0) {
            t->total = static_cast<uint64_t>(clen);
            if (t->total > t->opts->maxBytes) {
                t->abortReason = "asset larger than the download cap";
                return 0;
            }
        }
        char* ct = nullptr;
        if (t->api->easy_getinfo(t->curl, kInfoContentType, &ct) == kCurleOk && ct != nullptr) t->contentType = ct;
    }
    if (got == 0) return 0;
    t->done += got;
    if (t->done > t->opts->maxBytes) {
        t->abortReason = "asset larger than the download cap";
        return 0;
    }
    if (t->head.size() < 16)
        t->head.insert(t->head.end(), reinterpret_cast<uint8_t*>(ptr),
                       reinterpret_cast<uint8_t*>(ptr) + std::min<size_t>(got, 16 - t->head.size()));
    if (std::fwrite(ptr, 1, got, t->file) != got) {
        t->abortReason = ErrnoText("write");
        return 0;
    }
    if (t->opts->progress) t->opts->progress(t->done, t->total);
    return got;
}

} // namespace

std::string
DefaultCacheDir(const char* appDirName)
{
    std::string base;
    const char* xdg = getenv("XDG_CACHE_HOME");
    if (xdg != nullptr && xdg[0] == '/') {
        base = xdg; // the spec: a relative value is invalid and ignored
    } else {
        const char* home = getenv("HOME");
        if (home == nullptr || home[0] != '/') return {};
        base = std::string(home) + "/.cache";
    }
    return base + "/displayxr/" + (appDirName != nullptr ? appDirName : "viewer");
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
    MakeDirs(opts.cacheDir);
    const std::string base = opts.cacheDir + "/" + Sha1Hex(url);

    // 1. Cache hit: any allowed extension already present for this key.
    if (!opts.noCache) {
        for (const std::string& ext : opts.allowedExtensions) {
            const std::string candidate = base + ext;
            if (FileExistsPosix(candidate)) {
                r.ok = true;
                r.fromCache = true;
                r.path = candidate;
                if (opts.progress) opts.progress(1, 1);
                return r;
            }
        }
    }

    const CurlApi& api = Curl();
    if (!api.ok) {
        r.error = "no HTTP library (libcurl) — install libcurl4t64 (Ubuntu 24.04+) or libcurl4 (22.04)";
        return r;
    }
    const bool https = url.compare(0, 8, "https://") == 0;
    if (!https && url.compare(0, 7, "http://") != 0) {
        r.error = "malformed URL";
        return r;
    }

    const std::string part = base + ".part";
    Transfer t;
    t.api = &api;
    t.opts = &opts;
    t.finalUrl = url;
    t.file = std::fopen(part.c_str(), "wb");
    if (t.file == nullptr) {
        r.error = ErrnoText("create cache file");
        return r;
    }
    t.curl = api.easy_init();
    if (t.curl == nullptr) {
        std::fclose(t.file);
        unlink(part.c_str());
        r.error = "curl_easy_init failed";
        return r;
    }
    CURL* c = t.curl;
    api.easy_setopt(c, kOptUrl, url.c_str());
    api.easy_setopt(c, kOptUserAgent, "DisplayXR-Viewer/1.0");
    api.easy_setopt(c, kOptNoSignal, 1L);
    api.easy_setopt(c, kOptNoProgress, 1L);
    api.easy_setopt(c, kOptFollowLocation, 1L);
    api.easy_setopt(c, kOptMaxRedirs, 10L);
    api.easy_setopt(c, kOptConnectTimeoutMs, static_cast<long>(opts.connectTimeoutMs));
    api.easy_setopt(c, kOptLowSpeedLimit, 1L);
    api.easy_setopt(c, kOptLowSpeedTime, static_cast<long>((opts.receiveTimeoutMs + 999) / 1000));
    api.easy_setopt(c, kOptMaxFileSizeLarge, static_cast<int64_t>(opts.maxBytes));
    // http(s) only, and never a downgrade redirect (WinHTTP's
    // REDIRECT_POLICY_DISALLOW_HTTPS_TO_HTTP); the final URL is re-checked by
    // the policy in the write callback too.
    if (api.easy_setopt(c, kOptProtocolsStr, "http,https") == kCurleUnknownOption)
        api.easy_setopt(c, kOptProtocols, kProtoHttp | kProtoHttps);
    if (api.easy_setopt(c, kOptRedirProtocolsStr, https ? "https" : "http,https") == kCurleUnknownOption)
        api.easy_setopt(c, kOptRedirProtocols, https ? kProtoHttps : (kProtoHttp | kProtoHttps));
    size_t (*cb)(char*, size_t, size_t, void*) = &WriteCb;
    api.easy_setopt(c, kOptWriteFunction, cb);
    api.easy_setopt(c, kOptWriteData, static_cast<void*>(&t));

    const CURLcode rc = api.easy_perform(c);
    const bool closed = std::fclose(t.file) == 0;
    t.file = nullptr;
    if (!t.checked && rc == kCurleOk) {
        // No body at all: still apply the status / policy checks.
        char* eff = nullptr;
        if (api.easy_getinfo(c, kInfoEffectiveUrl, &eff) == kCurleOk && eff != nullptr) t.finalUrl = eff;
        long status = 0;
        api.easy_getinfo(c, kInfoResponseCode, &status);
        if (opts.urlAllowed && !opts.urlAllowed(t.finalUrl)) {
            t.abortReason = "redirected to a URL the policy does not allow";
        } else if (status != 200) {
            char buf[64];
            std::snprintf(buf, sizeof(buf), "HTTP %ld", status);
            t.abortReason = buf;
        }
    }
    api.easy_cleanup(c);
    r.finalUrl = t.finalUrl;

    if (!t.abortReason.empty()) {
        r.error = t.abortReason;
    } else if (rc == kCurleFilesizeExceeded) {
        r.error = "asset larger than the download cap";
    } else if (rc == kCurleTooManyRedirects) {
        r.error = "too many redirects";
    } else if (rc == kCurleOperationTimedout) {
        r.error = "request timed out";
    } else if (rc != kCurleOk) {
        char buf[192];
        std::snprintf(buf, sizeof(buf), "request failed (%s)", api.easy_strerror(rc));
        r.error = buf;
    } else if (!closed) {
        r.error = ErrnoText("write");
    } else if (t.done == 0) {
        r.error = "empty response";
    }
    if (!r.error.empty()) {
        unlink(part.c_str());
        return r;
    }

    const std::string ext = ResolveAssetExtension(r.finalUrl, t.contentType, t.head, opts.allowedExtensions);
    if (ext.empty()) {
        unlink(part.c_str());
        r.error = "could not determine the asset type";
        return r;
    }
    const std::string finalPath = base + ext;
    if (std::rename(part.c_str(), finalPath.c_str()) != 0) {
        r.error = ErrnoText("rename");
        unlink(part.c_str());
        return r;
    }
    r.ok = true;
    r.path = finalPath;
    r.bytes = t.done;
    if (opts.progress) opts.progress(t.done, t.done);
    return r;
}

} // namespace dxr

#endif // desktop Linux
