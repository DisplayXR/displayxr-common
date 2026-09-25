// Copyright 2026, DisplayXR
// SPDX-License-Identifier: BSL-1.0
/*!
 * @file
 * @brief  Live test of the desktop-Linux url_fetch.h fetcher (libcurl, dlopen)
 *         against tests/url_fetch_server.py on loopback.
 *
 * Opt-in: DXR_URL_FETCH_TEST_PORT=<port of a running url_fetch_server.py>.
 * Without it the test only checks DefaultCacheDir and exits 0, so a bare
 * `ctest` never needs a server.
 */

#include "url_fetch.h"

#include <sys/stat.h>
#include <unistd.h>

#include <cstdio>
#include <cstdlib>
#include <string>

static int g_failures = 0;

#define CHECK(cond, msg)                                                                 \
    do {                                                                                 \
        if (!(cond)) {                                                                   \
            std::fprintf(stderr, "FAIL: %s (%s:%d)\n", msg, __FILE__, __LINE__);         \
            g_failures++;                                                                \
        }                                                                                \
    } while (0)

static std::string g_base;
static std::string g_cache;

static dxr::UrlFetchResult
Fetch(const std::string& path, uint64_t maxBytes = 1 << 20, bool noCache = false,
      std::function<bool(const std::string&)> allowed = nullptr)
{
    dxr::UrlFetchOptions o;
    o.cacheDir = g_cache;
    o.allowedExtensions = {".glb", ".gltf"};
    o.maxBytes = maxBytes;
    o.noCache = noCache;
    o.urlAllowed = std::move(allowed);
    const dxr::UrlFetchResult r = dxr::FetchUrlToCache(g_base + path, o);
    std::printf("  %-18s ok=%d cache=%d bytes=%llu path=%s error='%s'\n", path.c_str(), (int)r.ok,
                (int)r.fromCache, (unsigned long long)r.bytes, r.path.c_str(), r.error.c_str());
    return r;
}

int
main()
{
    setenv("XDG_CACHE_HOME", "/tmp/dxr-xdg-test", 1);
    CHECK(dxr::DefaultCacheDir("modelviewer") == "/tmp/dxr-xdg-test/displayxr/modelviewer", "XDG_CACHE_HOME");
    setenv("XDG_CACHE_HOME", "relative/ignored", 1);
    setenv("HOME", "/home/someone", 1);
    CHECK(dxr::DefaultCacheDir("gaussiansplat") == "/home/someone/.cache/displayxr/gaussiansplat",
          "relative XDG_CACHE_HOME falls back to ~/.cache");

    const char* port = std::getenv("DXR_URL_FETCH_TEST_PORT");
    if (port == nullptr) {
        std::printf("live fetch checks skipped (run tests/url_fetch_server.py and set DXR_URL_FETCH_TEST_PORT)\n");
        return g_failures ? 1 : 0;
    }
    g_base = std::string("http://127.0.0.1:") + port;
    char tmpl[] = "/tmp/dxr-url-fetch-XXXXXX";
    g_cache = std::string(mkdtemp(tmpl)) + "/nested/cache";

    dxr::UrlFetchResult r = Fetch("/a.glb");
    CHECK(r.ok && !r.fromCache && r.bytes == 64, "download");
    CHECK(r.path == g_cache + "/" + dxr::Sha1Hex(g_base + "/a.glb") + ".glb", "cache file is <sha1(url)>.glb");
    r = Fetch("/a.glb");
    CHECK(r.ok && r.fromCache, "second fetch is a cache hit");
    r = Fetch("/a.glb", 1 << 20, /*noCache=*/true);
    CHECK(r.ok && !r.fromCache, "--no-cache re-downloads");
    r = Fetch("/noext");
    CHECK(r.ok && r.path.size() > 4 && r.path.substr(r.path.size() - 4) == ".glb", "extension from magic bytes");
    r = Fetch("/redir");
    CHECK(r.ok && r.finalUrl == g_base + "/redir-target.glb", "redirect followed, final URL reported");
    r = Fetch("/redir", 1 << 20, true, [](const std::string& u) { return u.find("redir-target") == std::string::npos; });
    CHECK(!r.ok && r.error == "redirected to a URL the policy does not allow", "final URL re-checked by policy");
    r = Fetch("/missing.glb");
    CHECK(!r.ok && r.error == "HTTP 404", "404 reported as HTTP 404");
    r = Fetch("/big.glb", 1000);
    CHECK(!r.ok && r.error == "asset larger than the download cap", "Content-Length over the cap refused");
    r = Fetch("/text");
    CHECK(!r.ok && r.error == "could not determine the asset type", "unresolvable type refused");
    struct stat st;
    CHECK(stat((g_cache + "/" + dxr::Sha1Hex(g_base + "/text") + ".part").c_str(), &st) != 0,
          "no .part left behind");

    if (g_failures) {
        std::fprintf(stderr, "url_fetch_test: %d failure(s)\n", g_failures);
        return 1;
    }
    std::printf("url_fetch_test: all checks passed\n");
    return 0;
}
