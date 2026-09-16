#pragma once

#include <cstdint>
#include <string>
#include <vector>

#include "PdfDocument.h"
#include "PdfTypes.h"

// Character-code to Unicode mapping for one PDF font, plus the glyph widths the
// extractor needs to tell a word gap from a kerning nudge.
//
// A PDF font's codes mean nothing on their own: the same byte is a different
// character in every font on the page. Resolution order is /ToUnicode (exact,
// what the producer intended), then a base encoding with /Differences applied,
// then Latin-1 as the last resort.
class PdfFontMap {
 public:
  // `fontDict` is a resolved /Font entry. Never fails outright: a font it
  // cannot interpret still decodes as Latin-1 so the page yields readable ASCII.
  void load(PdfDocument& doc, const PdfObject& fontDict, const std::string& scratchDir);

  // True when codes are two bytes wide (Type0 with a two-byte CMap).
  [[nodiscard]] bool isTwoByte() const { return twoByte; }

  // Append the UTF-8 for `code` to `out`. Unmapped codes append nothing.
  void appendUtf8(uint32_t code, std::string& out) const;

  // Glyph advance in thousandths of an em.
  [[nodiscard]] int widthOf(uint32_t code) const;

 private:
  void parseToUnicode(PdfDocument& doc, const PdfObject& streamObj, const std::string& scratchDir);
  void applyBaseEncoding(PdfDocument& doc, const PdfObject& fontDict);
  void loadSimpleWidths(PdfDocument& doc, const PdfObject& fontDict);
  void loadCidWidths(PdfDocument& doc, const PdfObject& descendant);

  void setMapping(uint32_t code, const std::string& utf8);

  struct Mapping {
    uint32_t code;
    uint32_t offset;  // index into `blob`
    uint16_t len;
  };

  std::vector<Mapping> mappings;  // sorted by code
  std::string blob;               // UTF-8 bytes, referenced by Mapping

  struct WidthRange {
    uint32_t first;
    uint32_t last;
    int width;
  };
  std::vector<WidthRange> widths;  // sorted by first
  int defaultWidth = 500;

  bool twoByte = false;
};
