# X4 Pro audit repair delivery — October 7, 2026

Firmware source revision: `55bc1f21` on `fix/reader-audit-2026-10-07`.

The fork retains PDF, Plan, Articles, SD fonts, settings, book identities, and EPUB reading progress. Hardware acceptance is separate from the build and host-test results below.

## Repairs

| Item | Implemented behavior | Verification boundary |
| --- | --- | --- |
| F1 | Library reconciles a dirty index; article publication marks it dirty; USB marks it before raw card ownership so the marker survives disconnects and reboots. The Library refresh action is available. | New Articles/Pushed Books reconciliation regression; initial marker added to the mounted card. Real reader display still to check. |
| F2 | Power taps wait for the light double-click window before sleeping. | Audited upstream repair integrated and compiled. Physical power/light check pending. |
| F3 | Debug and personal builds show branch and SHA. Personal builds use LOG_LEVEL=1 and omit the serial boot wait. | Version injection and resolved profile flags verified; binaries checked below. |
| F4 | All 17 batch A repairs plus the library SDK bump are integrated. | Host font, glyph, parser, and navigation coverage; debug build passes. Page-turn timing and sleep/wake still to measure on the reader. |
| F5 | All seven requested batch B repairs are ported to the fork. PDF inclusion/filtering, filename normalization, release-based navigation, and buffered book metadata remain intact. | Font-cache, kerning, ligature, and paragraph regressions pass. The unrelated FreeInkFont migration was excluded; existing nothrow array ownership is retained. |
| F6 | Plan tab and page changes rebuild under the render lock. | Source/lifecycle review and compilation. Rapid switching on hardware pending. |
| F7 | The redundant OPDS row is hidden when Articles already covers the single configured inbox. | Production row counts and forward/reverse selection mappings compiled and checked for all OPDS/Articles combinations. Physical taps still to check. |
| F8 | Stock Check for Updates is hidden. SD firmware installation remains available. | Settings source reviewed and compiled. |
| F9 | Plan/Articles stop Wi-Fi on exit. X4 Pro reboots only when the internal heap is below the existing TLS floors; constrained boards retain conservative reboot recovery. | Uses existing HAL heap measurements and TLS floors. Five repeated device refresh/exit cycles remain untested. |
| F10 | Back is polled during HTTP response-header/body waits; cancellation discards partial list/Plan responses and partial downloads. Successful sync offers Open Articles. | Five tests run the real SDK HTTP parser with simulated sockets, including stalled headers and partial bodies. DNS/initial socket/TLS connection is still synchronous in the SDK; cancellation resumes when that call returns. |
| F13 | PDF buffered write failures stay failed; failed page-content staging and cancelled extraction do not publish a cache; wrong xref object offsets trigger one scan/retry. | Three new fault-injection regressions reproduce the original failures and pass with the repairs. Existing abort regression now requires failure instead of permitting incomplete cache publication. |

## Verification

- Host tests: **369/369 passed**, rebuilt after resolving snapshot timestamps.
- Additional checks: production Home menu mapping matrix; upstream list viewport regression; Python version-injection checks.
- Debug firmware: **x4pro build passed**; application fits the 6,553,600-byte slot.
- **Personal release build passed**; targeted cppcheck passed with no high-severity defects.
- Formatting: all 66 changed C/C++ files checked with the repository wrapper; two extra blank lines removed. Firmware compilation was not repeated for these whitespace-only edits.

## Firmware files

- Install: `crosspoint-x4pro-evan-2026-10-07-55bc1f21.bin` — 5,521,968 bytes, copied to the card and verified against the local file.
- Debug: `crosspoint-x4pro-debug-2026-10-07-55bc1f21.bin` — 5,562,816 bytes, available locally for serial timing logs.
- Both contain `1.6.5-dev-fix/reader-audit-2026-10-07-55bc1f21` and fit the 6,553,600-byte firmware slot.
- Release SHA-256: `da696605e824bb9e6fd4f64e8c2093030c00adf944f8dca83d461244e0bc4507`.

On the reader, choose **Settings → System → SD Card Firmware Update** and select the file ending **55bc1f21.bin**. The firmware has been staged; it has not been flashed or tested on the device.

## Card and device

The mounted card has no crash report. Its library index and relevant state/configuration files were backed up outside the repository under `../outputs/reader-audit-2026-10-07/card-backup/`. The card received two additive files: `/.crosspoint/library.dirty` and `/.metadata_never_index`. Existing books, settings, caches, and the v4 firmware were kept.

F11: SDK `13418e0` contains recognition for UC8279 LUT variants 0x03 and 0x67. The actual reader panel/LUT has not been read: USB Drive mode exposes the card but no serial port.

F12: No before/after page-render timing is claimed. The debug profile retains the render-time logging needed for AA on/off and JetBrainsMono/Noto Serif comparisons.

