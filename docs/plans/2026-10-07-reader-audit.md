# Reader audit repairs implementation plan

**Goal:** Ship the scoped X4 Pro fixes from the October 7 handoff, preserving PDF, Plan, Articles, settings, and reading progress.

**Architecture:** Integrate the named upstream patches in dependency order on a fork-only branch. Keep one editor in this repository. Add small local repairs at the existing library, activity, HTTP, and PDF boundaries; retain the current TXT-based PDF reader.

**Tech stack:** C++20, Arduino/ESP-IDF, FreeInk SDK, PlatformIO, CMake/Ninja host GoogleTest suites, Python build scripts.

## 1. Upstream library and reader fixes (F1, F2, F4, F5)

- Inspect each named patch, apply batch A in handoff order with `10d0aa1e` after `4b17a7bb`, and resolve the SDK at `13418e0`.
- Apply batch B preserving the PDF include, PDF browser filter, fork font support, and reader lifecycle. Exclude `e6af0c95`, `004c4892`, and `43a33582`.
- Mark the library dirty after each published article and before a USB-session restart. Current evidence: `LibraryListActivity.cpp:90`, `ArticleSyncActivity.cpp:250`, and USB activity exit/restart paths.
- Verification: baseline and updated host suites, patch integrity, x4pro compilation. Commit each upstream fix separately with provenance and Lore trailers; push only to origin.

## 2. Local behavior and release identity (F3, F6–F10, F13)

- Add SHA-stamped `x4pro-evan` release flags and stamp debug `x4pro`; remove the stock OTA entry. Evidence: `platformio.ini:289`, `scripts/git_branch.py:82`, `SettingsActivity.cpp:108`.
- Protect Plan tab/page rebuilds and all newly touched render-visible state with RenderLock. Evidence: `PlanActivity.cpp:326`; never wait for rendering while holding its lock.
- Hide redundant OPDS navigation when the single configured server is the Articles inbox; keep row counts, index mappings, rendering, and tap selection aligned. Evidence: `HomeActivity.h:36`, `HomeActivity.cpp:120,320`.
- Replace compulsory Plan/Articles restarts on X4 Pro with Wi-Fi teardown and an internal-heap recovery threshold; retain conservative behavior on constrained boards. Evidence: `PlanActivity.cpp:82`, `ArticleSyncActivity.cpp:76`, existing TLS heap floors in `HttpDownloader.h`.
- Pump cancellation during HTTP connection/header/body waits for Plan and Articles. Reuse the existing HTTP transport and input mapping; avoid additional application threads. Evidence: `HttpDownloader.cpp:151–239`, `ArticleSyncActivity.cpp:135`, `PlanActivity.cpp:468`.
- Propagate PDF sink write failures and recover unusable xref offsets through object scanning. Evidence: `PdfTextExtractor.cpp:95`, `PdfDocument.cpp:139,612`. Write failing host regressions before these repairs.
- Verification: meaningful version, library, HTTP cancellation, menu, PDF write-failure, and xref regressions; existing font/parser suites; both firmware profiles; repository formatting wrapper and relevant static analysis.

## 3. Delivery and device checks (F11, F12, F14, F15)

- Deliver uniquely SHA-named debug and personal release binaries with build evidence and a checked repair ledger.
- If the reader reconnects, read logs to identify its controller/LUT and collect render times; never invent hardware measurements.
- Physical acceptance requires 20 EPUB page turns, double-click light/single-click sleep, rapid Plan tabs, five refresh/exit cycles, library refresh after USB/Articles, and no new crash report.
- No SD deletion or full upstream merge: F14 explicitly requires approval and F15 is a separate PDF migration project. Additive Spotlight exclusion is allowed if the SD mounts.
- Mark host-tested repairs separately from outstanding physical verification. Firmware builds and host tests cannot prove display appearance, actual panel variant, page-turn speed, or TLS stability across device cycles.

## Progress

- [x] Baseline host tests
- [x] Upstream integration and SDK
- [x] Local repairs and regressions
- [x] Final verification: 369 host tests, both firmware profiles, 66-file formatting check, and targeted cppcheck passed
- [x] Firmware copied to card and verified by SHA-256; fork publication follows the delivery commit
- [ ] Physical checks (reader mounted in USB Drive mode; no serial port)
