# `extensions/` — vendored DisplayXR OpenXR extension headers

This directory exists for **one reason**: to carry an extension header that the
pinned [`displayxr-extensions`](https://github.com/DisplayXR/displayxr-extensions)
fallback in `CMakeLists.txt` does not have yet, because the runtime PR that
introduces it has not merged.

`openxr/XR_DXR_display_info.h` is vendored **byte-for-byte** from
`displayxr-runtime` `src/external/openxr_includes/openxr/XR_DXR_display_info.h`
at branch `feat/model-e-fixed-view-count`, head `158016f88` — i.e.
`XR_DXR_display_info_SPEC_VERSION` **21**, the snapshot that adds
`XrViewActivityStateDXR` / `XR_TYPE_VIEW_ACTIVITY_STATE_DXR` (ADR-041,
"Model E", runtime [#1533](https://github.com/DisplayXR/displayxr-runtime/pull/1533)).
Do not hand-edit it; the lint workflow's vendor-name guard skips `openxr/`
precisely because these files must match upstream exactly.

## Precedence

The directory is put on the include path **only on the standalone (this repo's
CI) path**, ahead of the fetched `displayxr-extensions` snapshot, so this repo's
own build sees SPEC_VERSION 21. A consumer that sets
`DISPLAYXR_EXTENSIONS_INCLUDE_DIR` keeps supplying its own snapshot and this
directory is not used at all — that precedence rule is unchanged and deliberate
(lib TUs and app TUs must compile against the same `openxr.h`).

A consumer pinned to a pre-21 snapshot is covered by the guarded fallback
definition in `common/view_submission.h`, the same two-stage mechanism
`common/dxr_view_config.h` uses for `PRIMARY_MULTIVIEW_DXR`.

## Retire it

Once runtime #1533 merges to `main`, `displayxr-extensions` auto-syncs within
minutes. Then:

1. bump `GIT_TAG` in the `displayxr_extensions_headers` `FetchContent_Declare`
   to a commit carrying SPEC_VERSION >= 21,
2. delete this directory and the `displayxr_vendored_ext_headers` include entry,
3. leave the fallback in `common/view_submission.h` — it covers consumers, not
   this repo.
