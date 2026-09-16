#include "AgentLimitsActivity.h"

#include <Arduino.h>
#include <GfxRenderer.h>
#include <HalClock.h>
#include <I18n.h>
#include <Logging.h>
#include <WiFi.h>

#include <algorithm>
#include <cstdio>
#include <variant>

#include "AgentLimitsStore.h"
#include "MappedInputManager.h"
#include "SilentRestart.h"
#include "activities/network/WifiSelectionActivity.h"
#include "activities/util/KeyboardEntryActivity.h"
#include "components/UITheme.h"
#include "fontIds.h"
#include "network/HttpDownloader.h"

namespace {
constexpr const char* TAG = "AGENTUI";

// Layout, in pixels. The bar is thin on purpose: e-ink renders a solid block
// crisply at any height, so extra thickness only costs a row another provider
// could have used.
constexpr int BAR_HEIGHT = 10;
constexpr int BAR_GAP = 6;
constexpr int HEADER_BLOCK_HEIGHT = 34;
constexpr int WINDOW_BLOCK_HEIGHT = 44;
constexpr int STATUS_BLOCK_HEIGHT = 26;
constexpr int SIDE_PADDING = 18;

// At or above this the bar fills edge to edge instead of inset, so a window
// near its cap reads as alarming from across the desk.
constexpr int HEAVY_USE_PERCENT = 80;

std::string formatUsed(const AgentWindow& window) {
  char buf[64];
  snprintf(buf, sizeof(buf), "%s %d%% used", window.label.c_str(), window.usedPercent);
  return buf;
}

std::string formatResets(const AgentWindow& window) {
  if (window.resets.empty()) return {};
  char buf[48];
  snprintf(buf, sizeof(buf), "Resets in %s", window.resets.c_str());
  return buf;
}

std::string formatUpdated(const uint32_t epoch) {
  if (epoch == 0) return I18N.get(StrId::STR_AGENT_LIMITS_NEVER);
  const auto t = static_cast<time_t>(epoch);
  struct tm local {};
  if (!localtime_r(&t, &local)) return I18N.get(StrId::STR_AGENT_LIMITS_NEVER);
  char buf[16];
  strftime(buf, sizeof(buf), "%H:%M", &local);
  return buf;
}

// Epoch seconds from the RTC, or 0 when the device has no clock set.
uint32_t nowEpoch() {
  struct tm local {};
  if (!halClock.isAvailable() || !halClock.localTime(local)) return 0;
  const time_t t = mktime(&local);
  return t > 0 ? static_cast<uint32_t>(t) : 0;
}
}  // namespace

void AgentLimitsActivity::onEnter() {
  Activity::onEnter();
  AGENT_LIMITS.loadFromFile();
  rebuildBlocks();
  requestUpdate();
}

void AgentLimitsActivity::onExit() {
  // A fetch leaves the radio and its LWIP/TLS allocations behind; the other
  // network screens restart rather than read the fragmented heap.
  if (wifiStarted && WiFi.getMode() != WIFI_MODE_NULL) {
    WiFi.disconnect(false);
    delay(30);
    silentRestart();
  }
  Activity::onExit();
}

int AgentLimitsActivity::bodyTop() const {
  const auto& metrics = UITheme::getInstance().getMetrics();
  return metrics.topPadding + metrics.headerHeight + metrics.verticalSpacing;
}

int AgentLimitsActivity::bodyHeight() const {
  const auto& metrics = UITheme::getInstance().getMetrics();
  return renderer.getScreenHeight() - bodyTop() - metrics.buttonHintsHeight - metrics.verticalSpacing;
}

void AgentLimitsActivity::rebuildBlocks() {
  blocks.clear();
  const auto& providers = AGENT_LIMITS.getProviders();

  if (providers.empty()) {
    Block block;
    block.kind = Block::Kind::Message;
    block.height = STATUS_BLOCK_HEIGHT;
    block.text =
        I18N.get(AGENT_LIMITS.getUrl().empty() ? StrId::STR_AGENT_LIMITS_NOT_SET : StrId::STR_AGENT_LIMITS_NONE);
    blocks.push_back(std::move(block));
    paginate();
    return;
  }

  size_t estimate = providers.size();
  for (const auto& provider : providers) estimate += std::max<size_t>(provider.windows.size(), 1);
  blocks.reserve(estimate);

  for (size_t p = 0; p < providers.size(); ++p) {
    const AgentProvider& provider = providers[p];

    Block header;
    header.kind = Block::Kind::ProviderHeader;
    header.height = HEADER_BLOCK_HEIGHT;
    header.providerIndex = static_cast<int>(p);
    header.text = provider.name;
    header.rightText = provider.status;
    blocks.push_back(std::move(header));

    if (provider.windows.empty()) {
      // A provider with nothing to report still gets a line, so signed out
      // reads as signed out rather than as missing.
      Block status;
      status.kind = Block::Kind::Status;
      status.height = STATUS_BLOCK_HEIGHT;
      status.providerIndex = static_cast<int>(p);
      status.text = provider.status.empty() ? "-" : provider.status;
      blocks.push_back(std::move(status));
      continue;
    }

    for (size_t w = 0; w < provider.windows.size(); ++w) {
      Block window;
      window.kind = Block::Kind::Window;
      window.height = WINDOW_BLOCK_HEIGHT;
      window.providerIndex = static_cast<int>(p);
      window.windowIndex = static_cast<int>(w);
      window.text = formatUsed(provider.windows[w]);
      window.rightText = formatResets(provider.windows[w]);
      blocks.push_back(std::move(window));
    }
  }

  paginate();
}

