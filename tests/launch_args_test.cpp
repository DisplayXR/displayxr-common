// Copyright 2026, DisplayXR
// SPDX-License-Identifier: BSL-1.0
/*!
 * @file
 * @brief  Unit tests for launch_args.h + the pure url_fetch.h helpers.
 *
 * Platform-neutral: the parser and the policy pass run on every CI runner.
 * The security negatives here are the contract — a change that makes one of
 * them pass is a regression, not a relaxation.
 */

#include <cstdio>
#include <string>
#include <vector>

#include "launch_args.h"
#include "url_fetch.h"

static int g_failures = 0;

#define CHECK(cond, msg)                                                                 \
    do {                                                                                 \
        if (!(cond)) {                                                                   \
            std::fprintf(stderr, "FAIL: %s (%s:%d)\n", msg, __FILE__, __LINE__);         \
            g_failures++;                                                                \
        }                                                                                \
    } while (0)

using dxr::LaunchArgs;
using dxr::LaunchSrcKind;
using dxr::ParseLaunchArgs;

static LaunchArgs
P(std::vector<std::string> v)
{
    return ParseLaunchArgs(v);
}

static void
test_cli_positional()
{
    LaunchArgs a = P({"C:\\models\\a b.glb"});
    CHECK(a.ok(), "positional path parses");
    CHECK(a.positionalPath == "C:\\models\\a b.glb", "positional path kept verbatim");
    CHECK(!a.transparent && !a.hasRect && !a.fromProtocol, "defaults off");

    a = P({"--transparent", "--rect=100,-20,900,700", "--vh=0.25", "--title=Water Bottle",
           "--src=C:\\x\\y.glb", "--type=Model", "--dpr=2.5", "--max-bytes=1048576"});
    CHECK(a.ok(), "full flag set parses");
    CHECK(a.transparent, "--transparent");
    CHECK(a.hasRect && a.rectX == 100 && a.rectY == -20 && a.rectW == 900 && a.rectH == 700, "--rect");
    CHECK(a.hasVh && a.vh > 0.249f && a.vh < 0.251f, "--vh");
    CHECK(a.title == "Water Bottle", "--title");
    CHECK(a.srcKind == LaunchSrcKind::LocalPath && a.src == "C:\\x\\y.glb", "--src local path allowed on CLI");
    CHECK(a.type == "model", "--type lower-cased");
    CHECK(a.hasDpr, "--dpr");
    CHECK(a.maxBytes == 1048576, "--max-bytes");

    a = P({"--", "--not-a-flag.glb"});
    CHECK(a.ok() && a.positionalPath == "--not-a-flag.glb", "-- ends flag parsing");

    a = P({"--rect"});
    CHECK(!a.ok(), "--rect without =value is an error");

    a = P({"--bogus"});
    CHECK(a.ok() && a.warnings.size() == 1, "unknown flag is a warning, not an error");

    a = P({"--src=file:///C:/exports/part.stl"});
    CHECK(a.ok() && a.srcKind == LaunchSrcKind::LocalPath && a.src == "C:/exports/part.stl",
          "file: URL on the CLI becomes a local path");
}

static void
test_protocol_happy_path()
{
    LaunchArgs a = P({"displayxr-view://open?src=http%3A%2F%2Flocalhost%2Fassets%2Fmodels%2FWaterBottle.glb"
                      "&type=model&rect=200,200,800,800&vh=0.2&dpr=2.5&title=Water%20Bottle&v=1"});
    CHECK(a.ok(), "protocol happy path parses");
    CHECK(a.fromProtocol && a.protocolVersion == 1, "fromProtocol + v=1");
    CHECK(a.srcKind == LaunchSrcKind::Url, "loopback http is a URL src");
    CHECK(a.src == "http://localhost/assets/models/WaterBottle.glb", "src percent-decoded");
    CHECK(a.hasRect && a.rectW == 800, "rect from protocol");
    CHECK(a.title == "Water Bottle", "title percent-decoded");
    CHECK(a.type == "model", "type");
    CHECK(!a.protocolUrl.empty(), "raw URL retained for forwarding");

    a = P({"DisplayXR-View://OPEN/?src=https%3A%2F%2Fcdn.example.com%2Fm.glb&v=1"});
    CHECK(a.ok() && a.srcKind == LaunchSrcKind::Url, "scheme/verb are case-insensitive, trailing slash ok");

    a = P({"displayxr-view://open?src=https%3A%2F%2Fcdn.example.com%2Fm.glb"});
    CHECK(a.ok() && a.protocolVersion == 1 && !a.warnings.empty(), "missing v= assumed 1 with a warning");

    a = P({"displayxr-view://open?src=https%3A%2F%2Fcdn.example.com%2Fm.glb&v=1&future=x"});
    CHECK(a.ok() && !a.warnings.empty(), "unknown key is a warning");

    a = P({"displayxr-view://open?src=https%3A%2F%2Fcdn.example.com%2Fm.glb&v=1#frag"});
    CHECK(a.ok() && a.src == "https://cdn.example.com/m.glb", "fragment stripped");

    // Transparent is the protocol's DEFAULT (undock = floating overlay); opt out with =0.
    a = P({"displayxr-view://open?src=https%3A%2F%2Fh%2Fx.glb&v=1"});
    CHECK(a.ok() && a.transparent, "protocol launch is transparent by default");
    a = P({"displayxr-view://open?src=https%3A%2F%2Fh%2Fx.glb&transparent=1&v=1"});
    CHECK(a.ok() && a.transparent, "transparent=1 explicit");
    a = P({"displayxr-view://open?src=https%3A%2F%2Fh%2Fx.glb&transparent=0&v=1"});
    CHECK(a.ok() && !a.transparent, "transparent=0 opts out (framed window)");
    a = P({"displayxr-view://open?src=https%3A%2F%2Fh%2Fx.glb&transparent=maybe&v=1"});
    CHECK(!a.ok(), "transparent=<garbage> is an error");
    a = P({"--rect=1,1,100,100"});
    CHECK(a.ok() && !a.transparent, "CLI stays opt-in (no --transparent => framed)");
}

