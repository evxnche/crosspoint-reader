#include "AgentLimitsActivity.h"

#include <GfxRenderer.h>
#include <HalClock.h>
#include <I18n.h>
#include <Logging.h>
#include <WiFi.h>

#include <cstdio>
#include <variant>

#include "AgentLimitsStore.h"
#include "MappedInputManager.h"
#include "SilentRestart.h"
#include "activities/network/WifiSelectionActivity.h"
#include "activities/util/KeyboardEntryActivity.h"
#include "components/UITheme.h"
#include "network/HttpDownloader.h"

namespace fui = freeink::ui;

namespace {
constexpr const char* TAG = "AGENTUI";

// "42/100% - 3h 12m", or "42% - 3h 12m" when the endpoint reports no cap.
std::string formatLimit(const AgentLimit& limit) {
  char buf[64];
  if (limit.total > 0) {
    snprintf(buf, sizeof(buf), "%d/%d%s", limit.used, limit.total, limit.unit.c_str());
  } else {
    snprintf(buf, sizeof(buf), "%d%s", limit.used, limit.unit.c_str());
  }
  std::string text(buf);
  if (!limit.resets.empty()) {
    text += " · ";
    text += limit.resets;
  }
  return text;
}

std::string formatUpdated(const uint32_t epoch) {
  if (epoch == 0) return I18N.get(StrId::STR_AGENT_LIMITS_NEVER);
  const auto t = static_cast<time_t>(epoch);
  struct tm local{};
  if (!localtime_r(&t, &local)) return I18N.get(StrId::STR_AGENT_LIMITS_NEVER);
  char buf[16];
  strftime(buf, sizeof(buf), "%H:%M", &local);
  return buf;
}

// Epoch seconds from the RTC, or 0 when the device has no clock set.
uint32_t nowEpoch() {
  struct tm local{};
  if (!halClock.isAvailable() || !halClock.localTime(local)) return 0;
  const time_t t = mktime(&local);
  return t > 0 ? static_cast<uint32_t>(t) : 0;
}
}  // namespace

AgentLimitsActivity::AgentLimitsActivity(GfxRenderer& renderer, MappedInputManager& mappedInput)
    : UiListActivity("AgentLimits", renderer, mappedInput) {}

const char* AgentLimitsActivity::headerTitle() const { return I18N.get(StrId::STR_AGENT_LIMITS); }

void AgentLimitsActivity::onEnter() {
  UiListActivity::onEnter();
  AGENT_LIMITS.loadFromFile();
  rebuildRows();
}

void AgentLimitsActivity::onExit() {
  // A fetch leaves the radio and its LWIP/mbedTLS allocations behind; the other
  // network screens restart rather than read the fragmented heap.
  if (wifiStarted && WiFi.getMode() != WIFI_MODE_NULL) {
    WiFi.disconnect(false);
    delay(30);
    silentRestart();
  }
  UiListActivity::onExit();
}

void AgentLimitsActivity::rebuildRows() {
  const auto& limits = AGENT_LIMITS.getLimits();

  rowLabels_.clear();
  rowValues_.clear();
  rowLabels_.reserve(limits.size() + 3);
  rowValues_.reserve(limits.size() + 3);

  for (const auto& limit : limits) {
    rowLabels_.push_back(limit.name);
    rowValues_.push_back(formatLimit(limit));
  }

  if (limits.empty()) {
    rowLabels_.emplace_back(I18N.get(StrId::STR_AGENT_LIMITS_NONE));
    rowValues_.emplace_back();
  }

  // Status line: either the plan/subtitle the endpoint sent, or the age of the
  // numbers above. Either way the user can tell how much to trust them.
  std::string status = statusText;
  if (status.empty()) {
    status = AGENT_LIMITS.getSubtitle();
    if (status.empty()) status = I18N.get(StrId::STR_AGENT_LIMITS_UPDATED);
  }
  rowLabels_.push_back(status);
  rowValues_.push_back(formatUpdated(AGENT_LIMITS.getUpdatedAt()));

  refreshRowIndex = static_cast<int>(rowLabels_.size());
  rowLabels_.emplace_back(I18N.get(refreshing ? StrId::STR_AGENT_LIMITS_FETCHING : StrId::STR_AGENT_LIMITS_REFRESH));
  rowValues_.emplace_back();

  endpointRowIndex = static_cast<int>(rowLabels_.size());
  rowLabels_.emplace_back(I18N.get(StrId::STR_AGENT_LIMITS_ENDPOINT));
  const std::string& url = AGENT_LIMITS.getUrl();
  rowValues_.emplace_back(url.empty() ? I18N.get(StrId::STR_AGENT_LIMITS_NOT_SET) : url);

  rowItems_.assign(rowLabels_.size(), fui::ListItem{});
  for (size_t i = 0; i < rowLabels_.size(); ++i) {
    rowItems_[i].actionValue = static_cast<int16_t>(i);
  }
}

void AgentLimitsActivity::buildScreen(UiScreen& screen) {
  const auto& metrics = UITheme::getInstance().getMetrics();
  screen.setContentMarginFromScreen(fui::Insets{static_cast<int16_t>(metrics.topPadding + metrics.headerHeight), 0,
                                                static_cast<int16_t>(metrics.buttonHintsHeight), 0});
  screen.spacer(static_cast<int16_t>(metrics.verticalSpacing));

  for (size_t i = 0; i < rowItems_.size(); ++i) {
    rowItems_[i].label = rowLabels_[i].c_str();
    rowItems_[i].value = rowValues_[i].c_str();
  }

  fui::ListProps props;
  props.items = rowItems_.data();
  props.count = static_cast<int>(rowItems_.size());
  props.action = ACTION_ROW;
  props.inputMask = fui::InputTouch;  // physical buttons stay in loop()
  props.valueInset = 8;
  props.labelText = screen.theme().smallText;
  props.labelText.maxLines = 1;
  syncListViewport(screen, props);
  screen.list(props);
}

void AgentLimitsActivity::activateIndex(const int index) {
  if (index == endpointRowIndex) {
    editEndpoint();
    return;
  }
  if (index == refreshRowIndex && !refreshing) {
    startRefresh();
  }
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
        rebuildRows();
      });
}

void AgentLimitsActivity::startRefresh() {
  if (AGENT_LIMITS.getUrl().empty()) {
    RenderLock lock(*this);
    statusText = I18N.get(StrId::STR_AGENT_LIMITS_NOT_SET);
    rebuildRows();
    return;
  }

  {
    RenderLock lock(*this);
    refreshing = true;
    statusText = I18N.get(StrId::STR_AGENT_LIMITS_FETCHING);
    rebuildRows();
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
  rebuildRows();
  requestUpdate();
}
