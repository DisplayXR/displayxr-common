// Copyright 2026, The DisplayXR Project
// SPDX-License-Identifier: BSL-1.0
/*!
 * @file
 * @brief  Pure parser for this process's window in the window-geometry
 *         publisher's `GetWindows` payload (dxr_wl_placement's
 *         get_own_geometry). No D-Bus, no compositor.
 *
 * The payload is `{"version":…,"layout_mode":…,"windows":[{…},…]}` and the
 * spec (docs/specs/runtime/wayland-window-geometry.md) fixes its keys, NOT
 * their order. The GNOME extension writes each window `pid` first and its
 * monitor afterwards; a KWin publisher writes them sorted (runtime#1928). So
 * this walks the structure — the top-level object, the `windows` array, each
 * window object, its `monitor` object — and looks every key up at its own
 * depth, inside its own object. It used to search substrings from the first
 * `"pid":N,`, which assumed the GNOME order and dereferenced a null `monitor`
 * when the caller's window was the last one and its monitor came before `pid`.
 *
 * Deliberately minimal: enough JSON to skip any value (strings with escapes,
 * nested objects and arrays) and read numbers, booleans and the one string
 * it needs. Rect integers are truncated toward zero, exactly as the runtime's
 * own consumer (comp_vk_native_wl_geom.c, cJSON valuedouble cast) reads them,
 * so the app and the runtime never disagree on a pixel.
 */
#pragma once

#include "util/u_wayland_layout.h"

#include <cstdint>
#include <cstdlib>
#include <cstring>

