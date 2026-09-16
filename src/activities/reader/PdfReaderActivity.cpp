#include "PdfReaderActivity.h"

#include <Arduino.h>
#include <GfxRenderer.h>
#include <I18n.h>
#include <Logging.h>
#include <Memory.h>

#include "components/UITheme.h"

namespace {
constexpr const char* TAG = "PRS";
// Long enough for the message to be read before the activity closes.
constexpr TickType_t MESSAGE_DWELL_MS = 2000;
}  // namespace

bool PdfReaderActivity::onExtractProgress(void* ctx, const size_t pageIndex, const size_t pageCount) {
  auto* self = static_cast<PdfReaderActivity*>(ctx);
  if (pageCount == 0) return true;

  const int percent = static_cast<int>(pageIndex * 100 / pageCount);
  // E-ink refreshes are expensive; only redraw when the number actually moves.
  if (percent == self->lastProgressPercent) return true;
  self->lastProgressPercent = percent;
  GUI.fillPopupProgress(self->renderer, self->progressPopup, percent);
  return true;
}

bool PdfReaderActivity::loadBook() {
  pdf = makeUniqueNoThrow<Pdf>(bookPath, "/.crosspoint");
  if (!pdf) {
    LOG_ERR(TAG, "OOM: Pdf");
    return false;
  }
  if (!pdf->load()) {
    LOG_ERR(TAG, "Failed to open PDF");
    return false;
  }

  if (!pdf->isExtracted()) {
    // Parsing a few hundred pages takes long enough that a bare screen looks
    // like a crash; show the popup and drive its bar from the extractor.
    progressPopup = GUI.drawPopup(renderer, tr(STR_PDF_READING));
    lastProgressPercent = -1;
    if (!pdf->extract(&onExtractProgress, this)) {
      LOG_ERR(TAG, "Extraction failed");
      GUI.drawPopup(renderer, tr(STR_PDF_FAILED));
      vTaskDelay(pdMS_TO_TICKS(MESSAGE_DWELL_MS));
      return false;
    }
  }

  // Every page empty means the file is page images with no text layer. There is
  // nothing to reflow, and silently opening a blank book would be worse.
  if (pdf->totalPages() > 0 && pdf->emptyPages() >= pdf->totalPages()) {
    LOG_ERR(TAG, "No text layer in %s", bookPath.c_str());
    GUI.drawPopup(renderer, tr(STR_PDF_NO_TEXT));
    vTaskDelay(pdMS_TO_TICKS(MESSAGE_DWELL_MS));
    return false;
  }

  // From here the extracted text is an ordinary book. Its cache lives under the
  // extracted file's own hash, so progress and bookmarks survive re-extraction
  // only when the source is unchanged - which is the behaviour we want.
  txt = makeUniqueNoThrow<Txt>(pdf->getTextPath(), "/.crosspoint");
  if (!txt) {
    LOG_ERR(TAG, "OOM: Txt");
    return false;
  }
  if (!txt->load()) {
    LOG_ERR(TAG, "Extracted text missing");
    return false;
  }
  txt->setDisplayTitle(pdf->getTitle());
  txt->setupCacheDir();
  return true;
}

std::string PdfReaderActivity::getBookThumbBmpPath() const {
  if (!pdf || !pdf->generateCoverBmp()) return "";
  return pdf->getCoverBmpPath();
}
