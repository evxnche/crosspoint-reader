#include "HighlightsActivity.h"

#include <GfxRenderer.h>
#include <I18n.h>
#include <Logging.h>

#include <algorithm>
#include <cstdio>

#include "MappedInputManager.h"
#include "activities/ActivityManager.h"
#include "activities/reader/EpubReaderActivity.h"
#include "components/UITheme.h"
#include "components/UiAppHelpers.h"

namespace fui = freeink::ui;

namespace {

constexpr size_t MAX_LABEL_BYTES = 160;

// The list row shows the start of the passage; the full text stays in the file.
std::string shortLabel(const std::string& text) {
  if (text.size() <= MAX_LABEL_BYTES) return text;
  size_t cut = MAX_LABEL_BYTES;
  while (cut > 0 && (static_cast<uint8_t>(text[cut]) & 0xC0) == 0x80) cut--;
  return text.substr(0, cut) + "\xE2\x80\xA6";
}

}  // namespace

void HighlightsActivity::onEnter() {
  UiListActivity::onEnter();
  loadBooks();
}

// SD reads happen before taking the render lock; only the swap of what
// render() reads happens under it.
void HighlightsActivity::loadBooks() {
  auto loaded = HighlightFile::listBooks();
  RenderLock lock(*this);
  books = std::move(loaded);
  openBookIndex = -1;
  highlights.clear();
  rebuildRows();
  moveSelectionTo(0);
}

void HighlightsActivity::openBook(const int bookIndex) {
  if (bookIndex < 0 || bookIndex >= static_cast<int>(books.size())) return;
  std::vector<HighlightEntry> loaded;
  HighlightFile::load(books[bookIndex].bookPath, loaded);
  if (loaded.empty()) {
    loadBooks();  // the file vanished or emptied since the list was built
    requestUpdate();
    return;
  }
  {
    RenderLock lock(*this);
    highlights = std::move(loaded);
    openBookIndex = bookIndex;
    rebuildRows();
    moveSelectionTo(0);
  }
  requestUpdate();
}

void HighlightsActivity::rebuildRows() {
  labels.clear();
  subtitles.clear();
  rowItems.clear();
  const size_t count = openBookIndex < 0 ? books.size() : highlights.size();
  labels.reserve(count);
  subtitles.reserve(count);
  rowItems.reserve(count);

  for (size_t i = 0; i < count; i++) {
    char buf[48];
    if (openBookIndex < 0) {
      labels.push_back(books[i].title);
      if (books[i].count == 1) {
        subtitles.emplace_back(tr(STR_HIGHLIGHT_COUNT_ONE));
      } else {
        snprintf(buf, sizeof(buf), tr(STR_HIGHLIGHT_COUNT), static_cast<int>(books[i].count));
        subtitles.emplace_back(buf);
      }
    } else {
      labels.push_back(shortLabel(highlights[i].text));
      snprintf(buf, sizeof(buf), "%d%%",
               static_cast<int>(std::clamp(highlights[i].percentage, 0.0f, 1.0f) * 100.0f + 0.5f));
      subtitles.emplace_back(buf);
    }
  }
  // Pointers into labels/subtitles: taken only after both stopped growing.
  for (size_t i = 0; i < count; i++) {
    fui::ListItem item;
    item.label = labels[i].c_str();
    item.subtitle = subtitles[i].c_str();
    if (openBookIndex < 0) item.icon = listIconFor(UIIcon::Book, 32);
    item.actionValue = static_cast<int16_t>(i);
    rowItems.push_back(item);
  }
}

const char* HighlightsActivity::headerTitle() const {
  return openBookIndex < 0 ? tr(STR_HIGHLIGHTS) : books[openBookIndex].title.c_str();
}

