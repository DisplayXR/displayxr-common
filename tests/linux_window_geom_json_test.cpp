// Copyright 2026, The DisplayXR Project
// SPDX-License-Identifier: BSL-1.0
/*!
 * @file
 * @brief  This process's window out of the window-geometry publisher's
 *         payload (common/linux/dxr_wl_geom_json.h). No display, no bus.
 *
 * 1. The GNOME extension's key order (pid first, monitor after).
 * 2. The same payload with every object's keys SORTED, as a KWin publisher
 *    writes it (runtime#1928) — the old substring parser dereferenced a null
 *    monitor here when the caller's window was the last one.
 * 3. A pid that is a prefix of another (12 vs 123), a key name inside a
 *    title string, and a nested object carrying the same key names: none of
 *    them may be read as the caller's.
 * 4. Missing pieces fail cleanly; optional fields default.
 */
#include "dxr_wl_geom_json.h"

#include <cstdio>

static int g_fail = 0;
#define CHECK(c)                                                               \
  do {                                                                         \
    if (!(c)) {                                                                \
      fprintf(stderr, "FAIL %s:%d: %s\n", __FILE__, __LINE__, #c);             \
      g_fail++;                                                                \
    }                                                                          \
  } while (0)

using dxr_wl_geom_json::parse_own_window;
using dxr_wl_geom_json::Window;

static void check_window(const Window &w) {
  CHECK(w.frame[0] == 257 && w.frame[1] == 173 && w.frame[2] == 1280 &&
        w.frame[3] == 766);
  CHECK(w.buffer[0] == 257 && w.buffer[1] == 219 && w.buffer[2] == 1280 &&
        w.buffer[3] == 720);
  CHECK(w.monitor[0] == 0 && w.monitor[1] == 0 && w.monitor[2] == 2304 &&
        w.monitor[3] == 1296);
  CHECK(w.monitor_scale > 1.6666 && w.monitor_scale < 1.6667);
  CHECK(w.device_scale > 1.6666 && w.device_scale < 1.6667);
  CHECK(w.layout_mode == U_WL_LAYOUT_MODE_LOGICAL);
  CHECK(w.move_sync);
}

int main() {
  // 1. GNOME order. Ours (pid 4242) is the LAST window.
  static const char *gnome =
      "{\"version\":12,\"layout_mode\":\"logical\",\"windows\":["
      "{\"pid\":77,\"title\":\"other\",\"frame\":[1,2,3,4],\"buffer\":[1,2,3,4]"
      ","
      "\"monitor\":{\"x\":0,\"y\":0,\"w\":2304,\"h\":1296,\"scale\":1,\"device_"
      "scale\":1},\"move_sync\":false},"
      "{\"pid\":4242,\"title\":\"cube\",\"frame\":[257,173,1280,766],"
      "\"buffer\":[257,219,1280,720],"
      "\"monitor\":{\"x\":0,\"y\":0,\"w\":2304,\"h\":1296,\"scale\":1.6666666,"
      "\"device_scale\":1.6666666},"
      "\"moving\":false,\"move_sync\":true}]}";
  {
    Window w;
    CHECK(parse_own_window(gnome, 4242, &w));
    check_window(w);
  }

  // 2. Sorted keys everywhere (KWin), ours last, whitespace between tokens.
  static const char *sorted =
      "{ \"layout_mode\": \"logical\", \"version\": 12, \"windows\": [\n"
      "  {\"buffer\": [1,2,3,4], \"frame\": [1,2,3,4],"
      " \"monitor\": {\"device_scale\": 1, \"h\": 1296, \"scale\": 1, \"w\": "
      "2304, \"x\": 0, \"y\": 0},"
      " \"move_sync\": false, \"pid\": 77, \"title\": \"other\"},\n"
      "  {\"buffer\": [257, 219, 1280, 720], \"frame\": [257, 173, 1280, 766],"
      " \"monitor\": {\"device_scale\": 1.6666666, \"h\": 1296, \"scale\": "
      "1.6666666, \"w\": 2304, \"x\": 0, "
      "\"y\": 0},"
      " \"move_sync\": true, \"moving\": false, \"pid\": 4242, \"title\": "
      "\"cube\"}\n"
      "] }";
  {
    Window w;
    CHECK(parse_own_window(sorted, 4242, &w));
    check_window(w);
    // The first window is read as itself, not as the next one's fields.
    Window o;
    CHECK(parse_own_window(sorted, 77, &o));
    CHECK(o.frame[0] == 1 && o.buffer[3] == 4 && o.monitor_scale == 1.0 &&
          !o.move_sync);
  }

  // 3. Decoys: pid 123 must not match 12; a title spelling "pid":12 and a
  //    nested object with frame/buffer keys must not be read.
  static const char *decoys =
      "{\"windows\":["
      "{\"pid\":123,\"title\":\"\\\"pid\\\":12,\\\"frame\\\":[9,9,9,9]\","
      "\"frame\":[5,5,5,5],"
      "\"buffer\":[5,5,5,5],\"monitor\":{\"x\":0,\"y\":0,\"w\":10,\"h\":10,"
      "\"scale\":1}},"
      "{\"extra\":{\"frame\":[8,8,8,8],\"pid\":12},\"pid\":12,\"frame\":[6,6,6,"
      "6],\"buffer\":[7,7,7,7],"
      "\"monitor\":{\"x\":3,\"y\":4,\"w\":10,\"h\":10,\"scale\":2}}]}";
  {
    Window w;
    CHECK(parse_own_window(decoys, 12, &w));
    CHECK(w.frame[0] == 6 && w.buffer[0] == 7 && w.monitor[0] == 3 &&
          w.monitor_scale == 2.0);
    CHECK(w.device_scale == 0.0 && w.layout_mode == U_WL_LAYOUT_MODE_UNKNOWN &&
          !w.move_sync);
    CHECK(!parse_own_window(decoys, 1, &w));
  }

  // 4. Failures: no such pid, no monitor, zero scale, truncated, garbage.
  {
    Window w;
    CHECK(!parse_own_window(gnome, 999, &w));
    CHECK(!parse_own_window(
        "{\"windows\":[{\"pid\":1,\"frame\":[1,2,3,4],\"buffer\":[1,2,3,4]}]}",
        1, &w));
    CHECK(!parse_own_window(
        "{\"windows\":[{\"pid\":1,\"frame\":[1,2,3,4],\"buffer\":[1,2,3,4],"
        "\"monitor\":{\"x\":0,\"y\":0,\"w\":1,\"h\":1,\"scale\":0}}]}",
        1, &w));
    CHECK(
        !parse_own_window("{\"windows\":[{\"pid\":1,\"frame\":[1,2,3", 1, &w));
    CHECK(!parse_own_window(
        "{\"windows\":[{\"pid\":1,\"frame\":[1,2,3],\"buffer\":[1,2,3,4],"
        "\"monitor\":{\"x\":0,\"y\":0,\"w\":1,\"h\":1,\"scale\":1}}]}",
        1, &w));
    CHECK(!parse_own_window("", 1, &w));
    CHECK(!parse_own_window("not json", 1, &w));
    CHECK(!parse_own_window(nullptr, 1, &w));
    CHECK(!parse_own_window("{\"windows\":[]}", 1, &w));
  }

  if (g_fail == 0) {
    printf("linux_window_geom_json_test: all checks passed\n");
  }
  return g_fail == 0 ? 0 : 1;
}