namespace dxr_wl_geom_json {

//! This process's window, as get_own_geometry reports it (see
//! DxrWlPlacement::OwnGeometry for each field's meaning).
struct Window {
  int32_t frame[4] = {0, 0, 0, 0};
  int32_t buffer[4] = {0, 0, 0, 0};
  int32_t monitor[4] = {0, 0, 0, 0};
  double monitor_scale = 0.0;
  double device_scale = 0.0;
  int layout_mode = 0;
  bool move_sync = false;
};

namespace detail {

inline const char *skip_ws(const char *p, const char *end) {
  while (p < end && (*p == ' ' || *p == '\t' || *p == '\n' || *p == '\r')) {
    p++;
  }
  return p;
}

//! Past the end of the string whose opening quote is at @p p; nullptr if
//! unterminated.
inline const char *skip_string(const char *p, const char *end) {
  for (p++; p < end; p++) {
    if (*p == '\\') {
      p++;
    } else if (*p == '"') {
      return p + 1;
    }
  }
  return nullptr;
}

//! Past the end of the value starting at @p p; nullptr if malformed.
inline const char *skip_value(const char *p, const char *end) {
  p = skip_ws(p, end);
  if (p >= end) {
    return nullptr;
  }
  if (*p == '"') {
    return skip_string(p, end);
  }
  if (*p == '{' || *p == '[') {
    int depth = 0;
    while (p < end) {
      if (*p == '"') {
        p = skip_string(p, end);
        if (p == nullptr) {
          return nullptr;
        }
        continue;
      }
      if (*p == '{' || *p == '[') {
        depth++;
      } else if (*p == '}' || *p == ']') {
        if (--depth == 0) {
          return p + 1;
        }
      }
      p++;
    }
    return nullptr;
  }
  // Number, true, false or null: up to the next delimiter.
  while (p < end && *p != ',' && *p != '}' && *p != ']' && *p != ' ' &&
         *p != '\n' && *p != '\r' && *p != '\t') {
    p++;
  }
  return p;
}

//! The value of @p key directly inside the object at @p obj ('{'), or
//! nullptr. Nested objects' keys never match.
inline const char *find_key(const char *obj, const char *end, const char *key,
                            const char **value_end = nullptr) {
  if (obj == nullptr || obj >= end || *obj != '{') {
    return nullptr;
  }
  const size_t klen = strlen(key);
  const char *p = obj + 1;
  for (;;) {
    p = skip_ws(p, end);
    if (p >= end || *p == '}' || *p != '"') {
      return nullptr;
    }
    const char *kstart = p + 1;
    const char *kend = skip_string(p, end);
    if (kend == nullptr) {
      return nullptr;
    }
    const bool match =
        (size_t)(kend - 1 - kstart) == klen && strncmp(kstart, key, klen) == 0;
    p = skip_ws(kend, end);
    if (p >= end || *p != ':') {
      return nullptr;
    }
    const char *v = skip_ws(p + 1, end);
    const char *vend = skip_value(v, end);
    if (vend == nullptr) {
      return nullptr;
    }
    if (match) {
      if (value_end != nullptr) {
        *value_end = vend;
      }
      return v;
    }
    p = skip_ws(vend, end);
    if (p < end && *p == ',') {
      p++;
    }
  }
}

inline bool read_num(const char *v, const char *end, double *out) {
  if (v == nullptr || v >= end) {
    return false;
  }
  char *e = nullptr;
  const double d = strtod(v, &e);
  if (e == v || e > end) {
    return false;
  }
  *out = d;
  return true;
}

inline bool num_key(const char *obj, const char *end, const char *key,
                    double *out) {
  return read_num(find_key(obj, end, key), end, out);
}

//! `"key":[a,b,c,d]` directly inside @p obj.
inline bool int4_key(const char *obj, const char *end, const char *key,
                     int32_t out[4]) {
  const char *aend = nullptr;
  const char *p = find_key(obj, end, key, &aend);
  if (p == nullptr || *p != '[') {
    return false;
  }
  p++;
  for (int i = 0; i < 4; i++) {
    p = skip_ws(p, aend);
    double d = 0.0;
    if (!read_num(p, aend, &d)) {
      return false;
    }
    out[i] = (int32_t)d;
    p = skip_value(p, aend);
    if (p == nullptr) {
      return false;
    }
    p = skip_ws(p, aend);
    if (i < 3) {
      if (p >= aend || *p != ',') {
        return false;
      }
      p++;
    }
  }
  return true;
}

inline bool bool_key(const char *obj, const char *end, const char *key) {
  const char *v = find_key(obj, end, key);
  return v != nullptr && end - v >= 4 && strncmp(v, "true", 4) == 0;
}

} // namespace detail

/*!
 * Fill @p out from the first window in @p json whose `pid` is @p pid.
 * False when the payload is malformed, has no such window, or that window
 * lacks frame, buffer or a monitor with a positive scale.
 */
inline bool parse_own_window(const char *json, long pid, Window *out) {
  using namespace detail;
  if (json == nullptr || out == nullptr) {
    return false;
  }
  const char *end = json + strlen(json);
  const char *root = skip_ws(json, end);

  const char *wend = nullptr;
  const char *w = find_key(root, end, "windows", &wend);
  if (w == nullptr || *w != '[') {
    return false;
  }
  for (const char *p = skip_ws(w + 1, wend); p < wend && *p != ']';) {
    const char *oend = skip_value(p, wend);
    if (oend == nullptr) {
      return false;
    }
    double wpid = 0.0;
    if (*p == '{' && num_key(p, oend, "pid", &wpid) && (long)wpid == pid) {
      Window g;
      const char *mend = nullptr;
      const char *mon = find_key(p, oend, "monitor", &mend);
      double mx = 0.0, my = 0.0, mw = 0.0, mh = 0.0;
      if (!int4_key(p, oend, "frame", g.frame) ||
          !int4_key(p, oend, "buffer", g.buffer) || mon == nullptr ||
          *mon != '{' || !num_key(mon, mend, "x", &mx) ||
          !num_key(mon, mend, "y", &my) || !num_key(mon, mend, "w", &mw) ||
          !num_key(mon, mend, "h", &mh) ||
          !num_key(mon, mend, "scale", &g.monitor_scale) ||
          !(g.monitor_scale > 0.0)) {
        return false;
      }
      g.monitor[0] = (int32_t)mx;
      g.monitor[1] = (int32_t)my;
      g.monitor[2] = (int32_t)mw;
      g.monitor[3] = (int32_t)mh;
      // Extension v11: device px per stage px, inside the monitor.
      if (!num_key(mon, mend, "device_scale", &g.device_scale) ||
          !(g.device_scale > 0.0)) {
        g.device_scale = 0.0;
      }
      // Extension v11, top level.
      const char *lm_end = nullptr;
      const char *lm = find_key(root, end, "layout_mode", &lm_end);
      if (lm != nullptr && *lm == '"' && lm_end - lm >= 2) {
        char buf[16];
        const size_t n = (size_t)(lm_end - lm - 2);
        if (n < sizeof(buf)) {
          memcpy(buf, lm + 1, n);
          buf[n] = '\0';
          g.layout_mode = (int)u_wl_layout_mode_from_string(buf);
        }
      }
      // Extension v9+ (runtime#1748).
      g.move_sync = bool_key(p, oend, "move_sync");
      *out = g;
      return true;
    }
    p = skip_ws(oend, wend);
    if (p < wend && *p == ',') {
      p = skip_ws(p + 1, wend);
    }
  }
  return false;
}

} // namespace dxr_wl_geom_json
