#include "PageHighlights.h"

#include <GfxRenderer.h>

#include <cctype>
#include <string>

namespace {

// Same rule as dictionary word selection: an ASCII letter/digit or a
// non-ASCII codepoint outside General Punctuation (U+2000-U+206F).
bool isSelectableToken(const char* text) {
  for (const uint8_t* p = reinterpret_cast<const uint8_t*>(text); *p != 0; p++) {
    if (*p < 0x80) {
      if (std::isalnum(*p)) return true;
    } else if (*p == 0xE2 && (p[1] == 0x80 || p[1] == 0x81)) {
      if (p[2] == 0) break;
      p += 2;
    } else {
      return true;
    }
  }
  return false;
}

// Light-gray wash: every other pixel of every other row, phase-locked to the
// screen so adjacent runs join seamlessly.
void shadeRect(const GfxRenderer& renderer, const int x, const int y, const int w, const int h) {
  for (int py = y + (y & 1); py < y + h; py += 2) {
    for (int px = x + (x & 1); px < x + w; px += 2) {
      renderer.drawPixel(px, py, true);
    }
  }
}

}  // namespace

void PageHighlights::collectWords(const Page& page, const int marginLeft, const int marginTop, const int ascender,
                                  std::vector<Word>& out) {
  out.clear();
  out.reserve(256);
  uint16_t row = 0;
  for (const auto& element : page.elements) {
    if (element->getTag() != TAG_PageLine) continue;
    const auto* line = static_cast<const PageLine*>(element.get());
    const auto* block = line->getBlock();
    if (!block || !block->valid() || block->wordCount() == 0) continue;

    const int rubyShift = block->getRubyShift(ascender);
    for (uint16_t i = 0; i < block->wordCount(); i++) {
      Word word;
      word.x = static_cast<int16_t>(line->xPos + block->wordXpos(i) + marginLeft);
      word.y = static_cast<int16_t>(line->yPos + marginTop + rubyShift);
      word.width = 0;
      word.row = row;
      word.offset = block->wordVisibleOffset(i);
      word.text = block->wordText(i);
      word.style = block->wordStyle(i);
      word.selectable = isSelectableToken(word.text);
      out.push_back(word);
    }
    row++;
  }
}

void PageHighlights::measureWords(GfxRenderer& renderer, const int fontId, std::vector<Word>& words,
                                  const std::vector<bool>& marked) {
  // Batch the codepoints into the SD font's advance table first so the
  // per-word measuring below stays in RAM.
  std::string text;
  text.reserve(512);
  uint8_t styleMask = 0;
  for (size_t i = 0; i < words.size(); i++) {
    if (!marked.empty() && !marked[i]) continue;
    text.append(words[i].text);
    text.push_back(' ');
    styleMask |= static_cast<uint8_t>(1u << (static_cast<uint8_t>(words[i].style) & 0x03));
  }
  if (text.empty()) return;
  renderer.ensureSdCardFontReady(fontId, text.c_str(), styleMask == 0 ? 0x01 : styleMask);
  for (size_t i = 0; i < words.size(); i++) {
    if (!marked.empty() && !marked[i]) continue;
    words[i].width = static_cast<int16_t>(renderer.getTextAdvanceX(fontId, words[i].text, words[i].style));
  }
}

void PageHighlights::shadeMarked(const GfxRenderer& renderer, const std::vector<Word>& words,
                                 const std::vector<bool>& marked, const int lineHeight) {
  size_t i = 0;
  while (i < words.size()) {
    if (!marked[i]) {
      i++;
      continue;
    }
    // Extend the run while the next word is marked and on the same row.
    size_t last = i;
    while (last + 1 < words.size() && marked[last + 1] && words[last + 1].row == words[i].row) last++;
    int left = words[i].x;
    int right = words[i].x + words[i].width;
    for (size_t k = i; k <= last; k++) {
      if (words[k].x < left) left = words[k].x;
      if (words[k].x + words[k].width > right) right = words[k].x + words[k].width;
    }
    shadeRect(renderer, left - 1, words[i].y, right - left + 2, lineHeight);
    i = last + 1;
  }
}

void PageHighlights::shadePage(GfxRenderer& renderer, const Page& page, const int fontId, const int marginLeft,
                               const int marginTop, const uint16_t spineIndex,
                               const std::vector<HighlightEntry>& highlights) {
  bool spineHasHighlight = false;
  for (const auto& h : highlights) {
    if (h.spineIndex == spineIndex) {
      spineHasHighlight = true;
      break;
    }
  }
  if (!spineHasHighlight) return;

  std::vector<Word> words;
  collectWords(page, marginLeft, marginTop, renderer.getFontAscenderSize(fontId), words);
  std::vector<bool> marked(words.size(), false);
  bool any = false;
  for (size_t i = 0; i < words.size(); i++) {
    for (const auto& h : highlights) {
      if (h.overlaps(spineIndex, words[i].offset, words[i].offset + 1)) {
        marked[i] = true;
        any = true;
        break;
      }
    }
  }
  if (!any) return;
  measureWords(renderer, fontId, words, marked);
  shadeMarked(renderer, words, marked, renderer.getLineHeight(fontId));
}
