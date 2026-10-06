#include "PlanActivity.h"

#include <Arduino.h>
#include <GfxRenderer.h>
#include <HalClock.h>
#include <I18n.h>
#include <Logging.h>
#include <WiFi.h>

#include <algorithm>
#include <cstdio>
#include <variant>

#include "MappedInputManager.h"
#include "PlanStore.h"
#include "SilentRestart.h"
#include "activities/network/WifiSelectionActivity.h"
#include "activities/util/KeyboardEntryActivity.h"
#include "components/UITheme.h"
#include "fontIds.h"
#include "network/HttpDownloader.h"

namespace {
constexpr const char* TAG = "PLANUI";

constexpr int SIDE_PADDING = 18;
constexpr int TAB_HEIGHT = 34;
constexpr int TAB_GAP = 8;
constexpr int ROW_GAP = 6;
constexpr int HEAD_GAP = 10;  // extra space above a heading that follows other rows
constexpr int MAX_LINES = 4;
constexpr int BOX_SIZE = 14;
constexpr int TASK_INDENT = 26;
constexpr int ITEM_INDENT = 16;
constexpr int TIME_COLUMN = 84;
constexpr int CONTROL_HEIGHT = 40;
constexpr int CONTROL_GAP = 10;
constexpr unsigned long EDIT_HOLD_MS = 1000;

int fontFor(const PlanRow::Kind kind) {
  if (kind == PlanRow::Kind::Head) return UI_12_FONT_ID;
  if (kind == PlanRow::Kind::Note) return SMALL_FONT_ID;
  return UI_10_FONT_ID;
}

EpdFontFamily::Style styleFor(const PlanRow::Kind kind) {
  return kind == PlanRow::Kind::Head ? EpdFontFamily::BOLD : EpdFontFamily::REGULAR;
}

std::string formatUpdated(const uint32_t epoch) {
  if (epoch == 0) return {};
  const auto t = static_cast<time_t>(epoch);
  struct tm local {};
  if (!localtime_r(&t, &local)) return {};
  char time[16];
  strftime(time, sizeof(time), "%H:%M", &local);
  char buf[48];
  snprintf(buf, sizeof(buf), "%s %s", tr(STR_PLAN_UPDATED), time);
  return buf;
}

// Epoch seconds from the RTC, or 0 when the device has no clock set.
uint32_t nowEpoch() {
  struct tm local {};
  if (!halClock.isAvailable() || !halClock.localTime(local)) return 0;
  const time_t t = mktime(&local);
  return t > 0 ? static_cast<uint32_t>(t) : 0;
}

bool hits(const Rect& r, const int x, const int y) {
  return x >= r.x && x < r.x + r.width && y >= r.y && y < r.y + r.height;
}
}  // namespace

void PlanActivity::onEnter() {
  Activity::onEnter();
  PLAN.loadFromFile();
  rebuildBlocks();
  requestUpdate();
}

void PlanActivity::onExit() {
  // A fetch leaves the radio and its LWIP/TLS allocations behind; the other
  // network screens restart rather than read the fragmented heap.
  if (wifiStarted && WiFi.getMode() != WIFI_MODE_NULL) {
    WiFi.disconnect(false);
    delay(30);
    silentRestart();
  }
  Activity::onExit();
}

int PlanActivity::tabsTop() const {
  const auto& metrics = UITheme::getInstance().getMetrics();
  return metrics.topPadding + metrics.headerHeight + metrics.verticalSpacing;
}

int PlanActivity::bodyTop() const { return tabsTop() + TAB_HEIGHT + TAB_GAP; }

// Top of the on-screen control row, or where the button-hint band begins on a
// device without touch. The page counter sits just above it.
int PlanActivity::controlsTop() const {
  const auto& metrics = UITheme::getInstance().getMetrics();
  const int bottom = renderer.getScreenHeight() - metrics.verticalSpacing -
                     (mappedInput.hasTouch() ? 0 : metrics.buttonHintsHeight);
  return bottom - (mappedInput.hasTouch() ? CONTROL_HEIGHT : 0);
}

int PlanActivity::textX(const int row) const {
  const auto& screens = PLAN.getScreens();
  if (row < 0 || screenIndex >= static_cast<int>(screens.size())) return SIDE_PADDING;
  switch (screens[screenIndex].rows[row].kind) {
    case PlanRow::Kind::Task:
      return SIDE_PADDING + TASK_INDENT;
    case PlanRow::Kind::Item:
      return SIDE_PADDING + ITEM_INDENT;
    case PlanRow::Kind::Event:
      return SIDE_PADDING + TIME_COLUMN;
    default:
      return SIDE_PADDING;
  }
}