F14: Spotlight exclusion is added. Deleting old Agent Limits or original-firmware cache files remains deferred under the handoff's explicit permission rule.

F15: A full upstream merge/PDF migration is excluded as the separate project identified in the handoff. The selected fixes preserve the existing TXT-based PDF reading pipeline. The deadlocking close-book patches and the missing-file patch were not imported.

## Physical acceptance after flashing

1. Open Library and verify Articles/Pushed Books appear; copy a book over USB and check that it appears after reopening Library.
2. Double-click Power for the light, then single-click for sleep; open an EPUB with JetBrainsMono/AA and turn 20 pages, sleep, and wake.
3. Switch Plan tabs rapidly for 30 seconds; refresh and exit Plan/Articles five times; press Back during a stalled refresh and open Articles after a successful sync.
4. Read the boot controller/LUT log and compare render timings across AA and font choices; check that no new crash report appears.

## Changed files

```text
USER_GUIDE.md
docs/file-formats.md
docs/plans/2026-10-07-reader-audit.md
docs/reader-audit-2026-10-07.md
freeink-sdk
lib/EpdFont/SdCardFont.cpp
lib/EpdFont/SdCardFont.h
lib/Epub/Epub/BookMetadataCache.cpp
lib/Epub/Epub/ParsedText.cpp
lib/Epub/Epub/ParsedText.h
lib/Epub/Epub/Section.cpp
lib/Epub/Epub/parsers/ChapterHtmlSlimParser.cpp
lib/Epub/Epub/parsers/ChapterHtmlSlimParser.h
lib/GfxRenderer/FontCacheManager.h
lib/GfxRenderer/GfxRenderer.cpp
lib/GfxRenderer/GfxRenderer.h
lib/GfxRenderer/GlyphBitmap.h
lib/I18n/translations/english.yaml
lib/LibraryIndex/LibraryBuilder.cpp
lib/LibraryIndex/LibraryBuilder.h
lib/Pdf/PdfDocument.cpp
lib/Pdf/PdfFilters.cpp
lib/Pdf/PdfFilters.h
lib/Pdf/PdfTextExtractor.cpp
lib/hal/HalPowerManager.cpp
platformio.ini
scripts/git_branch.py
scripts/patch_sdfat.py
scripts/sdfat_patches/0001-invalidate-failed-cache-fill.patch
scripts/sdfat_patches/0002-allow-separate-fat-cache-override.patch
src/RecentBooksStore.cpp
src/SilentRestart.h
src/activities/ActivityManager.cpp
src/activities/RenderLock.h
src/activities/UiListActivity.cpp
src/activities/browser/ArticleSyncActivity.cpp
src/activities/browser/ArticleSyncActivity.h
src/activities/browser/OpdsBookBrowserActivity.cpp
src/activities/home/FileBrowserActivity.cpp
src/activities/home/FileBrowserActivity.h
src/activities/home/HomeActivity.cpp
src/activities/library/LibraryListActivity.cpp
src/activities/library/LibraryListActivity.h
src/activities/network/CrossPointWebServerActivity.cpp
src/activities/network/CrossPointWebServerActivity.h
src/activities/network/UsbDriveActivity.cpp
src/activities/plan/PlanActivity.cpp
src/activities/reader/EpubReaderActivity.cpp
src/activities/reader/EpubReaderActivity.h
src/activities/reader/ReaderActivity.cpp
src/activities/reader/ReaderActivity.h
src/activities/reader/TxtReaderActivity.cpp
src/activities/reader/XtcReaderActivity.cpp
src/activities/settings/SettingsActivity.cpp
src/activities/settings/SettingsActivity.h
src/activities/settings/TextSettingsActivity.cpp
src/activities/settings/TextSettingsActivity.h
src/components/icons/listIcons.h
src/components/icons/listIcons.manifest
src/main.cpp
src/network/CrossPointWebServer.cpp
src/network/CrossPointWebServer.h
src/network/HttpDownloader.cpp
src/network/HttpDownloader.h
src/util/ButtonNavigator.h
test/CMakeLists.txt
test/chapter_html_slim_parser/ChapterHtmlSlimParserTest.cpp
test/chapter_html_slim_parser/stubs/GfxRenderer.h
test/glyph_bitmap/CMakeLists.txt
test/glyph_bitmap/GlyphBitmapTest.cpp
test/http_downloader/CMakeLists.txt
test/http_downloader/HttpDownloaderTest.cpp
test/http_downloader/stubs/Arduino.h
test/http_downloader/stubs/Client.h
test/http_downloader/stubs/WiFiClient.h
test/http_downloader/stubs/base64.h
test/http_downloader/stubs/esp_wifi.h
test/library_builder/LibraryBuilderTest.cpp
test/pdf_extract/PdfExtractTest.cpp
test/pdf_extract/stubs/HalStorage.h
test/sd_card_font/CMakeLists.txt
test/sd_card_font/SdCardFontTest.cpp
test/test_list_viewport.py
```
