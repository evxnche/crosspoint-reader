# Open a selected Reader Inbox article after download

Firmware source: `295680b0` on `fix/reader-audit-2026-10-07`.

## Observed behavior and cause

The reader downloaded the selected article, showed Loading, and returned to the Reader Inbox catalog. The success branch in `OpdsBookBrowserActivity::downloadBook()` explicitly fetched the catalog again without requesting a reader activity.

## Repair

On X4 Pro, a successful download now queues the existing reader activity for the saved EPUB. The OPDS activity switches Wi-Fi off during that handoff, so its normal exit reboot cannot replace the pending book with the Home screen. If allocating the reader fails, the saved download is retained and the Memory error screen is shown. Failed/cancelled downloads do not open a book. Other boards retain the existing download-only flow and exit recovery.

The change uses one existing reader-screen allocation through `ReaderActivity::create()`. It adds no dependency or parallel task and does not alter saved settings, article conversion, remembered-book policy, or reading progress handling.

## Changed files

| File | Purpose |
| --- | --- |
| `src/activities/browser/OpdsBookBrowserActivity.cpp` | Queue the downloaded book and suppress the catalog reboot only for that handoff. |
| `src/activities/browser/OpdsBookBrowserActivity.h` | Record the pending book handoff. |
| `test/test_opds_download_open.py` | Compile the production methods with fake device I/O; check six transition scenarios using the real filename policy. |
| `.github/workflows/ci.yml` | Run the new regression in the host-test CI job. |

## Verification

The new regression failed on the original success branch because no book was opened. It passes after the repair, covering root/configured-folder destinations, download failure, cancellation, allocation failure, and constrained-board behavior. The existing 369 host tests also pass.

Both x4pro and x4pro-evan firmware builds pass and fit the 6,553,600-byte slot. Targeted cppcheck reports no high-severity defects. The CI workflow parses and includes the regression. The personal release (5,522,144 bytes) was copied to the card and its SHA-256 verified: `ca81f10073ee830afe6710c293fdbaed84ba3a4935e905983d353b4c21865395`. Physical verification remains pending after installation.

## Reader check

Install the new firmware ending `295680b0.bin`. Open Reader Inbox and select one article. After the download finishes, its first page should replace the catalog. Confirm that the reader stays in the book and that Back returns normally.