void AgentLimitsActivity::paginate() {
  pageStarts.clear();
  const int available = bodyHeight();
  const int total = static_cast<int>(blocks.size());

  int index = 0;
  while (index < total) {
    const int pageStart = index;
    pageStarts.push_back(pageStart);

    int used = 0;
    while (index < total && used + blocks[index].height <= available) {
      used += blocks[index].height;
      ++index;
    }
    // A single block taller than the page would otherwise loop forever.
    if (index == pageStart) ++index;

    // Never end a page on a provider header: its first bar belongs with it.
    if (index < total && index - 1 > pageStart && blocks[index - 1].kind == Block::Kind::ProviderHeader) {
      --index;
    }
  }

  if (pageStarts.empty()) pageStarts.push_back(0);
  pageStarts.push_back(total);

  const int pages = static_cast<int>(pageStarts.size()) - 1;
  currentPage = std::clamp(currentPage, 0, std::max(0, pages - 1));
}

void AgentLimitsActivity::drawBar(const int x, const int y, const int width, const int percent) const {
  const int filled = std::clamp(width * percent / 100, 0, width);

  // Track first, so an empty bar is still visible as a bar.
  renderer.drawRect(x, y, width, BAR_HEIGHT, true);
  if (filled <= 2) return;

  if (percent >= HEAVY_USE_PERCENT) {
    renderer.fillRect(x, y, filled, BAR_HEIGHT, true);
  } else {
    // Inset so the fill never swallows the track outline.
    renderer.fillRect(x + 1, y + 1, filled - 2, BAR_HEIGHT - 2, true);
  }
}

void AgentLimitsActivity::drawBlock(const Block& block, const int x, const int y, const int width) const {
  switch (block.kind) {
    case Block::Kind::ProviderHeader: {
      const int baseline = y + renderer.getLineHeight(UI_12_FONT_ID);
      renderer.drawText(UI_12_FONT_ID, x, baseline, block.text.c_str(), true, EpdFontFamily::BOLD);
      if (!block.rightText.empty()) {
        const int w = renderer.getTextWidth(SMALL_FONT_ID, block.rightText.c_str());
        renderer.drawText(SMALL_FONT_ID, x + width - w, baseline, block.rightText.c_str());
      }
      // Rule under the name, separating one provider's card from the next.
      renderer.fillRect(x, y + HEADER_BLOCK_HEIGHT - 8, width, 1, true);
      break;
    }
    case Block::Kind::Window: {
      const int baseline = y + renderer.getLineHeight(UI_10_FONT_ID);
      renderer.drawText(UI_10_FONT_ID, x, baseline, block.text.c_str(), true, EpdFontFamily::BOLD);
      if (!block.rightText.empty()) {
        const int w = renderer.getTextWidth(SMALL_FONT_ID, block.rightText.c_str());
        renderer.drawText(SMALL_FONT_ID, x + width - w, baseline, block.rightText.c_str());
      }
      const auto& providers = AGENT_LIMITS.getProviders();
      if (block.providerIndex >= 0 && block.providerIndex < static_cast<int>(providers.size())) {
        const auto& windows = providers[block.providerIndex].windows;
        if (block.windowIndex >= 0 && block.windowIndex < static_cast<int>(windows.size())) {
          drawBar(x, baseline + BAR_GAP, width, windows[block.windowIndex].usedPercent);
        }
      }
      break;
    }
    case Block::Kind::Status:
    case Block::Kind::Message: {
      const int baseline = y + renderer.getLineHeight(SMALL_FONT_ID);
      renderer.drawText(SMALL_FONT_ID, x, baseline, block.text.c_str());
      break;
    }
  }
}

