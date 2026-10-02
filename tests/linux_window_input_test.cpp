// Copyright 2026, The DisplayXR Project
// SPDX-License-Identifier: BSL-1.0
/*!
 * @file
 * @brief  The pure halves of DxrLinuxWindow's text input and file drops
 *         (common/linux/dxr_input_text.h, dxr_fd_drain.h). No display.
 *
 * 1. utf8_encode / utf8_first round-trip at every length boundary.
 * 2. accept_text: printable text only — controls, DEL, C1 and anything typed
 *    with Ctrl or Alt held carry no text (Enter / Backspace / Ctrl+K stay
 *    keysym-only).
 * 3. us_keysym_char: the no-keymap fallback, shifted and not.
 * 4. parse_uri_list: file URIs to decoded paths (host forms, %XX, CRLF,
 *    comments, blanks), other URIs verbatim, order kept.
 * 5. FdDrain over a real pipe: a payload split across writes is read whole
 *    and never blocks; the size cap and the time cap fail it.
 */
#include "dxr_fd_drain.h"
#include "dxr_input_text.h"

#include <chrono>
#include <cstdio>
#include <cstring>
#include <string>
#include <thread>

#include <unistd.h>

namespace I = dxr_input;

static int g_fail = 0;
#define CHECK(c, msg)                                                                                                  \
	do {                                                                                                           \
		if (!(c)) {                                                                                            \
			std::fprintf(stderr, "FAIL: %s (%s:%d)\n", msg, __FILE__, __LINE__);                           \
			g_fail++;                                                                                      \
		}                                                                                                      \
	} while (0)

static void
test_utf8()
{
	const uint32_t cps[] = {0x41, 0x7F, 0x80, 0xE9, 0x7FF, 0x800, 0x20AC, 0xFFFD, 0x10000, 0x1F600, 0x10FFFF};
	for (uint32_t cp : cps) {
		char b[8];
		const size_t n = I::utf8_encode(cp, b);
		CHECK(n >= 1 && n <= 4 && std::strlen(b) == n, "utf8_encode length");
		CHECK(I::utf8_first(b) == cp, "utf8 round-trip");
	}
	char b[8];
	CHECK(I::utf8_encode(0xD800, b) == 0 && b[0] == '\0', "surrogate rejected");
	CHECK(I::utf8_encode(0x110000, b) == 0, "beyond U+10FFFF rejected");
	CHECK(std::strcmp((I::utf8_encode(0xE9, b), b), "\xC3\xA9") == 0, "e-acute bytes");
	CHECK(I::utf8_first("\xC3") == 0, "truncated sequence");
}

static void
test_accept_text()
{
	char out[8];
	CHECK(I::accept_text("a", false, out, sizeof(out)) && std::strcmp(out, "a") == 0, "plain letter");
	CHECK(I::accept_text(" ", false, out, sizeof(out)) && std::strcmp(out, " ") == 0, "space is text");
	CHECK(I::accept_text("\xC3\xA9", false, out, sizeof(out)) && std::strcmp(out, "\xC3\xA9") == 0, "e-acute");
	CHECK(I::accept_text("\xE2\x82\xAC", false, out, sizeof(out)), "euro sign");
	CHECK(!I::accept_text("\r", false, out, sizeof(out)) && out[0] == '\0', "Enter carries no text");
	CHECK(!I::accept_text("\b", false, out, sizeof(out)), "Backspace carries no text");
	CHECK(!I::accept_text("\t", false, out, sizeof(out)), "Tab carries no text");
	CHECK(!I::accept_text("\x1b", false, out, sizeof(out)), "Escape carries no text");
	CHECK(!I::accept_text("\x7f", false, out, sizeof(out)), "DEL carries no text");
	CHECK(!I::accept_text("\xC2\x85", false, out, sizeof(out)), "C1 control carries no text");
	CHECK(!I::accept_text("\x0b", false, out, sizeof(out)), "Ctrl+K's control char carries no text");
	CHECK(!I::accept_text("k", true, out, sizeof(out)) && out[0] == '\0', "Ctrl/Alt + letter carries no text");
	CHECK(!I::accept_text("", false, out, sizeof(out)), "empty");
	CHECK(!I::accept_text(nullptr, false, out, sizeof(out)), "null");
	CHECK(!I::accept_text("abcdefgh", false, out, sizeof(out)), "longer than the field");
}

