#pragma once

#include <Pdf.h>

#include <memory>
#include <string>

#include "TxtReaderActivity.h"
#include "components/themes/BaseTheme.h"  // Rect

// Reads a PDF as a book.
//
// The document is parsed once into reflowed UTF-8 held beside the file in the
// cache, and everything after that — pagination, progress, fonts, bookmarks —
// is the plain-text reader working on that cache. Reflowing is the whole point:
// a page laid out for A4 is unreadable shrunk to 4.3 inches, while the same
// text wrapped to the user's font is an ordinary book.
class PdfReaderActivity final : public TxtReaderActivity {
 public:
  PdfReaderActivity(GfxRenderer& renderer, MappedInputManager& mappedInput, std::string bookPath,
                    const bool allowFastInitialRefresh)
      : TxtReaderActivity("PdfReader", renderer, mappedInput, std::move(bookPath), allowFastInitialRefresh) {}
  ~PdfReaderActivity() override = default;

 protected:
  bool loadBook() override;
  std::string getBookThumbBmpPath() const override;

 private:
  // Progress callback for the extractor; `ctx` is the activity.
  static bool onExtractProgress(void* ctx, size_t pageIndex, size_t pageCount);

  std::unique_ptr<Pdf> pdf;
  // Popup geometry, kept between progress ticks so the bar redraws in place.
  Rect progressPopup{};
  int lastProgressPercent = -1;
};
