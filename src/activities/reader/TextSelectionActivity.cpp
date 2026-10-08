#include "TextSelectionActivity.h"

#include <GfxRenderer.h>
#include <I18n.h>
#include <Memory.h>

#include <algorithm>
#include <climits>
#include <cstdlib>
#include <cstring>

#include "CrossPointSettings.h"
#include "DictionaryWordSelectActivity.h"
#include "ReaderUtils.h"
#include "components/UITheme.h"
#include "util/HighlightFile.h"

namespace {

uint32_t countCodepoints(const char* text) {
  uint32_t count = 0;
  for (const uint8_t* p = reinterpret_cast<const uint8_t*>(text); *p != 0; p++) {
    if ((*p & 0xC0) != 0x80) count++;
  }
  return count;
}

}  // namespace

void TextSelectionActivity::onEnter() {
  Activity::onEnter();
  fontId = SETTINGS.getReaderFontId();
  lineHeight = renderer.getLineHeight(fontId);
  PageHighlights::collectWords(*page, marginLeft, marginTop, renderer.getFontAscenderSize(fontId), words);
  PageHighlights::measureWords(renderer, fontId, words);

  anchor = wordNear(touchX, touchY);
  if (anchor < 0) {
    // Held on a margin or an image: nothing to select.
    done = true;
    finish();
    return;
  }
  focus = anchor;

  {
    RenderLock lock(*this);
    cleanFrameSize = renderer.getBufferSize();
    // A failed copy only costs shrinking: the selection can still grow.
    cleanFrame = makeUniqueNoThrow<uint8_t[]>(cleanFrameSize);
    if (cleanFrame) {
      memcpy(cleanFrame.get(), renderer.getFrameBuffer(), cleanFrameSize);
    } else {
      LOG_ERR("TSEL", "OOM: page copy (%u bytes)", static_cast<uint32_t>(cleanFrameSize));
    }
  }
  requestUpdate();
}

// Nearest selectable word to (x, y): the row whose line box is vertically
// closest, then the word on it horizontally closest. -1 when the point is more
// than a line away from all text (only matters for the initial press; a drag
// past the last line clamps to it).
int TextSelectionActivity::wordNear(const int x, const int y) const {
  int bestRowDistance = INT_MAX;
  int bestRow = -1;
  for (const auto& word : words) {
    if (!word.selectable) continue;
    const int distance = y < word.y ? word.y - y : (y >= word.y + lineHeight ? y - (word.y + lineHeight) + 1 : 0);
    if (distance < bestRowDistance) {
      bestRowDistance = distance;
      bestRow = word.row;
    }
  }
  if (bestRow < 0 || (anchor < 0 && bestRowDistance > lineHeight)) return -1;

  int best = -1;
  int bestDistance = INT_MAX;
  for (int i = 0; i < static_cast<int>(words.size()); i++) {
    const auto& word = words[i];
    if (!word.selectable || word.row != bestRow) continue;
    const int distance = x < word.x ? word.x - x : (x >= word.x + word.width ? x - (word.x + word.width) + 1 : 0);
    if (distance < bestDistance) {
      bestDistance = distance;
      best = i;
    }
  }
  return best;
}

void TextSelectionActivity::loop() {
  if (done) return;

  if (showNoDictionary) {
    if (millis() - noDictionaryTime >= ReaderUtils::BOOKMARK_MESSAGE_DURATION_MS) {
      done = true;
      finish();
    }
    return;
  }

  int x = 0;
  int y = 0;
  if (mappedInput.isScreenTouchHeld(x, y)) {
    const int hit = wordNear(x, y);
    if (hit >= 0 && hit != focus) {
      focus = hit;
      moved = true;
      requestUpdate();
    }
    return;
  }

  // Finger lifted.
  commit();
}

void TextSelectionActivity::commit() {
  if (!moved) {
    lookUpAnchor();
    return;
  }
  saveHighlight();
  done = true;
  finish();
}

void TextSelectionActivity::lookUpAnchor() {
  if (SETTINGS.dictionaryName[0] == '\0') {
    showNoDictionary = true;
    noDictionaryTime = millis();
    requestUpdate();
    return;
  }
  const auto& word = words[anchor];
  const int centerX = word.x + word.width / 2;
  const int centerY = word.y + lineHeight / 2;
  done = true;
  // The page moves to the lookup screen; `words` keeps only geometry that is
  // still valid for a repaint, never its text.
  startActivityForResult(std::make_unique<DictionaryWordSelectActivity>(renderer, mappedInput, std::move(page),
                                                                        marginLeft, marginTop, centerX, centerY),
                         [](const ActivityResult&) { finish(); });
}

void TextSelectionActivity::saveHighlight() {
  const int lo = std::min(anchor, focus);
  const int hi = std::max(anchor, focus);

  HighlightEntry entry;
  entry.spineIndex = spineIndex;
  entry.start = words[lo].offset;
  entry.end = words[hi].offset + std::max<uint32_t>(1, countCodepoints(words[hi].text));
  entry.percentage = bookProgress;
  entry.text.reserve(256);
  for (int i = lo; i <= hi; i++) {
    if (i > lo) {
      const auto& prev = words[i - 1];
      const size_t prevLen = strlen(prev.text);
      const bool hyphenBreak = words[i].row != prev.row && prevLen > 0 && prev.text[prevLen - 1] == '-';
      // Touching words were glued in the source (punctuation, split tokens).
      const bool glued = words[i].row == prev.row && words[i].x <= prev.x + prev.width + 1;
      if (!hyphenBreak && !glued) entry.text.push_back(' ');
    }
    entry.text.append(words[i].text);
    if (entry.text.size() > HighlightEntry::MAX_TEXT_BYTES) break;  // addOrToggle trims and marks the cut
  }

  const bool added = HighlightFile::addOrToggle(highlights, std::move(entry));
  if (!HighlightFile::save(bookPath, bookTitle, highlights)) {
    LOG_ERR("TSEL", "Failed to save highlights for %s", bookPath.c_str());
  }
  LOG_DBG("TSEL", "Highlight %s (%zu in book)", added ? "added" : "removed", highlights.size());
}

void TextSelectionActivity::render(RenderLock&&) {
  if (cleanFrame) {
    memcpy(renderer.getFrameBuffer(), cleanFrame.get(), cleanFrameSize);
  }

  if (showNoDictionary) {
    // drawPopup refreshes the display itself.
    GUI.drawPopup(renderer, tr(STR_DICT_NO_DICT_SET));
    return;
  }

  if (anchor >= 0 && focus >= 0 && !words.empty()) {
    const int lo = std::min(anchor, focus);
    const int hi = std::max(anchor, focus);
    std::vector<bool> marked(words.size(), false);
    for (int i = lo; i <= hi; i++) marked[i] = true;
    PageHighlights::shadeMarked(renderer, words, marked, lineHeight);
  }
  renderer.displayBuffer(HalDisplay::FAST_REFRESH);
}
