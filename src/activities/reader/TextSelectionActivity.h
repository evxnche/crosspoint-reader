#pragma once

#include <Epub/Page.h>

#include <memory>
#include <string>
#include <vector>

#include "HighlightEntry.h"
#include "PageHighlights.h"
#include "activities/Activity.h"

// Started by a long-press on the reader page while the finger is still down.
// Sliding the finger extends a gray selection word by word; lifting it saves
// the selection as a highlight (or removes the highlight it lies inside).
// Lifting without sliding looks the pressed word up in the dictionary instead.
//
// The screen is never re-rendered from the book: the reader's page stays in
// the framebuffer, a copy of it is restored before each selection repaint, and
// the selection wash only adds pixels on top.
class TextSelectionActivity final : public Activity {
 public:
  TextSelectionActivity(GfxRenderer& renderer, MappedInputManager& mappedInput, std::unique_ptr<Page> page,
                        int marginLeft, int marginTop, int touchX, int touchY, std::string bookPath,
                        std::string bookTitle, uint16_t spineIndex, float bookProgress,
                        std::vector<HighlightEntry>& highlights)
      : Activity("TextSelection", renderer, mappedInput),
        page(std::move(page)),
        marginLeft(marginLeft),
        marginTop(marginTop),
        touchX(touchX),
        touchY(touchY),
        bookPath(std::move(bookPath)),
        bookTitle(std::move(bookTitle)),
        spineIndex(spineIndex),
        bookProgress(bookProgress),
        highlights(highlights) {}

  void onEnter() override;
  void loop() override;
  void render(RenderLock&&) override;

 private:
  int wordNear(int x, int y) const;
  void commit();
  void lookUpAnchor();
  void saveHighlight();

  std::unique_ptr<Page> page;
  const int marginLeft;
  const int marginTop;
  const int touchX;
  const int touchY;
  const std::string bookPath;
  const std::string bookTitle;
  const uint16_t spineIndex;
  const float bookProgress;
  std::vector<HighlightEntry>& highlights;

  int fontId = 0;
  int lineHeight = 0;
  std::vector<PageHighlights::Word> words;
  int anchor = -1;
  int focus = -1;  // word under the finger now
  bool moved = false;
  bool done = false;
  bool showNoDictionary = false;
  unsigned long noDictionaryTime = 0;

  // The reader page as it was on entry; each repaint starts from it.
  std::unique_ptr<uint8_t[]> cleanFrame;
  size_t cleanFrameSize = 0;
};