void PlanActivity::rebuildBlocks() {
  blocks.clear();
  const auto& screens = PLAN.getScreens();
  const int right = renderer.getScreenWidth() - SIDE_PADDING;

  if (screens.empty()) {
    Block block;
    block.lines = renderer.wrappedText(SMALL_FONT_ID, PLAN.getUrl().empty() ? tr(STR_PLAN_NOT_SET) : tr(STR_PLAN_NONE),
                                       right - SIDE_PADDING, MAX_LINES);
    block.height = static_cast<int>(block.lines.size()) * renderer.getLineHeight(SMALL_FONT_ID) + ROW_GAP;
    blocks.push_back(std::move(block));
    paginate();
    return;
  }

  screenIndex = std::clamp(screenIndex, 0, static_cast<int>(screens.size()) - 1);
  const auto& rows = screens[screenIndex].rows;
  blocks.reserve(rows.size());
  for (size_t i = 0; i < rows.size(); ++i) {
    Block block;
    block.row = static_cast<int>(i);
    const int font = fontFor(rows[i].kind);
    block.lines = renderer.wrappedText(font, rows[i].text.c_str(), right - textX(block.row), MAX_LINES,
                                       styleFor(rows[i].kind));
    block.height = static_cast<int>(block.lines.size()) * renderer.getLineHeight(font) + ROW_GAP;
    if (rows[i].kind == PlanRow::Kind::Head && i > 0) block.height += HEAD_GAP;
    blocks.push_back(std::move(block));
  }
  paginate();
}

void PlanActivity::paginate() {
  pageStarts.clear();
  const int available = controlsTop() - CONTROL_GAP - renderer.getLineHeight(SMALL_FONT_ID) - bodyTop();
  const int total = static_cast<int>(blocks.size());
  const auto& screens = PLAN.getScreens();
  const auto isHead = [&](const int i) {
    const int row = blocks[i].row;
    return row >= 0 && screenIndex < static_cast<int>(screens.size()) &&
           screens[screenIndex].rows[row].kind == PlanRow::Kind::Head;
  };

  int index = 0;
  while (index < total) {
    const int pageStart = index;
    pageStarts.push_back(pageStart);
    // A heading opening a page drops its top gap, so measure it without.
    int used = isHead(index) && index > 0 ? -HEAD_GAP : 0;
    while (index < total && used + blocks[index].height <= available) {
      used += blocks[index].height;
      ++index;
    }
    // A single block taller than the page would otherwise loop forever.
    if (index == pageStart) ++index;
    // Never end a page on a heading: its first row belongs with it.
    if (index < total && index - 1 > pageStart && isHead(index - 1)) --index;
  }

  if (pageStarts.empty()) pageStarts.push_back(0);
  pageStarts.push_back(total);

  const int pages = static_cast<int>(pageStarts.size()) - 1;
  currentPage = std::clamp(currentPage, 0, std::max(0, pages - 1));
}

Rect PlanActivity::tabRect(const int index) const {
  const int count = std::max<int>(1, PLAN.getScreens().size());
  const int width = (renderer.getScreenWidth() - SIDE_PADDING * 2) / count;
  return Rect{SIDE_PADDING + index * width, tabsTop(), width, TAB_HEIGHT};
}

Rect PlanActivity::refreshRect() const {
  const int usable = renderer.getScreenWidth() - SIDE_PADDING * 2 - CONTROL_GAP;
  return Rect{SIDE_PADDING, controlsTop(), usable / 2, CONTROL_HEIGHT};
}

Rect PlanActivity::endpointRect() const {
  const Rect left = refreshRect();
  return Rect{left.x + left.width + CONTROL_GAP, left.y, left.width, CONTROL_HEIGHT};
}

void PlanActivity::drawTabs() const {
  const auto& screens = PLAN.getScreens();
  const int lineHeight = renderer.getLineHeight(UI_10_FONT_ID);
  for (size_t i = 0; i < screens.size(); ++i) {
    const Rect r = tabRect(static_cast<int>(i));
    const bool active = static_cast<int>(i) == screenIndex;
    // The current screen is a filled pill; the others are plain labels.
    if (active) renderer.fillRect(r.x + 2, r.y, r.width - 4, r.height, true);
    const auto style = active ? EpdFontFamily::BOLD : EpdFontFamily::REGULAR;
    const std::string label = renderer.truncatedText(UI_10_FONT_ID, screens[i].title.c_str(), r.width - 8, style);
    const int w = renderer.getTextWidth(UI_10_FONT_ID, label.c_str(), style);
    renderer.drawText(UI_10_FONT_ID, r.x + (r.width - w) / 2, r.y + (r.height - lineHeight) / 2, label.c_str(), !active,
                      style);
  }
  renderer.fillRect(SIDE_PADDING, tabsTop() + TAB_HEIGHT + TAB_GAP / 2, renderer.getScreenWidth() - SIDE_PADDING * 2,
                    1, true);
}

