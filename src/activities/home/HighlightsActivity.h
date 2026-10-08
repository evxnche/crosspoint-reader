#pragma once

#include <string>
#include <vector>

#include "HighlightEntry.h"
#include "activities/UiListActivity.h"
#include "components/OptionPopup.h"
#include "util/HighlightFile.h"

// Home > Highlights. First level lists every book that has highlights; opening
// one lists its highlights in reading order. Tapping a highlight opens the book
// at that passage; holding it offers to delete it.
class HighlightsActivity final : public UiListActivity {
 public:
  HighlightsActivity(GfxRenderer& renderer, MappedInputManager& mappedInput)
      : UiListActivity("Highlights", renderer, mappedInput, /*wantsTouchLongPress=*/true) {}

  void onEnter() override;
  void render(RenderLock&&) override;

 private:
  int listCount() const override { return static_cast<int>(rowItems.size()); }
  void buildScreen(UiScreen& screen) override;
  void activateIndex(int index) override;
  void onRowLongPress(int index) override;
  bool handleCustomInput() override;
  bool handleButtons() override;
  void onBackButton() override;
  const char* headerTitle() const override;

  void loadBooks();
  void openBook(int bookIndex);
  void rebuildRows();
  void confirmDelete(int index);

  std::vector<HighlightFile::BookSummary> books;
  int openBookIndex = -1;  // -1: the book list is showing
  std::vector<HighlightEntry> highlights;

  std::vector<std::string> labels;
  std::vector<std::string> subtitles;
  std::vector<freeink::ui::ListItem> rowItems;
  OptionPopup popup;
};
