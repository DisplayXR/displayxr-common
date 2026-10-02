// Copyright 2026, The DisplayXR Project
// SPDX-License-Identifier: BSL-1.0
/*!
 * @file
 * @brief  Pure helpers behind DxrLinuxWindow's text input and file drops:
 *         UTF-8 encoding, the "is this typed text" rule, and the
 *         text/uri-list parser. No window-system calls, so unit tested
 *         (tests/linux_window_input_test.cpp).
 */
#pragma once

#include <cstddef>
#include <cstdint>
#include <cstring>
#include <string>
#include <vector>

namespace dxr_input {

//! Encode one Unicode code point as UTF-8 into @p out (NUL-terminated,
//! at least 5 bytes). Returns the byte count (0 for an invalid code point).
inline size_t
utf8_encode(uint32_t cp, char *out)
{
	if (cp >= 0xD800 && cp <= 0xDFFF) {
		out[0] = '\0';
		return 0;
	}
	if (cp < 0x80) {
		out[0] = (char)cp;
		out[1] = '\0';
		return 1;
	}
	if (cp < 0x800) {
		out[0] = (char)(0xC0 | (cp >> 6));
		out[1] = (char)(0x80 | (cp & 0x3F));
		out[2] = '\0';
		return 2;
	}
	if (cp < 0x10000) {
		out[0] = (char)(0xE0 | (cp >> 12));
		out[1] = (char)(0x80 | ((cp >> 6) & 0x3F));
		out[2] = (char)(0x80 | (cp & 0x3F));
		out[3] = '\0';
		return 3;
	}
	if (cp < 0x110000) {
		out[0] = (char)(0xF0 | (cp >> 18));
		out[1] = (char)(0x80 | ((cp >> 12) & 0x3F));
		out[2] = (char)(0x80 | ((cp >> 6) & 0x3F));
		out[3] = (char)(0x80 | (cp & 0x3F));
		out[4] = '\0';
		return 4;
	}
	out[0] = '\0';
	return 0;
}

//! First code point of a UTF-8 string (0 for empty / malformed).
inline uint32_t
utf8_first(const char *s)
{
	const auto *u = reinterpret_cast<const unsigned char *>(s);
	if (u == nullptr || u[0] == 0) {
		return 0;
	}
	if (u[0] < 0x80) {
		return u[0];
	}
	int n = 0;
	uint32_t cp = 0;
	if ((u[0] & 0xE0) == 0xC0) {
		n = 1;
		cp = u[0] & 0x1F;
	} else if ((u[0] & 0xF0) == 0xE0) {
		n = 2;
		cp = u[0] & 0x0F;
	} else if ((u[0] & 0xF8) == 0xF0) {
		n = 3;
		cp = u[0] & 0x07;
	} else {
		return 0;
	}
	for (int i = 1; i <= n; i++) {
		if ((u[i] & 0xC0) != 0x80) {
			return 0;
		}
		cp = (cp << 6) | (u[i] & 0x3F);
	}
	return cp;
}

/*!
 * THE text rule. A key press carries text only when it TYPES something:
 * a printable character (no C0/C1 control, no DEL — so Enter, Backspace,
 * Tab and Escape stay keysym-only), with neither Ctrl nor Alt held (so
 * Ctrl+K is a shortcut, not a 'k'). AltGr is not Alt: layouts type with it.
 *
 * @param utf8      what the keymap produced for the press
 * @param ctrl_or_alt Ctrl or Alt is held
 * @param out       receives @p utf8 when it is text, else ""; size >= 8
 */
inline bool
accept_text(const char *utf8, bool ctrl_or_alt, char *out, size_t out_size)
{
	out[0] = '\0';
	if (ctrl_or_alt || utf8 == nullptr) {
		return false;
	}
	const uint32_t cp = utf8_first(utf8);
	if (cp < 0x20 || cp == 0x7F || (cp >= 0x80 && cp < 0xA0)) {
		return false;
	}
	const size_t n = strlen(utf8);
	if (n == 0 || n + 1 > out_size) {
		return false;
	}
	memcpy(out, utf8, n + 1);
	return true;
}

/*!
 * US-layout text for a level-0 keysym (the fallback when no keymap can
 * say what a key types: a build without libxkbcommon on Wayland). ASCII
 * keysyms ARE their characters; Shift picks the shifted symbol.
 */
inline uint32_t
us_keysym_char(uint32_t keysym, bool shift)
{
	if (keysym < 0x20 || keysym > 0x7E) {
		return 0;
	}
	const char c = (char)keysym;
	if (!shift) {
		return (uint32_t)(unsigned char)c;
	}
	if (c >= 'a' && c <= 'z') {
		return (uint32_t)(c - 'a' + 'A');
	}
	static const char kFrom[] = "`1234567890-=[]\\;',./";
	static const char kTo[] = "~!@#$%^&*()_+{}|:\"<>?";
	for (size_t i = 0; kFrom[i] != '\0'; i++) {
		if (kFrom[i] == c) {
			return (uint32_t)(unsigned char)kTo[i];
		}
	}
	return (uint32_t)(unsigned char)c;
}

inline int
hex_digit(char c)
{
	if (c >= '0' && c <= '9') {
		return c - '0';
	}
	if (c >= 'a' && c <= 'f') {
		return c - 'a' + 10;
	}
	if (c >= 'A' && c <= 'F') {
		return c - 'A' + 10;
	}
	return -1;
}

//! %XX-decode (RFC 3986). A malformed escape is kept verbatim.
inline std::string
percent_decode(const std::string &s)
{
	std::string out;
	out.reserve(s.size());
	for (size_t i = 0; i < s.size(); i++) {
		if (s[i] == '%' && i + 2 < s.size()) {
			const int hi = hex_digit(s[i + 1]), lo = hex_digit(s[i + 2]);
			if (hi >= 0 && lo >= 0) {
				out.push_back((char)((hi << 4) | lo));
				i += 2;
				continue;
			}
		}
		out.push_back(s[i]);
	}
	return out;
}

/*!
 * Parse a text/uri-list (RFC 2483) payload into what a drop hands the app:
 * a LOCAL PATH for each `file:` URI (percent-decoded; a `file://host/...`
 * host part is dropped — "localhost" and the machine's own name are the
 * only hosts a desktop source ever writes), and any other URI verbatim
 * (an app may open an http(s) URL). Comment lines (`#`) and blank lines
 * are skipped; CRLF and bare LF both end a line. Order is preserved.
 */
inline std::vector<std::string>
parse_uri_list(const char *data, size_t size)
{
	std::vector<std::string> out;
	size_t i = 0;
	while (i < size) {
		size_t j = i;
		while (j < size && data[j] != '\n' && data[j] != '\0') {
			j++;
		}
		std::string line(data + i, j - i);
		i = j + 1;
		while (!line.empty() && (line.back() == '\r' || line.back() == ' ' || line.back() == '\t')) {
			line.pop_back();
		}
		size_t k = 0;
		while (k < line.size() && (line[k] == ' ' || line[k] == '\t')) {
			k++;
		}
		line.erase(0, k);
		if (line.empty() || line[0] == '#') {
			continue;
		}
		if (line.compare(0, 5, "file:") == 0) {
			std::string rest = line.substr(5);
			if (rest.compare(0, 2, "//") == 0) {
				const size_t slash = rest.find('/', 2);
				rest = slash == std::string::npos ? std::string() : rest.substr(slash);
			}
			const std::string path = percent_decode(rest);
			if (!path.empty()) {
				out.push_back(path);
			}
			continue;
		}
		out.push_back(line);
	}
	return out;
}

} // namespace dxr_input