void PlanActivity::drawControls() const {
  if (!mappedInput.hasTouch()) return;

  const auto draw = [this](const Rect& rect, const char* label) {
    renderer.drawRect(rect.x, rect.y, rect.width, rect.height, true);
    const int w = renderer.getTextWidth(UI_10_FONT_ID, label);
    const int top = rect.y + (rect.height - renderer.getLineHeight(UI_10_FONT_ID)) / 2;
    renderer.drawText(UI_10_FONT_ID, rect.x + (rect.width - w) / 2, top, label);
  };

  draw(refreshRect(), refreshing ? tr(STR_PLAN_FETCHING) : tr(STR_PLAN_REFRESH));
  draw(endpointRect(), tr(STR_PLAN_ENDPOINT));
}

void PlanActivity::drawBlock(const Block& block, int y) const {
  const auto& screens = PLAN.getScreens();
  if (block.row < 0 || screenIndex >= static_cast<int>(screens.size())) {
    for (const auto& line : block.lines) {
      renderer.drawText(SMALL_FONT_ID, SIDE_PADDING, y, line.c_str());
      y += renderer.getLineHeight(SMALL_FONT_ID);
    }
    return;
  }

  const PlanRow& row = screens[screenIndex].rows[block.row];
  const int font = fontFor(row.kind);
  const auto style = styleFor(row.kind);
  const int lineHeight = renderer.getLineHeight(font);
  const int x = textX(block.row);

  switch (row.kind) {
    case PlanRow::Kind::Task: {
      const int boxY = y + (lineHeight - BOX_SIZE) / 2;
      renderer.drawRect(SIDE_PADDING, boxY, BOX_SIZE, BOX_SIZE, true);
      if (row.done) renderer.fillRect(SIDE_PADDING + 3, boxY + 3, BOX_SIZE - 6, BOX_SIZE - 6, true);
      break;
    }
    case PlanRow::Kind::Item:
      renderer.fillRect(SIDE_PADDING + 2, y + lineHeight / 2 - 2, 5, 5, true);
      break;
    case PlanRow::Kind::Event:
      renderer.drawText(SMALL_FONT_ID, SIDE_PADDING, y + (lineHeight - renderer.getLineHeight(SMALL_FONT_ID)) / 2,
                        row.time.c_str());
      break;
    default:
      break;
  }

  for (const auto& line : block.lines) {
    renderer.drawText(font, x, y, line.c_str(), true, style);
    y += lineHeight;
  }
}

void PlanActivity::render(RenderLock&&) {
  const auto& metrics = UITheme::getInstance().getMetrics();
  const int pageWidth = renderer.getScreenWidth();

  renderer.clearScreen();

  const std::string subtitle = statusText.empty() ? formatUpdated(PLAN.getUpdatedAt()) : statusText;
  GUI.drawHeader(renderer, Rect{0, metrics.topPadding, pageWidth, metrics.headerHeight}, tr(STR_PLAN),
                 subtitle.c_str());
  drawTabs();

  const int pages = static_cast<int>(pageStarts.size()) - 1;
  if (pages > 0) {
    int y = bodyTop();
    const int from = pageStarts[currentPage];
    const int to = pageStarts[currentPage + 1];
    for (int i = from; i < to; ++i) {
      // A heading at the top of a page needs no gap above it.
      const auto& screens = PLAN.getScreens();
      const bool headFirst = i == from && i > 0 && blocks[i].row >= 0 &&
                             screens[screenIndex].rows[blocks[i].row].kind == PlanRow::Kind::Head;
      if (headFirst) y -= HEAD_GAP;
      const bool gapAbove = blocks[i].row > 0 && screens[screenIndex].rows[blocks[i].row].kind == PlanRow::Kind::Head;
      drawBlock(blocks[i], gapAbove ? y + HEAD_GAP : y);
      y += blocks[i].height;
    }
  }

  // The page counter only earns its space when there is more than one page.
  if (pages > 1) {
    char buf[24];
    snprintf(buf, sizeof(buf), "%d / %d", currentPage + 1, pages);
    const int w = renderer.getTextWidth(SMALL_FONT_ID, buf);
    renderer.drawText(SMALL_FONT_ID, (pageWidth - w) / 2,
                      controlsTop() - CONTROL_GAP / 2 - renderer.getLineHeight(SMALL_FONT_ID), buf);
  }

  drawControls();

  // Front Left/Right are named after the screens they lead to.
  const auto& screens = PLAN.getScreens();
  const int count = static_cast<int>(screens.size());
  const char* previous = count > 1 ? screens[(screenIndex + count - 1) % count].title.c_str() : nullptr;
  const char* next = count > 1 ? screens[(screenIndex + 1) % count].title.c_str() : nullptr;
  GUI.drawButtonHints(renderer, tr(STR_BACK), tr(STR_PLAN_REFRESH), previous, next);
}