static void
test_protocol_security()
{
    LaunchArgs a = P({"displayxr-view://open?src=file%3A%2F%2F%2FC%3A%2FWindows%2Fwin.ini&v=1"});
    CHECK(!a.ok() && a.srcKind == LaunchSrcKind::None, "file: via protocol REJECTED");

    a = P({"displayxr-view://open?src=C%3A%5CWindows%5Cwin.ini&v=1"});
    CHECK(!a.ok(), "bare local path via protocol REJECTED");

    a = P({"displayxr-view://open?src=%5C%5Cserver%5Cshare%5Cx.glb&v=1"});
    CHECK(!a.ok(), "UNC via protocol REJECTED");

    a = P({"displayxr-view://open?src=http%3A%2F%2Fevil.example%2Fx.glb&v=1"});
    CHECK(!a.ok(), "non-loopback http REJECTED");

    a = P({"displayxr-view://open?src=http%3A%2F%2F127.0.0.1%3A8080%2Fx.glb&v=1"});
    CHECK(a.ok(), "127.0.0.1 with port is loopback");

    a = P({"displayxr-view://open?src=http%3A%2F%2Flocalhost.evil.example%2Fx.glb&v=1"});
    CHECK(!a.ok(), "localhost.<domain> is NOT loopback");

    a = P({"displayxr-view://open?src=https%3A%2F%2Fuser%40evil.example%2Fx.glb&v=1"});
    CHECK(!a.ok(), "userinfo in authority REJECTED");

    a = P({"displayxr-view://open?src=ftp%3A%2F%2Fh%2Fx.glb&v=1"});
    CHECK(!a.ok(), "other schemes REJECTED");

    a = P({"displayxr-view://open?src=javascript%3Aalert(1)&v=1"});
    CHECK(!a.ok(), "javascript: REJECTED");

    a = P({"displayxr-view://close?v=1"});
    CHECK(!a.ok(), "unknown verb REJECTED");

    a = P({"displayxr-view://open?src=https%3A%2F%2Fh%2Fx.glb&v=2"});
    CHECK(!a.ok(), "future version REJECTED");

    a = P({"displayxr-view://open?src=https%3A%2F%2Fh%2Fx.glb&v=1&title=a%00b%01c"});
    CHECK(a.ok() && a.title == "abc", "control chars stripped from title");

    a = P({"displayxr-view://open?src=https%3A%2F%2Fh%2Fx.glb&v=1&rect=0,0,10,10"});
    CHECK(!a.ok(), "tiny rect REJECTED");

    a = P({"displayxr-view://open?src=https%3A%2F%2Fh%2Fx.glb&v=1&rect=0,0,9000,100"});
    CHECK(!a.ok(), "huge rect REJECTED");

    a = P({"displayxr-view://open?src=https%3A%2F%2Fh%2Fx.glb&v=1&rect=1,2,3"});
    CHECK(!a.ok(), "3-part rect REJECTED");

    a = P({"displayxr-view://open?src=https%3A%2F%2Fh%2Fx.glb&v=1&vh=-1"});
    CHECK(!a.ok(), "negative vh REJECTED");

    a = P({"displayxr-view://open?src=https%3A%2F%2Fh%2Fx.glb&v=1&src=https%3A%2F%2Fh%2Fy.glb%zz"});
    CHECK(!a.ok(), "malformed percent-encoding REJECTED");

    std::string longUrl = "displayxr-view://open?v=1&src=https%3A%2F%2Fh%2F";
    longUrl.append(2100, 'a');
    a = P({longUrl});
    CHECK(!a.ok(), "over-long URL REJECTED");

    a = P({"displayxr-view://open?src=https%3A%2F%2Fh%2Fx.glb&v=1&max-bytes=1"});
    CHECK(a.ok() && a.maxBytes == (1ull << 20), "max-bytes clamped to the 1 MiB floor");

    // A native caller can opt local sources back in — argv only, a page can't set it.
    a = P({"--allow-local", "displayxr-view://open?src=file%3A%2F%2F%2FC%3A%2Fexports%2Fpart.stl&v=1"});
    CHECK(a.ok() && a.srcKind == LaunchSrcKind::LocalPath && a.src == "C:/exports/part.stl",
          "--allow-local admits file: (flag before URL)");
    a = P({"displayxr-view://open?src=C%3A%2Fexports%2Fpart.stl&v=1", "--allow-local"});
    CHECK(a.ok() && a.srcKind == LaunchSrcKind::LocalPath, "--allow-local admits a path (flag after URL)");
}

