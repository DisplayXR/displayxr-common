// Copyright 2026, DisplayXR
// SPDX-License-Identifier: BSL-1.0
/*!
 * @file
 * @brief  Launch contract for undockable viewers: CLI flags + the
 *         `displayxr-view:` URL protocol, parsed and policy-checked in one place.
 *
 * A DisplayXR viewer (model viewer, splat viewer, ...) can be started by a
 * shell, by a web page through the OS protocol handler, or by a native app
 * (a CAD tool "undocking" a part). All three arrive as argv; this header
 * turns argv into a validated `LaunchArgs` so every viewer applies the SAME
 * grammar and the SAME security policy. Nothing in here touches a window or
 * the network — see url_fetch.h and view_protocol.h for those.
 *
 * ## Grammar
 *
 * Flags are GNU-style `--key=value` (never `--key value`: a value that
 * starts with `-` or contains spaces would otherwise need quoting rules that
 * differ between cmd, PowerShell and CreateProcess). `--` ends flag parsing.
 * The first token that is neither a flag nor a protocol URL is the legacy
 * positional model path, kept for every existing launcher and shell tile.
 *
 *   --transparent          borderless + topmost + shaped from the first frame
 *   --rect=X,Y,W,H         window rect, physical virtual-screen pixels
 *   --src=<url|path>       asset to load instead of the bundled sample
 *   --vh=<metres>          virtual display height the asset was authored at
 *   --title=<suffix>       appended to the viewer's window title, never replaces it
 *   --type=model|splat     which viewer the launch is meant for (routing hint)
 *   --env=studio|sky|none  lighting the sender rendered with, so the undocked view matches
 *   --dpr=<float>          the launching page's devicePixelRatio (logged only)
 *   --max-bytes=<n>        download cap (default 256 MiB)
 *   --no-cache             bypass the download cache (dev aid)
 *   --allow-local          native callers only: permit file:/local src in a protocol URL
 *
 * The protocol form carries the same fields as a query string:
 *
 *   displayxr-view://open?src=<pct>&type=model|splat&rect=X,Y,W,H&vh=0.2
 *                        &dpr=2.5&title=<pct>&env=studio&transparent=1&v=1
 *
 * `open` is the AUTHORITY (verb), leaving room for future verbs; `v=1` lets an
 * old handler reject a future grammar loudly instead of half-honouring it.
 * A protocol launch is TRANSPARENT by default — undocking into a floating
 * overlay is what the scheme exists for — and `transparent=0` opts out for a
 * framed, positionable window. On the CLI, `--transparent` stays opt-in.
 * Every value is percent-encoded by the sender (`encodeURIComponent`); only
 * `%XX` is decoded here — `+` is NOT a space.
 *
 * ## Trust boundary — read before changing the policy
 *
 * The browser's one-time "Open <app>?" dialog with its "Always allow" tick is
 * the ONLY consent gate on the protocol path, it is sticky per origin, and
 * once ticked ANY page on that origin can drive this parser. So a viewer must
 * be safe against a hostile URL on its own merits. Hence, when
 * `fromProtocol` is set (and `--allow-local` is not):
 *   - `src` must be `https:` (any host) or `http:` on loopback only. The
 *     storefront demo is a static export served from `http://localhost`, which
 *     is why loopback http is admitted at all.
 *   - `file:`, UNC and bare local paths are REJECTED. A protocol-launched
 *     viewer that opened arbitrary local files would be a file-existence
 *     oracle through its own error toasts, reachable from any web page.
 *   - lengths are capped, control characters are refused, rects are bounded.
 * `--allow-local` exists for native callers (a CAD app spawning the viewer
 * with an exported file); it is an argv flag, so a web page can never set it.
 */

#pragma once

#include <cctype>
#include <cmath>
#include <cstdint>
#include <cstdlib>
#include <string>
#include <string_view>
#include <vector>

namespace dxr {

enum class LaunchSrcKind { None, LocalPath, Url };

struct LaunchArgs {
    bool transparent = false;

    bool hasRect = false;
    int32_t rectX = 0, rectY = 0, rectW = 0, rectH = 0;

