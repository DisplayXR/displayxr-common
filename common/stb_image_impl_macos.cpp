// Copyright 2026, The DisplayXR Project and its contributors
// SPDX-License-Identifier: BSL-1.0
/*!
 * @file
 * @brief  stb_image (read) implementation translation unit for macOS targets.
 *
 * App code that decodes textures with stb_image takes the *declarations only*
 * (#include "stb_image.h", optionally TINYGLTF_NO_STB_IMAGE), expecting the
 * implementation to be linked in from elsewhere. On Windows that
 * implementation lives in common/d3d11_renderer.cpp (Windows-gated). That file
 * never builds on Apple, so macOS executables need this dedicated TU to supply
 * the read symbols (stbi_load_from_memory, stbi_image_free,
 * stbi_failure_reason, …).
 *
 * Mirrors how common/atlas_capture_macos.mm supplies
 * STB_IMAGE_WRITE_IMPLEMENTATION on macOS. Exactly one TU per link target may
 * define STB_IMAGE_IMPLEMENTATION; on macOS this is the only one (the Windows
 * definer, d3d11_renderer.cpp, is gated out). Consumers must NOT define
 * STB_IMAGE_IMPLEMENTATION in their own TUs when linking displayxr::common
 * on Apple.
 */
#define STB_IMAGE_IMPLEMENTATION
#include "stb_image.h"