void PlanActivity::showScreen(const int index) {
  const int count = static_cast<int>(PLAN.getScreens().size());
  if (count < 2) return;
  screenIndex = (index + count) % count;
  currentPage = 0;
  rebuildBlocks();
  requestUpdate();
}

void PlanActivity::turnPage(const int direction) {
  const int pages = static_cast<int>(pageStarts.size()) - 1;
  const int target = currentPage + direction;
  if (target < 0 || target >= pages) return;
  currentPage = target;
  requestUpdate();
}

void PlanActivity::loop() {
  if (refreshing) return;

  if (mappedInput.wasReleased(MappedInputManager::Button::Back)) {
    finish();
    return;
  }
  if (mappedInput.wasLongPressed(MappedInputManager::Button::Confirm, EDIT_HOLD_MS)) {
    editEndpoint();
    return;
  }
  if (mappedInput.wasReleased(MappedInputManager::Button::Confirm)) {
    startRefresh();
    return;
  }
  if (mappedInput.wasReleased(MappedInputManager::Button::Left)) {
    showScreen(screenIndex - 1);
    return;
  }
  if (mappedInput.wasReleased(MappedInputManager::Button::Right)) {
    showScreen(screenIndex + 1);
    return;
  }
  if (mappedInput.wasReleased(MappedInputManager::Button::Up)) {
    turnPage(-1);
    return;
  }
  if (mappedInput.wasReleased(MappedInputManager::Button::Down)) {
    turnPage(1);
    return;
  }

  switch (mappedInput.wasSwipe()) {
    case MappedInputManager::SwipeDir::Left:
      showScreen(screenIndex + 1);
      return;
    case MappedInputManager::SwipeDir::Right:
      if (!mappedInput.wasBackGesture()) showScreen(screenIndex - 1);
      return;
    default:
      break;
  }

  // Tabs and controls by position; elsewhere the reader's tap zones: the left
  // third is back a page, the rest forward.
  int tx = 0;
  int ty = 0;
  if (mappedInput.wasScreenTapped(tx, ty)) {
    if (mappedInput.hasTouch()) {
      if (hits(refreshRect(), tx, ty)) {
        startRefresh();
        return;
      }
      if (hits(endpointRect(), tx, ty)) {
        editEndpoint();
        return;
      }
    }
    for (size_t i = 0; i < PLAN.getScreens().size(); ++i) {
      if (hits(tabRect(static_cast<int>(i)), tx, ty)) {
        if (static_cast<int>(i) != screenIndex) showScreen(static_cast<int>(i));
        return;
      }
    }
    turnPage(tx < renderer.getScreenWidth() / 3 ? -1 : 1);
  }
}

void PlanActivity::editEndpoint() {
  startActivityForResult(
      std::make_unique<KeyboardEntryActivity>(renderer, mappedInput, tr(STR_PLAN_ENDPOINT), PLAN.getUrl(),
                                              PlanStore::MAX_URL_LENGTH, InputType::Url),
      [this](const ActivityResult& result) {
        if (!result.isCancelled) {
          PLAN.setUrl(std::get<KeyboardResult>(result.data).text);
          PLAN.saveToFile();
        }
        RenderLock lock(*this);
        statusText.clear();
        rebuildBlocks();
      });
}

void PlanActivity::startRefresh() {
  if (PLAN.getUrl().empty()) {
    editEndpoint();
    return;
  }

  {
    RenderLock lock(*this);
    refreshing = true;
    statusText = tr(STR_PLAN_FETCHING);
  }
  requestUpdateAndWait();

  wifiStarted = true;
  WiFi.mode(WIFI_STA);
  startActivityForResult(std::make_unique<WifiSelectionActivity>(renderer, mappedInput),
                         [this](const ActivityResult& result) { onWifiReady(!result.isCancelled); });
}

void PlanActivity::onWifiReady(const bool connected) {
  const char* status = tr(STR_PLAN_FAILED);

  if (connected) {
    // The fetch blocks this task for as long as the network takes (a captive
    // portal can stall it for minutes), so show the plan screen saying so
    // rather than leave the Wi-Fi screen up looking frozen.
    {
      RenderLock lock(*this);
      statusText = tr(STR_PLAN_FETCHING);
    }
    requestUpdateAndWait();
    std::string body;
    if (!HttpDownloader::fetchUrl(PLAN.getUrl(), body)) {
      LOG_ERR(TAG, "Fetch failed");
    } else if (!PLAN.applyResponse(body, nowEpoch())) {
      status = tr(STR_PLAN_BAD_DATA);
    } else {
      PLAN.saveToFile();
      status = nullptr;
    }
  }

  RenderLock lock(*this);
  refreshing = false;
  statusText = status ? status : "";
  currentPage = 0;
  rebuildBlocks();
  requestUpdate();
}