    //! Validated asset source, UTF-8. A URL (`http:`/`https:`) or a local
    //! path (only when policy allows — see the header comment).
    std::string src;
    LaunchSrcKind srcKind = LaunchSrcKind::None;

    //! Legacy first non-flag token, UTF-8. Always user-initiated (never set
    //! from a protocol URL), so it carries no policy restriction.
    std::string positionalPath;

    bool hasVh = false;
    float vh = 0.f;
    bool hasDpr = false;
    float dpr = 0.f;

    std::string title; //!< window-title SUFFIX, control chars stripped, <= 64 bytes
    std::string type;  //!< lower-case routing hint ("model", "splat", ""), <= 16 chars
    //! Lighting/environment hint the sender rendered with ("studio", "sky", "none", ""),
    //! lower-case, <= 16 chars, so the undocked view matches the page. Viewers map it.
    std::string env;

    uint64_t maxBytes = 256ull << 20;
    bool noCache = false;
    bool allowLocal = false;

    bool fromProtocol = false;
    std::string protocolUrl; //!< the raw URL token, for forwarding to a sibling viewer
    int protocolVersion = 0;

    std::vector<std::string> errors;   //!< any entry => the launch must be refused
    std::vector<std::string> warnings; //!< log and continue

    bool ok() const { return errors.empty(); }
};

namespace launch_detail {

inline bool
IEqualsAscii(std::string_view a, std::string_view b)
{
    if (a.size() != b.size()) return false;
    for (size_t i = 0; i < a.size(); ++i) {
        if (std::tolower(static_cast<unsigned char>(a[i])) !=
            std::tolower(static_cast<unsigned char>(b[i])))
            return false;
    }
    return true;
}

inline bool
IStartsWithAscii(std::string_view s, std::string_view prefix)
{
    return s.size() >= prefix.size() && IEqualsAscii(s.substr(0, prefix.size()), prefix);
}

inline int
HexVal(char c)
{
    if (c >= '0' && c <= '9') return c - '0';
    if (c >= 'a' && c <= 'f') return 10 + (c - 'a');
    if (c >= 'A' && c <= 'F') return 10 + (c - 'A');
    return -1;
}

//! Strict `%XX` decoding. `+` is left alone. Returns false on a malformed
//! escape (the caller treats that as a hostile/garbled URL, not as text).
inline bool
PercentDecode(std::string_view in, std::string& out)
{
    out.clear();
    out.reserve(in.size());
    for (size_t i = 0; i < in.size(); ++i) {
        if (in[i] == '%') {
            if (i + 2 >= in.size()) return false;
            const int hi = HexVal(in[i + 1]);
            const int lo = HexVal(in[i + 2]);
            if (hi < 0 || lo < 0) return false;
            out.push_back(static_cast<char>((hi << 4) | lo));
            i += 2;
        } else {
            out.push_back(in[i]);
        }
    }
    return true;
}

inline bool
HasControlOrSpace(std::string_view s)
{
    for (unsigned char c : s) {
        if (c < 0x20 || c == 0x7f || c == ' ') return true;
    }
    return false;
}

inline bool
ParseInt32(std::string_view s, int32_t& out)
{
    if (s.empty() || s.size() > 11) return false;
    size_t i = 0;
    bool neg = false;
    if (s[0] == '-' || s[0] == '+') {
        neg = s[0] == '-';
        i = 1;
        if (s.size() == 1) return false;
    }
    int64_t v = 0;
    for (; i < s.size(); ++i) {
        if (s[i] < '0' || s[i] > '9') return false;
        v = v * 10 + (s[i] - '0');
        if (v > INT32_MAX) return false;
    }
    out = static_cast<int32_t>(neg ? -v : v);
    return true;
}

inline bool
ParseU64(std::string_view s, uint64_t& out)
{
    if (s.empty() || s.size() > 20) return false;
    uint64_t v = 0;
    for (char c : s) {
        if (c < '0' || c > '9') return false;
        const uint64_t nv = v * 10 + static_cast<uint64_t>(c - '0');
        if (nv < v) return false;
        v = nv;
    }
    out = v;
    return true;
}

inline bool
ParseFloat(std::string_view s, float& out)
{
    if (s.empty() || s.size() > 32) return false;
    for (char c : s) {
        if (!(std::isdigit(static_cast<unsigned char>(c)) || c == '.' || c == '-' || c == '+' ||
              c == 'e' || c == 'E'))
            return false;
    }
    std::string tmp(s);
    char* end = nullptr;
    const double v = std::strtod(tmp.c_str(), &end);
    if (end != tmp.c_str() + tmp.size()) return false;
    if (!std::isfinite(v)) return false;
    out = static_cast<float>(v);
    return true;
}

//! `scheme:` prefix per RFC 3986 (`[A-Za-z][A-Za-z0-9+.-]*:`), at least two
//! characters so a Windows drive letter (`C:\...`) is a path, not a scheme.
inline std::string
UrlScheme(std::string_view s)
{
    size_t i = 0;
    if (s.empty() || !std::isalpha(static_cast<unsigned char>(s[0]))) return {};
    for (i = 1; i < s.size(); ++i) {
        const unsigned char c = static_cast<unsigned char>(s[i]);
        if (c == ':') break;
        if (!(std::isalnum(c) || c == '+' || c == '.' || c == '-')) return {};
    }
    if (i >= s.size() || i < 2) return {};
    std::string scheme(s.substr(0, i));
    for (char& c : scheme) c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
    return scheme;
}

//! Host part of `scheme://[userinfo@]host[:port]/...`, lower-cased. Empty
//! when there is no authority or it is malformed. `hadUserinfo` reports an
//! `@` in the authority (rejected by policy: a classic confusion vector).
inline std::string
UrlHost(std::string_view url, bool& hadUserinfo)
{
    hadUserinfo = false;
    const size_t colon = url.find(':');
    if (colon == std::string_view::npos) return {};
    std::string_view rest = url.substr(colon + 1);
    if (rest.size() < 2 || rest[0] != '/' || rest[1] != '/') return {};
    rest.remove_prefix(2);
    const size_t end = rest.find_first_of("/?#");
    std::string_view authority = rest.substr(0, end);
    const size_t at = authority.find('@');
    if (at != std::string_view::npos) {
        hadUserinfo = true;
        authority = authority.substr(at + 1);
    }
    std::string host;
    if (!authority.empty() && authority[0] == '[') {
        const size_t close = authority.find(']');
        if (close == std::string_view::npos) return {};
        host = std::string(authority.substr(0, close + 1));
    } else {
        const size_t port = authority.find(':');
        host = std::string(authority.substr(0, port));
    }
    for (char& c : host) c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
    return host;
}

inline bool
IsLoopbackHost(std::string_view host)
{
    return host == "localhost" || host == "127.0.0.1" || host == "[::1]";
}

//! `file:///C:/a/b.glb` -> `C:/a/b.glb`; `file://server/share/x` -> `//server/share/x`.
inline std::string
FileUrlToPath(std::string_view url)
{
    std::string_view rest = url.substr(5); // "file:"
    if (rest.size() >= 2 && rest[0] == '/' && rest[1] == '/') {
        rest.remove_prefix(2);
        if (!rest.empty() && rest[0] == '/') {
            rest.remove_prefix(1); // file:///C:/... -> C:/...
        } else {
            return "//" + std::string(rest); // UNC host form
        }
    }
    return std::string(rest);
}

//! Strip control characters and cap at `maxBytes`, never cutting a UTF-8
//! sequence in half.
inline std::string
SanitizeTitle(std::string_view in, size_t maxBytes)
{
    std::string out;
    for (unsigned char c : in) {
        if (c < 0x20 || c == 0x7f) continue;
        out.push_back(static_cast<char>(c));
    }
    if (out.size() > maxBytes) {
        size_t cut = maxBytes;
        while (cut > 0 && (static_cast<unsigned char>(out[cut]) & 0xC0) == 0x80) --cut;
        out.resize(cut);
    }
    return out;
}

inline void
ParseRect(std::string_view v, LaunchArgs& a, const char* where)
{
    int32_t parts[4];
    size_t start = 0;
    for (int k = 0; k < 4; ++k) {
        const size_t comma = v.find(',', start);
        const std::string_view piece =
            v.substr(start, comma == std::string_view::npos ? std::string_view::npos : comma - start);
        if (!ParseInt32(piece, parts[k]) || (k < 3 && comma == std::string_view::npos) ||
            (k == 3 && comma != std::string_view::npos)) {
            a.errors.push_back(std::string(where) + ": rect must be X,Y,W,H integers");
            return;
        }
        start = comma + 1;
    }
    a.hasRect = true;
    a.rectX = parts[0];
    a.rectY = parts[1];
    a.rectW = parts[2];
    a.rectH = parts[3];
}

inline void
ApplyKeyValue(std::string_view key, std::string_view value, LaunchArgs& a, const char* where)
{
    if (key == "src") {
        a.src = std::string(value);
    } else if (key == "rect") {
        ParseRect(value, a, where);
    } else if (key == "vh") {
        float f = 0.f;
        if (!ParseFloat(value, f)) {
            a.errors.push_back(std::string(where) + ": vh is not a number");
        } else {
            a.hasVh = true;
            a.vh = f;
        }
    } else if (key == "dpr") {
        float f = 0.f;
        if (!ParseFloat(value, f)) {
            a.errors.push_back(std::string(where) + ": dpr is not a number");
        } else {
            a.hasDpr = true;
            a.dpr = f;
        }
    } else if (key == "title") {
        a.title = SanitizeTitle(value, 64);
    } else if (key == "type") {
        std::string t(value);
        for (char& c : t) c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
        a.type = t;
    } else if (key == "env") {
        std::string e(value);
        for (char& c : e) c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
        a.env = e;
    } else if (key == "max-bytes") {
        uint64_t n = 0;
        if (!ParseU64(value, n)) {
            a.errors.push_back(std::string(where) + ": max-bytes is not an integer");
        } else {
            a.maxBytes = n;
        }
    } else if (key == "transparent") {
        // Protocol form: launches are transparent BY DEFAULT (undocking is the whole point of
        // the scheme); `transparent=0` opts out for a framed, positionable window.
        if (value.empty() || value == "1" || IEqualsAscii(value, "true")) a.transparent = true;
        else if (value == "0" || IEqualsAscii(value, "false")) a.transparent = false;
        else a.errors.push_back(std::string(where) + ": transparent must be 0 or 1");
    } else if (key == "v") {
        int32_t v = 0;
        if (!ParseInt32(value, v) || v < 1) {
            a.errors.push_back(std::string(where) + ": v must be a positive integer");
        } else {
            a.protocolVersion = v;
        }
    } else {
        a.warnings.push_back(std::string(where) + ": unknown key '" + std::string(key) + "' ignored");
    }
}

constexpr std::string_view kProtocolPrefix = "displayxr-view:";
constexpr size_t kMaxProtocolUrlBytes = 2048;
constexpr size_t kMaxSrcBytes = 2048;

inline void
ParseProtocolUrl(std::string_view url, LaunchArgs& a)
{
    a.fromProtocol = true;
    a.protocolUrl = std::string(url);
    a.transparent = true; // the protocol's default; `transparent=0` in the query opts out
    if (url.size() > kMaxProtocolUrlBytes) {
        a.errors.push_back("protocol: URL longer than 2048 bytes");
        return;
    }
    if (HasControlOrSpace(url)) {
        a.errors.push_back("protocol: URL contains whitespace or control characters");
        return;
    }
    std::string_view rest = url.substr(kProtocolPrefix.size());
    if (rest.size() >= 2 && rest[0] == '/' && rest[1] == '/') rest.remove_prefix(2);
    const size_t verbEnd = rest.find_first_of("/?#");
    const std::string_view verb = rest.substr(0, verbEnd);
    if (!IEqualsAscii(verb, "open")) {
        a.errors.push_back("protocol: unknown verb '" + std::string(verb) + "' (expected 'open')");
        return;
    }
    rest = verbEnd == std::string_view::npos ? std::string_view{} : rest.substr(verbEnd);
    if (!rest.empty() && rest[0] == '/') rest.remove_prefix(1);
    if (!rest.empty() && rest[0] == '?') rest.remove_prefix(1);
    const size_t frag = rest.find('#');
    if (frag != std::string_view::npos) rest = rest.substr(0, frag);

    bool sawV = false;
    while (!rest.empty()) {
        const size_t amp = rest.find('&');
        const std::string_view pair = rest.substr(0, amp);
        rest = amp == std::string_view::npos ? std::string_view{} : rest.substr(amp + 1);
        if (pair.empty()) continue;
        const size_t eq = pair.find('=');
        std::string key, value;
        if (!PercentDecode(pair.substr(0, eq), key) ||
            !PercentDecode(eq == std::string_view::npos ? std::string_view{} : pair.substr(eq + 1),
                           value)) {
            a.errors.push_back("protocol: malformed percent-encoding in '" + std::string(pair) + "'");
            return;
        }
        if (key == "v") sawV = true;
        ApplyKeyValue(key, value, a, "protocol");
    }
    if (!sawV) {
        a.warnings.push_back("protocol: no v= version, assuming v=1");
        a.protocolVersion = 1;
    } else if (a.protocolVersion > 1) {
        a.errors.push_back("protocol: v=" + std::to_string(a.protocolVersion) +
                           " is newer than this viewer understands (v=1)");
    }
}

//! The policy pass. Runs once after all tokens are in, so `--allow-local`
//! after the URL still counts.
inline void
ApplyPolicy(LaunchArgs& a)
{
    const bool untrusted = a.fromProtocol && !a.allowLocal;

    if (!a.src.empty()) {
        if (a.src.size() > kMaxSrcBytes) {
            a.errors.push_back("src: longer than 2048 bytes");
            a.src.clear();
        } else if (a.src.find('\0') != std::string::npos ||
                   [&] {
                       for (unsigned char c : a.src)
                           if (c < 0x20 || c == 0x7f) return true;
                       return false;
                   }()) {
            a.errors.push_back("src: contains control characters");
            a.src.clear();
        } else {
            const std::string scheme = UrlScheme(a.src);
            if (scheme == "https" || scheme == "http") {
                bool userinfo = false;
                const std::string host = UrlHost(a.src, userinfo);
                if (host.empty()) {
                    a.errors.push_back("src: URL has no host");
                } else if (userinfo) {
                    a.errors.push_back("src: userinfo ('@') in URL is not allowed");
                } else if (scheme == "http" && !IsLoopbackHost(host)) {
                    a.errors.push_back("src: plain http is only allowed on loopback (localhost)");
                } else {
                    a.srcKind = LaunchSrcKind::Url;
                }
            } else if (scheme == "file") {
                if (untrusted) {
                    a.errors.push_back("src: file: URLs are not accepted from a protocol launch");
                } else {
                    a.src = FileUrlToPath(a.src);
                    a.srcKind = LaunchSrcKind::LocalPath;
                }
            } else if (!scheme.empty()) {
                a.errors.push_back("src: unsupported URL scheme '" + scheme + "'");
            } else {
                if (untrusted) {
                    a.errors.push_back("src: local paths are not accepted from a protocol launch");
                } else {
                    a.srcKind = LaunchSrcKind::LocalPath;
                }
            }
            if (a.srcKind == LaunchSrcKind::None) a.src.clear();
        }
    }

    if (a.hasRect) {
        if (a.rectW < 64 || a.rectW > 8192 || a.rectH < 64 || a.rectH > 8192) {
            a.errors.push_back("rect: width and height must be within [64, 8192]");
            a.hasRect = false;
        } else if (a.rectX < -65536 || a.rectX > 65536 || a.rectY < -65536 || a.rectY > 65536) {
            a.errors.push_back("rect: origin out of range");
            a.hasRect = false;
        }
    }
    if (a.hasVh && !(a.vh > 0.f && a.vh <= 100.f)) {
        a.errors.push_back("vh: must be in (0, 100] metres");
        a.hasVh = false;
    }
    if (a.hasDpr && !(a.dpr > 0.f && a.dpr <= 16.f)) {
        a.warnings.push_back("dpr: implausible value ignored");
        a.hasDpr = false;
    }
    if (!a.type.empty()) {
        bool clean = a.type.size() <= 16;
        for (unsigned char c : a.type) {
            if (!(std::islower(c) || std::isdigit(c) || c == '-')) clean = false;
        }
        if (!clean) {
            a.errors.push_back("type: must be a short lower-case token");
            a.type.clear();
        }
    }
    if (!a.env.empty()) {
        bool clean = a.env.size() <= 16;
        for (unsigned char c : a.env) {
            if (!(std::islower(c) || std::isdigit(c) || c == '-')) clean = false;
        }
        if (!clean) {
            a.warnings.push_back("env: not a short lower-case token, ignored");
            a.env.clear();
        }
    }
    const uint64_t kMinBytes = 1ull << 20, kMaxBytes = 4ull << 30;
    if (a.maxBytes < kMinBytes) a.maxBytes = kMinBytes;
    if (a.maxBytes > kMaxBytes) a.maxBytes = kMaxBytes;
}

} // namespace launch_detail

/*!
 * Parse argv (WITHOUT argv[0]) into a policy-checked LaunchArgs. Tokens are
 * UTF-8. Never throws; check `ok()` and log `errors`/`warnings`.
 */
inline LaunchArgs
ParseLaunchArgs(const std::vector<std::string>& args)
{
    using namespace launch_detail;
    LaunchArgs a;
    bool flagsDone = false;
    for (const std::string& tok : args) {
        if (!flagsDone && tok == "--") {
            flagsDone = true;
            continue;
        }
        if (!flagsDone && IStartsWithAscii(tok, kProtocolPrefix)) {
            if (a.fromProtocol) {
                a.warnings.push_back("cli: second protocol URL ignored");
                continue;
            }
            ParseProtocolUrl(tok, a);
            continue;
        }
        if (!flagsDone && tok.size() >= 2 && tok[0] == '-' && tok[1] == '-') {
            const std::string_view body(tok.data() + 2, tok.size() - 2);
            const size_t eq = body.find('=');
            const std::string_view key = body.substr(0, eq);
            if (eq == std::string_view::npos) {
                if (key == "transparent") a.transparent = true;
                else if (key == "no-cache") a.noCache = true;
                else if (key == "allow-local") a.allowLocal = true;
                else if (key == "src" || key == "rect" || key == "vh" || key == "dpr" ||
                         key == "title" || key == "type" || key == "env" || key == "max-bytes")
                    a.errors.push_back("cli: --" + std::string(key) + " needs =value");
                else a.warnings.push_back("cli: unknown flag '" + tok + "' ignored");
            } else {
                ApplyKeyValue(key, body.substr(eq + 1), a, "cli");
            }
            continue;
        }
        if (a.positionalPath.empty()) {
            a.positionalPath = tok;
        } else {
            a.warnings.push_back("cli: extra positional argument '" + tok + "' ignored");
        }
    }
    ApplyPolicy(a);
    return a;
}

} // namespace dxr