void AgentLimitsActivity::render(RenderLock&&) {
  const auto& metrics = UITheme::getInstance().getMetrics();
  const int pageWidth = renderer.getScreenWidth();

  renderer.clearScreen();

  std::string subtitle = statusText;
  if (subtitle.empty()) {
    subtitle = AGENT_LIMITS.getSubtitle();
    if (subtitle.empty()) subtitle = formatUpdated(AGENT_LIMITS.getUpdatedAt());
  }
  GUI.drawHeader(renderer, Rect{0, metrics.topPadding, pageWidth, metrics.headerHeight},
                 I18N.get(StrId::STR_AGENT_LIMITS), subtitle.c_str());

  const int x = SIDE_PADDING;
  const int width = pageWidth - SIDE_PADDING * 2;
  int y = bodyTop();

  const int pages = static_cast<int>(pageStarts.size()) - 1;
  if (pages > 0) {
    const int from = pageStarts[currentPage];
    const int to = pageStarts[currentPage + 1];
    for (int i = from; i < to; ++i) {
      drawBlock(blocks[i], x, y, width);
      y += blocks[i].height;
    }
  }

  // The page counter only earns its space when there is more than one page.
  if (pages > 1) {
    char buf[24];
    snprintf(buf, sizeof(buf), "%d / %d", currentPage + 1, pages);
    const int w = renderer.getTextWidth(SMALL_FONT_ID, buf);
    renderer.drawText(SMALL_FONT_ID, (pageWidth - w) / 2, renderer.getScreenHeight() - metrics.buttonHintsHeight - 4,
                      buf);
  }

  GUI.drawButtonHints(renderer, I18N.get(StrId::STR_BACK), I18N.get(StrId::STR_AGENT_LIMITS_REFRESH),
                      I18N.get(StrId::STR_AGENT_LIMITS_ENDPOINT), nullptr);
}

void AgentLimitsActivity::loop() {
  if (refreshing) return;

  if (mappedInput.wasReleased(MappedInputManager::Button::Back)) {
    finish();
    return;
  }
  if (mappedInput.wasReleased(MappedInputManager::Button::Confirm)) {
    startRefresh();
    return;
  }
  if (mappedInput.wasReleased(MappedInputManager::Button::Left)) {
    editEndpoint();
    return;
  }

  const int pages = static_cast<int>(pageStarts.size()) - 1;

  // Same tap zones as the reader: left third is back a page, the rest forward.
  int tx = 0;
  int ty = 0;
  if (mappedInput.wasScreenTapped(tx, ty)) {
    if (tx < renderer.getScreenWidth() / 3) {
      if (currentPage > 0) {
        --currentPage;
        requestUpdate();
      }
    } else if (currentPage + 1 < pages) {
      ++currentPage;
      requestUpdate();
    }
    return;
  }

  buttonNavigator.onNext([this, pages] {
    if (currentPage + 1 < pages) {
      ++currentPage;
      requestUpdate();
    }
  });
  buttonNavigator.onPrevious([this] {
    if (currentPage > 0) {
      --currentPage;
      requestUpdate();
    }
  });
}

void AgentLimitsActivity::editEndpoint() {
  startActivityForResult(
      std::make_unique<KeyboardEntryActivity>(renderer, mappedInput, I18N.get(StrId::STR_AGENT_LIMITS_ENDPOINT),
                                              AGENT_LIMITS.getUrl(), AgentLimitsStore::MAX_URL_LENGTH, InputType::Url),
      [this](const ActivityResult& result) {
        if (!result.isCancelled) {
          AGENT_LIMITS.setUrl(std::get<KeyboardResult>(result.data).text);
          AGENT_LIMITS.saveToFile();
        }
        RenderLock lock(*this);
        statusText.clear();
        rebuildBlocks();
      });
}

void AgentLimitsActivity::startRefresh() {
  if (AGENT_LIMITS.getUrl().empty()) {
    RenderLock lock(*this);
    statusText = I18N.get(StrId::STR_AGENT_LIMITS_NOT_SET);
    rebuildBlocks();
    return;
  }

  {
    RenderLock lock(*this);
    refreshing = true;
    statusText = I18N.get(StrId::STR_AGENT_LIMITS_FETCHING);
  }
  requestUpdateAndWait();

  wifiStarted = true;
  WiFi.mode(WIFI_STA);
  startActivityForResult(std::make_unique<WifiSelectionActivity>(renderer, mappedInput),
                         [this](const ActivityResult& result) { onWifiReady(!result.isCancelled); });
}

void AgentLimitsActivity::onWifiReady(const bool connected) {
  StrId status = StrId::STR_AGENT_LIMITS_FAILED;

  if (connected) {
    std::string body;
    if (!HttpDownloader::fetchUrl(AGENT_LIMITS.getUrl(), body)) {
      LOG_ERR(TAG, "Fetch failed");
    } else if (!AGENT_LIMITS.applyResponse(body, nowEpoch())) {
      status = StrId::STR_AGENT_LIMITS_BAD_DATA;
    } else {
      AGENT_LIMITS.saveToFile();
      status = StrId::STR_NONE_OPT;
    }
  }

  RenderLock lock(*this);
  refreshing = false;
  statusText = status == StrId::STR_NONE_OPT ? "" : I18N.get(status);
  currentPage = 0;
  rebuildBlocks();
  requestUpdate();
}