void HighlightsActivity::activateIndex(const int index) {
  if (popup.isActive() || index < 0 || index >= listCount()) return;
  app.clearTapFlash();
  if (openBookIndex < 0) {
    openBook(index);
    return;
  }
  const auto& highlight = highlights[index];
  EpubReaderActivity::openAtHighlight(books[openBookIndex].bookPath, highlight.spineIndex, highlight.start);
  activityManager.goToReader(books[openBookIndex].bookPath);
}

void HighlightsActivity::onRowLongPress(const int index) {
  if (popup.isActive() || openBookIndex < 0 || index < 0 || index >= listCount()) return;
  app.clearTapFlash();
  confirmDelete(index);
}

void HighlightsActivity::confirmDelete(const int index) {
  nav.selected = index;
  const char* options[] = {tr(STR_CANCEL), tr(STR_DELETE)};
  popup.show(tr(STR_CONFIRM_DELETE_HIGHLIGHT), options, 2, 0, [this, index](const int choice) {
    if (choice != 1 || openBookIndex < 0 || index >= static_cast<int>(highlights.size())) return;
    std::vector<HighlightEntry> remaining = highlights;
    remaining.erase(remaining.begin() + index);
    const auto& book = books[openBookIndex];
    if (!HighlightFile::save(book.bookPath, book.title, remaining)) {
      LOG_ERR("HLS", "Failed to save highlights for %s", book.bookPath.c_str());
    }
    if (remaining.empty()) {
      loadBooks();
      return;
    }
    RenderLock lock(*this);
    highlights = std::move(remaining);
    books[openBookIndex].count = static_cast<uint16_t>(highlights.size());
    rebuildRows();
    moveSelectionTo(std::min(index, listCount() - 1));
  });
  requestUpdate();
}

bool HighlightsActivity::handleCustomInput() {
  return popup.handleInput(mappedInput, [this] { requestUpdate(); });
}

bool HighlightsActivity::handleButtons() {
  // Hold Confirm on a highlight: delete (the touch long-press equivalent).
  if (openBookIndex >= 0 && mappedInput.wasLongPressed(MappedInputManager::Button::Confirm, 700)) {
    if (nav.selected >= 0 && nav.selected < listCount()) confirmDelete(nav.selected);
    return true;
  }
  return UiListActivity::handleButtons();
}

void HighlightsActivity::onBackButton() {
  if (openBookIndex >= 0) {
    const int previous = openBookIndex;
    loadBooks();
    {
      RenderLock lock(*this);
      moveSelectionTo(std::min(previous, std::max(0, listCount() - 1)));
    }
    requestUpdate();
    return;
  }
  activityManager.goHome(HomeMenuItem::HIGHLIGHTS);
}

void HighlightsActivity::buildScreen(UiScreen& screen) {
  const auto& metrics = UITheme::getInstance().getMetrics();
  const Rect safe = UITheme::getInstance().getScreenSafeArea(renderer, true, false);
  screen.setContentMarginFromScreen(
      fui::Insets{static_cast<int16_t>(safe.y + metrics.topPadding + metrics.headerHeight),
                  static_cast<int16_t>(renderer.getScreenWidth() - (safe.x + safe.width)),
                  static_cast<int16_t>(renderer.getScreenHeight() - (safe.y + safe.height) + metrics.buttonHintsHeight),
                  static_cast<int16_t>(safe.x)});
  screen.spacer(static_cast<int16_t>(metrics.verticalSpacing));

  if (rowItems.empty()) {
    screen.centeredText(tr(STR_NO_HIGHLIGHTS), screen.theme().bodyText);
    return;
  }

  fui::ListProps props;
  props.items = rowItems.data();
  props.count = static_cast<uint16_t>(rowItems.size());
  props.action = ACTION_ROW;
  props.inputMask = openBookIndex < 0 ? fui::InputTouch : (fui::InputTouch | fui::InputLongPress);
  syncListViewport(screen, props);
  screen.list(props);
}

void HighlightsActivity::render(RenderLock&& lock) {
  if (popup.processRender(renderer, mappedInput)) return;
  UiListActivity::render(std::move(lock));
}
