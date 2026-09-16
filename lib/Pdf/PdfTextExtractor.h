#pragma once

#include <cstddef>
#include <memory>
#include <string>

#include "PdfDocument.h"
#include "PdfFontMap.h"

// Turns a PDF's page content streams into reflowable UTF-8 prose.
//
// A PDF page is a set of glyphs at absolute positions; it has no notion of a
// paragraph, or even of a line. This extractor rebuilds that structure from
// geometry: runs at the same baseline join into a line, consecutive lines join
// into a paragraph, and a paragraph is emitted as ONE long line so the reader's
// own layout engine can wrap it to whatever font and margin the user picked.
// That is what makes a PDF readable on a 4.3" screen — reflowing the text
// rather than shrinking the page.
class PdfTextExtractor {
 public:
  // Called between pages so the UI can show progress. Return false to abort.
  using ProgressFn = bool (*)(void* ctx, size_t pageIndex, size_t pageCount);

  struct Options {
    // Drop short runs pinned to the top and bottom margins: running heads and
    // page numbers, which reflow into the middle of a sentence otherwise.
    bool stripHeadersFooters = true;
    // Rejoin a word split across a line break by a hyphen.
    bool dehyphenate = true;
  };

  PdfTextExtractor() = default;

  // Extract every page of `doc` to `outPath`. Returns false only on an
  // unrecoverable error; a page that fails to decode is skipped.
  bool run(PdfDocument& doc, const std::string& outPath, const Options& options, ProgressFn progress, void* ctx);

  // Number of pages that yielded no text at all. A document where this equals
  // the page count is a scan, and needs OCR rather than extraction.
  [[nodiscard]] size_t emptyPages() const { return emptyPageCount; }
  [[nodiscard]] size_t charactersWritten() const { return charCount; }

 private:
  // 2D affine transform, laid out as PDF writes it: [a b c d e f].
  struct Matrix {
    double a = 1, b = 0, c = 0, d = 1, e = 0, f = 0;
  };

  // `m` applied first, then `n`.
  static Matrix multiply(const Matrix& m, const Matrix& n);

  struct TextState {
    Matrix tm;   // text matrix
    Matrix tlm;  // text line matrix
    double fontSize = 0;
    double charSpacing = 0;
    double wordSpacing = 0;
    double horizScale = 1;
    double leading = 0;
    double rise = 0;
    const PdfFontMap* font = nullptr;
  };

  bool runContent(PdfDocument& doc, PdfSource& content, const PdfObject& resources, const Matrix& ctm, int depth);
  void showString(const std::string& bytes, double tjAdjust);
  void placeRun(const std::string& text, double startX, double y, double scale, double endX);
  void flushLine();
  void flushParagraph();
  // Writes the pending paragraph out; flushParagraph() closes the line first.
  void emitParagraph();
  void startLine(const std::string& text, double startX, double y, double scale, double endX);
  // Ends the paragraph before the line about to start, when the geometry says
  // the two do not belong together.
  void breakBeforeLine(double startX, double y, double scale);
  void emit(const std::string& s);

  const PdfFontMap* fontFor(PdfDocument& doc, const PdfObject& resources, const std::string& name);

  // Output.
  class PdfSink* out = nullptr;  // defined in PdfFilters.h
  Options opts;
  size_t charCount = 0;
  size_t emptyPageCount = 0;

  // Assembly state.
  std::string paragraph;
  std::string line;
  double lineStartX = 0;
  double lineY = 0;
  double lineScale = 0;
  double lineEndX = 0;
  bool haveLine = false;
  // Geometry of the last line committed to the paragraph. Kept separately from
  // the in-progress line because a line is often closed (by ET) before the next
  // one starts, leaving nothing in progress to compare against.
  double lastLineY = 0;
  double lastLineScale = 0;
  double lastLineStartX = 0;
  bool haveLastLine = false;
  double paragraphIndent = 0;
  // Line-to-line drop measured inside the current paragraph; 0 until known.
  double paragraphLeading = 0;
  bool pageJustStarted = false;
  bool pageProducedText = false;

  // Page geometry, for the header/footer band.
  double pageTop = 0;
  double pageBottom = 0;

  // Graphics/text state for the stream being interpreted.
  TextState ts;
  Matrix currentCtm;

  // Font cache keyed by object number, shared across pages: decoding a
  // /ToUnicode CMap per page would dominate extraction time.
  struct FontSlot {
    uint32_t objNum = 0;
    bool valid = false;
    std::unique_ptr<PdfFontMap> map;
  };
  static constexpr size_t FONT_SLOTS = 8;
  FontSlot fonts[FONT_SLOTS];
  size_t fontNext = 0;

  std::string scratchDir;
};