static void
test_us_table()
{
	CHECK(I::us_keysym_char('a', false) == 'a' && I::us_keysym_char('a', true) == 'A', "letters");
	CHECK(I::us_keysym_char('1', true) == '!' && I::us_keysym_char(';', true) == ':', "shifted symbols");
	CHECK(I::us_keysym_char('/', true) == '?' && I::us_keysym_char('/', false) == '/', "slash");
	CHECK(I::us_keysym_char(0xFF0D /* XK_Return */, false) == 0, "non-ASCII keysym has no char");
	CHECK(I::us_keysym_char(0x1B, false) == 0, "control keysym has no char");
}

static void
test_uri_list()
{
	const std::string in = "# a comment\r\n"
	                       "file:///home/u/My%20Clip.mp4\r\n"
	                       "\r\n"
	                       "file://localhost/tmp/a%2Bb.png\r\n"
	                       "file://box.local/srv/x.jpg\n"
	                       "https://example.com/v.mp4?a=1%20b\n"
	                       "  file:///trailing%C3%A9.mpo  \n"
	                       "file:///no-newline";
	const auto v = I::parse_uri_list(in.data(), in.size());
	CHECK(v.size() == 6, "six entries (comment + blank skipped)");
	if (v.size() == 6) {
		CHECK(v[0] == "/home/u/My Clip.mp4", "file:/// decoded");
		CHECK(v[1] == "/tmp/a+b.png", "file://localhost/ host dropped");
		CHECK(v[2] == "/srv/x.jpg", "file://host/ host dropped");
		CHECK(v[3] == "https://example.com/v.mp4?a=1%20b", "other URIs verbatim (not decoded)");
		CHECK(v[4] == "/trailing\xC3\xA9.mpo", "whitespace trimmed, UTF-8 decoded");
		CHECK(v[5] == "/no-newline", "last line without a newline");
	}
	CHECK(I::parse_uri_list("", 0).empty(), "empty payload");
	const char nul[] = "file:///a\0garbage";
	const auto w = I::parse_uri_list(nul, sizeof(nul) - 1);
	CHECK(w.size() >= 1 && w[0] == "/a", "NUL ends a line");
	CHECK(I::percent_decode("%zz%4") == "%zz%4", "malformed escapes kept");
}

static void
test_fd_drain()
{
	// A payload written in two parts, with the reader stepping in between.
	{
		int fds[2];
		CHECK(pipe(fds) == 0, "pipe");
		I::FdDrain d;
		d.start(fds[0], 1 << 20, 3000);
		CHECK(d.step() == I::FdDrain::State::Reading, "nothing yet: Reading, and it did not block");
		CHECK(write(fds[1], "file:///a\n", 10) == 10, "write 1");
		CHECK(d.step() == I::FdDrain::State::Reading, "partial: still Reading");
		CHECK(write(fds[1], "file:///b\n", 10) == 10, "write 2");
		close(fds[1]);
		CHECK(d.step() == I::FdDrain::State::Done, "EOF: Done");
		CHECK(d.data() == "file:///a\nfile:///b\n", "whole payload");
		d.reset();
		CHECK(d.state() == I::FdDrain::State::Idle, "reset: Idle");
	}
	// Size cap.
	{
		int fds[2];
		CHECK(pipe(fds) == 0, "pipe");
		I::FdDrain d;
		d.start(fds[0], 16, 3000);
		CHECK(write(fds[1], "0123456789abcdefXYZ", 19) == 19, "write");
		CHECK(d.step() == I::FdDrain::State::Failed && std::strcmp(d.why(), "size cap") == 0, "size cap fails");
		close(fds[1]);
	}
	// Time cap: a source that never closes its end.
	{
		int fds[2];
		CHECK(pipe(fds) == 0, "pipe");
		I::FdDrain d;
		d.start(fds[0], 1 << 20, 50);
		const auto t0 = std::chrono::steady_clock::now();
		I::FdDrain::State s = d.step();
		CHECK(s == I::FdDrain::State::Reading, "open pipe: Reading");
		CHECK(std::chrono::steady_clock::now() - t0 < std::chrono::milliseconds(20), "step() never waits");
		std::this_thread::sleep_for(std::chrono::milliseconds(80));
		s = d.step();
		CHECK(s == I::FdDrain::State::Failed && std::strcmp(d.why(), "time cap") == 0, "time cap fails");
		close(fds[1]);
	}
}

int
main()
{
	test_utf8();
	test_accept_text();
	test_us_table();
	test_uri_list();
	test_fd_drain();
	if (g_fail != 0) {
		std::fprintf(stderr, "linux_window_input_test: %d failure(s)\n", g_fail);
		return 1;
	}
	std::printf("linux_window_input_test: OK\n");
	return 0;
}