static void
test_ext_resolution()
{
    const std::vector<std::string> model = {".glb", ".gltf", ".stl", ".obj", ".usdz"};
    const std::vector<std::string> splat = {".ply", ".spz"};
    std::vector<uint8_t> none;

    CHECK(dxr::UrlPathExtension("https://h/a/b/WaterBottle.glb?x=1#f") == ".glb", "path ext ignores query");
    CHECK(dxr::UrlPathExtension("https://h/a/b/noext?ext=glb") == "", "no path ext");
    CHECK(dxr::UrlPathExtension("https://h/a.b/c") == "", "dot in dir is not an ext");

    CHECK(dxr::ResolveAssetExtension("https://h/x.GLB", "", none, model) == ".glb", "path ext wins, lower-cased");
    CHECK(dxr::ResolveAssetExtension("https://h/x?ext=glb", "model/gltf-binary; charset=x", none, model) == ".glb",
          "content-type fallback, never the query");
    std::vector<uint8_t> glbMagic = {'g', 'l', 'T', 'F', 2, 0, 0, 0};
    CHECK(dxr::ResolveAssetExtension("https://h/x", "application/octet-stream", glbMagic, model) == ".glb",
          "magic sniff fallback");
    std::vector<uint8_t> gz = {0x1f, 0x8b, 8, 0};
    CHECK(dxr::ResolveAssetExtension("https://h/x", "", gz, splat) == ".spz", "gzip magic -> spz");
    CHECK(dxr::ResolveAssetExtension("https://h/x.exe", "application/x-msdownload", none, model) == "",
          "disallowed everything -> empty (refuse)");
    CHECK(dxr::ResolveAssetExtension("https://h/x.spz", "", none, model) == "",
          "allowed set is per viewer: spz is not a model");
}

#if defined(_WIN32)
static void
test_force_in_process()
{
    // Simulate the browser's inherited environment.
    SetEnvironmentVariableW(L"XRT_FORCE_MODE", L"ipc");
    SetEnvironmentVariableW(L"DISPLAYXR_WORKSPACE_SESSION", L"1");
    LaunchArgs shellTile = P({"C:\\x\\y.glb"});
    CHECK(!dxr::ForceInProcessRuntimeForUndock(shellTile), "plain launch leaves the env alone");
    wchar_t buf[64] = {};
    CHECK(GetEnvironmentVariableW(L"XRT_FORCE_MODE", buf, 64) > 0 && wcscmp(buf, L"ipc") == 0,
          "plain launch: XRT_FORCE_MODE untouched");

    LaunchArgs proto = P({"displayxr-view://open?src=https%3A%2F%2Fh%2Fx.glb&v=1"});
    CHECK(dxr::ForceInProcessRuntimeForUndock(proto), "protocol launch overrides inherited ipc");
    CHECK(GetEnvironmentVariableW(L"XRT_FORCE_MODE", buf, 64) > 0 && wcscmp(buf, L"native") == 0,
          "protocol launch: XRT_FORCE_MODE=native");
    CHECK(GetEnvironmentVariableW(L"DISPLAYXR_WORKSPACE_SESSION", buf, 64) == 0,
          "protocol launch: workspace-session trigger cleared");
    CHECK(!dxr::ForceInProcessRuntimeForUndock(proto), "second call: nothing left to override");

    SetEnvironmentVariableW(L"XRT_FORCE_MODE", nullptr);
    LaunchArgs cliT = P({"--transparent", "C:\\x\\y.glb"});
    dxr::ForceInProcessRuntimeForUndock(cliT);
    CHECK(GetEnvironmentVariableW(L"XRT_FORCE_MODE", buf, 64) > 0 && wcscmp(buf, L"native") == 0,
          "--transparent on the CLI also pins native");
    SetEnvironmentVariableW(L"XRT_FORCE_MODE", nullptr);
}
#endif

int
main()
{
    test_cli_positional();
    test_protocol_happy_path();
    test_protocol_security();
    test_ext_resolution();
#if defined(_WIN32)
    test_force_in_process();
#endif
    if (g_failures) {
        std::fprintf(stderr, "%d failure(s)\n", g_failures);
        return 1;
    }
    std::printf("launch_args_test: all checks passed\n");
    return 0;
}