#if defined(_WIN32)

#include <windows.h>
#include <shellapi.h>
#include <stdlib.h> // _putenv_s
#include <cwctype>  // towlower

namespace dxr {

namespace launch_detail {
inline bool
IEqualsAsciiW(std::wstring_view a, const wchar_t* b)
{
    const size_t n = wcslen(b);
    if (a.size() != n) return false;
    for (size_t i = 0; i < n; ++i) {
        if (towlower(a[i]) != towlower(b[i])) return false;
    }
    return true;
}
} // namespace launch_detail

inline std::string
Utf8FromWide(std::wstring_view w)
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

inline std::wstring
WideFromUtf8(std::string_view s)
{
    if (s.empty()) return {};
    const int n = MultiByteToWideChar(CP_UTF8, 0, s.data(), static_cast<int>(s.size()), nullptr, 0);
    std::wstring w(static_cast<size_t>(n > 0 ? n : 0), L'\0');
    if (n > 0) MultiByteToWideChar(CP_UTF8, 0, s.data(), static_cast<int>(s.size()), w.data(), n);
    return w;
}

//! UTF-8 -> the process's narrow code page, for loaders that still `fopen`
//! a `std::string` (lossy for characters the code page cannot represent —
//! exactly today's `lpCmdLine` behaviour, no worse).
inline std::string
NarrowPathForFopen(std::string_view utf8)
{
    const std::wstring w = WideFromUtf8(utf8);
    if (w.empty()) return {};
    const int n = WideCharToMultiByte(CP_ACP, 0, w.data(), static_cast<int>(w.size()), nullptr, 0,
                                      nullptr, nullptr);
    std::string s(static_cast<size_t>(n > 0 ? n : 0), '\0');
    if (n > 0)
        WideCharToMultiByte(CP_ACP, 0, w.data(), static_cast<int>(w.size()), s.data(), n, nullptr,
                            nullptr);
    return s;
}

/*!
 * Make sure an undocked viewer runs its OWN in-process compositor.
 *
 * A protocol handler is launched by the browser process and inherits its
 * environment — and the DisplayXR browser sets `XRT_FORCE_MODE=ipc` for
 * itself. Inherited, that routes the viewer to the service as an IPC
 * client, where it is not the panel owner: the service denies it the panel
 * lease, weaves it only while it is focused (the window flips to 2D whenever
 * the browser holds the panel), the drag phase-snap does not apply, and the
 * transparent present takes a different route. A floating overlay must be a
 * standalone session, so when the launch is a protocol launch or asks for
 * transparency, pin `XRT_FORCE_MODE=native` and clear the other two IPC
 * triggers the runtime honours (`DXR_IPC_FD`, `DISPLAYXR_WORKSPACE_SESSION`).
 *
 * Call BEFORE xrCreateInstance (the runtime DLL snapshots the environment
 * when it loads). Returns true when something inherited was overridden, so
 * the caller can log it. A shell-launched tile (no protocol URL, no
 * `--transparent`) is left alone and keeps its workspace routing.
 */
inline bool
ForceInProcessRuntimeForUndock(const LaunchArgs& a)
{
    if (!(a.fromProtocol || a.transparent)) return false;
    bool overrode = false;
    wchar_t buf[64] = {};
    if (GetEnvironmentVariableW(L"XRT_FORCE_MODE", buf, 64) > 0 && wcscmp(buf, L"native") != 0)
        overrode = true;
    if (GetEnvironmentVariableW(L"DXR_IPC_FD", buf, 64) > 0) overrode = true;
    if (GetEnvironmentVariableW(L"DISPLAYXR_WORKSPACE_SESSION", buf, 64) > 0) overrode = true;
    // Both channels, on purpose. The runtime reads getenv() FIRST and falls back
    // to GetEnvironmentVariableA only when getenv returns NULL. With a dynamic
    // CRT shared between this exe and the runtime DLL, getenv serves the CRT's
    // copy of the environment, snapshotted at PROCESS start — so the inherited
    // "ipc" would still win after a bare SetEnvironmentVariable. _putenv_s
    // updates the CRT copy (and, for a shared CRT, the DLL's view of it);
    // SetEnvironmentVariable updates the Win32 block a static-CRT DLL snapshots
    // when it loads. Verified on the panel box: SetEnvironmentVariable alone left
    // the viewer on the IPC path.
    _putenv_s("XRT_FORCE_MODE", "native");
    _putenv_s("DXR_IPC_FD", "");
    _putenv_s("DISPLAYXR_WORKSPACE_SESSION", "");
    SetEnvironmentVariableW(L"XRT_FORCE_MODE", L"native");
    SetEnvironmentVariableW(L"DXR_IPC_FD", nullptr);
    SetEnvironmentVariableW(L"DISPLAYXR_WORKSPACE_SESSION", nullptr);
    return overrode;
}

namespace launch_detail {

inline bool
InheritedIpcRouting()
{
    wchar_t buf[64] = {};
    if (GetEnvironmentVariableW(L"XRT_FORCE_MODE", buf, 64) > 0 && wcscmp(buf, L"native") != 0)
        return true;
    if (GetEnvironmentVariableW(L"DXR_IPC_FD", buf, 64) > 0) return true;
    if (GetEnvironmentVariableW(L"DISPLAYXR_WORKSPACE_SESSION", buf, 64) > 0) return true;
    return false;
}

//! Copy of this process's environment block with the IPC triggers removed,
//! `XRT_FORCE_MODE=native` and `DXR_UNDOCK_REEXEC=1` added. Double-NUL
//! terminated, ready for CreateProcessW(CREATE_UNICODE_ENVIRONMENT).
inline std::wstring
ScrubbedEnvironmentBlock()
{
    std::wstring out;
    LPWCH env = GetEnvironmentStringsW();
    if (env) {
        for (const wchar_t* p = env; *p; p += wcslen(p) + 1) {
            std::wstring_view kv(p);
            const size_t eq = kv.find(L'=', 1); // "=C:=..." drive entries start with '='
            std::wstring_view key = kv.substr(0, eq);
            if (IEqualsAsciiW(key, L"XRT_FORCE_MODE") || IEqualsAsciiW(key, L"DXR_IPC_FD") ||
                IEqualsAsciiW(key, L"DISPLAYXR_WORKSPACE_SESSION") || IEqualsAsciiW(key, L"DXR_UNDOCK_REEXEC"))
                continue;
            out.append(kv);
            out.push_back(L'\0');
        }
        FreeEnvironmentStringsW(env);
    }
    out.append(L"XRT_FORCE_MODE=native");
    out.push_back(L'\0');
    out.append(L"DXR_UNDOCK_REEXEC=1");
    out.push_back(L'\0');
    out.push_back(L'\0');
    return out;
}

} // namespace launch_detail

/*!
 * If this undock launch inherited IPC routing (the browser's
 * `XRT_FORCE_MODE=ipc`, a workspace session, an adopted service socket),
 * re-launch this exe ONCE with a scrubbed environment and return true so the
 * caller exits. Setting variables in place is not enough: the runtime DLL
 * links the dynamic CRT, whose environment copy is snapshotted at process
 * start, and its getenv() is consulted before the Win32 block — verified on
 * the panel box, where an in-place override still produced an IPC client.
 * A child that starts with the clean block has no such copy to be stale.
 * Loop-guarded by `DXR_UNDOCK_REEXEC`. Call before creating any window.
 */
inline bool
ReexecWithCleanRuntimeEnvIfNeeded(const LaunchArgs& a)
{
    using namespace launch_detail;
    if (!(a.fromProtocol || a.transparent)) return false;
    if (!InheritedIpcRouting()) return false;
    wchar_t guard[8] = {};
    if (GetEnvironmentVariableW(L"DXR_UNDOCK_REEXEC", guard, 8) > 0) return false;

    std::wstring env = ScrubbedEnvironmentBlock();
    std::wstring cmd = GetCommandLineW();
    wchar_t exe[MAX_PATH * 2] = {};
    GetModuleFileNameW(nullptr, exe, static_cast<DWORD>(sizeof(exe) / sizeof(exe[0])));
    STARTUPINFOW si{};
    si.cb = sizeof(si);
    PROCESS_INFORMATION pi{};
    const BOOL ok = CreateProcessW(exe, cmd.data(), nullptr, nullptr, FALSE, CREATE_UNICODE_ENVIRONMENT,
                                   env.data(), nullptr, &si, &pi);
    if (!ok) return false; // fall through and run here; the in-place override is the fallback
    CloseHandle(pi.hThread);
    CloseHandle(pi.hProcess);
    return true;
}

/*!
 * Parse this process's real command line (`GetCommandLineW`, so non-ASCII
 * paths survive, unlike WinMain's ANSI `lpCmdLine`). Skips argv[0].
 */
inline LaunchArgs
ParseLaunchArgsFromCommandLine()
{
    std::vector<std::string> args;
    int argc = 0;
    LPWSTR* argv = CommandLineToArgvW(GetCommandLineW(), &argc);
    if (argv) {
        for (int i = 1; i < argc; ++i) args.push_back(Utf8FromWide(argv[i]));
        LocalFree(argv);
    }
    return ParseLaunchArgs(args);
}

} // namespace dxr

#endif // _WIN32
