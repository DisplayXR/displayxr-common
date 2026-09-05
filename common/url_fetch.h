// Copyright 2026, DisplayXR
// SPDX-License-Identifier: BSL-1.0
/*!
 * @file
 * @brief  Download an asset URL into a per-user cache so a viewer can load it
 *         like a local file (the `--src=<url>` half of launch_args.h).
 *
 * Windows implementation on WinHTTP (no COM, no IE zone policy, no modal
 * proxy/auth dialogs popping under a topmost transparent window — all of
 * which urlmon's URLDownloadToCacheFile can do). The cache file is named by
 * the SHA-1 of the requested URL, never by anything in the URL's path, so a
 * hostile `src=.../../../x` has no traversal surface. A cache hit skips the
 * network entirely, which is what makes the second undock of the same asset
 * instant.
 *
 * The pure helpers (extension resolution) are platform-neutral and unit
 * tested; the fetch itself is `_WIN32`-only.
 */

#pragma once

#include <cstdint>
#include <functional>
#include <string>
#include <string_view>
#include <vector>

namespace dxr {

/*!
 * Decide the cache file's extension. Order: (1) the extension of the URL's
 * PATH (query/fragment stripped) if it is in `allowed`; (2) the Content-Type
 * map; (3) a magic-byte sniff of the first bytes. The result is always one of
 * `allowed` (with its leading dot) or empty = "unresolvable, refuse". A
 * `&ext=` query parameter is deliberately never consulted.
 */
std::string ResolveAssetExtension(std::string_view url, std::string_view contentType,
                                  const std::vector<uint8_t>& head,
                                  const std::vector<std::string>& allowed);

//! Extension (with dot, lower-case) of a URL's path component; empty if none.
std::string UrlPathExtension(std::string_view url);

#if defined(_WIN32)

struct UrlFetchOptions {
    //! Absolute cache directory; created if missing. See DefaultCacheDir().
    std::wstring cacheDir;
    //! Extensions the caller can load, with dots, lower-case (".glb", ".spz").
    std::vector<std::string> allowedExtensions;
    //! Hard cap on the body; enforced on Content-Length AND in the read loop.
    uint64_t maxBytes = 256ull << 20;
    //! Re-download even when the cache already has the file.
    bool noCache = false;
    //! Called on the fetching thread; `total` is 0 when the server sent no
    //! Content-Length. Keep it cheap (post to your own toast state).
    std::function<void(uint64_t done, uint64_t total)> progress;
    //! Policy re-check applied to the FINAL URL after redirects (pass the same
    //! predicate launch_args.h applied to the requested one). Null = allow.
    std::function<bool(const std::string& finalUrl)> urlAllowed;
    //! Per-request timeouts, milliseconds.
    uint32_t connectTimeoutMs = 10000;
    uint32_t receiveTimeoutMs = 60000;
};

struct UrlFetchResult {
    bool ok = false;
    bool fromCache = false;
    std::wstring path;     //!< absolute cache file path on success
    std::string finalUrl;  //!< URL after redirects (empty on a cache hit)
    uint64_t bytes = 0;
    std::string error;     //!< human-readable, safe to toast
};

//! `%LOCALAPPDATA%\DisplayXR\<appDirName>\cache`.
std::wstring DefaultCacheDir(const wchar_t* appDirName);

//! Synchronous. Call from a worker thread; never from the render thread.
UrlFetchResult FetchUrlToCache(const std::string& url, const UrlFetchOptions& opts);

#endif // _WIN32

} // namespace dxr
