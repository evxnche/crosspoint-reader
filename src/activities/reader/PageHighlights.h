#pragma once

#include <EpdFontFamily.h>
#include <Epub/Page.h>

#include <cstdint>
#include <vector>

#include "HighlightEntry.h"

class GfxRenderer;

// Word geometry and the highlighter wash shared by the reader page and the
// hold-and-slide selection screen.
namespace PageHighlights {

struct Word {
  int16_t x;
  int16_t y;
  int16_t width;  // 0 until measured
  uint16_t row;
  uint32_t offset;  // TextBlock::wordVisibleOffset
  const char* text;  // points into the Page's TextBlock arena
  EpdFontFamily::Style style;
  bool selectable;  // has a letter or digit (not a lone dash, bullet...)
};

// Every word on the page in layout order, with its screen position.
void collectWords(const Page& page, int marginLeft, int marginTop, int ascender, std::vector<Word>& out);

// Fills in width for the words whose `marked` flag is set (all words when
// `marked` is empty), loading the SD font's advances for them in one batch.
void measureWords(GfxRenderer& renderer, int fontId, std::vector<Word>& words, const std::vector<bool>& marked = {});

// Washes each row run of consecutive marked words in light gray. Only adds
// black pixels, so it can go over text that is already drawn.
void shadeMarked(const GfxRenderer& renderer, const std::vector<Word>& words, const std::vector<bool>& marked,
                 int lineHeight);

// Reader helper: washes the words of `page` that fall inside a highlight of
// spine `spineIndex`. A no-op (no measuring) when none do.
void shadePage(GfxRenderer& renderer, const Page& page, int fontId, int marginLeft, int marginTop,
               uint16_t spineIndex, const std::vector<HighlightEntry>& highlights);

}  // namespace PageHighlights
