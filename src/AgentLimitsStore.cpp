#include "AgentLimitsStore.h"

#include <Logging.h>

#include <algorithm>

namespace {
constexpr const char* TAG = "AGENTLIM";
constexpr size_t MAX_NAME_LENGTH = 40;
constexpr size_t MAX_FIELD_LENGTH = 24;

std::string clipped(const char* value, const size_t maxLength) {
  if (!value) return {};
  std::string s(value);
  if (s.size() > maxLength) s.resize(maxLength);
  return s;
}
}  // namespace

void AgentLimitsStore::setUrl(std::string value) {
  if (value.size() > MAX_URL_LENGTH) value.resize(MAX_URL_LENGTH);
  url = std::move(value);
}

bool AgentLimitsStore::parseProviders(JsonArrayConst array, std::vector<AgentProvider>& out) {
  out.clear();
  if (array.isNull()) return false;

  out.reserve(std::min(array.size(), MAX_PROVIDERS));
  for (JsonVariantConst item : array) {
    if (out.size() >= MAX_PROVIDERS) break;
    AgentProvider provider;
    provider.name = clipped(item["name"] | "", MAX_NAME_LENGTH);
    if (provider.name.empty()) continue;
    provider.status = clipped(item["status"] | "", MAX_FIELD_LENGTH);

    JsonArrayConst windows = item["windows"];
    if (!windows.isNull()) {
      provider.windows.reserve(std::min(windows.size(), MAX_WINDOWS));
      for (JsonVariantConst w : windows) {
        if (provider.windows.size() >= MAX_WINDOWS) break;
        AgentWindow window;
        window.label = clipped(w["label"] | "", MAX_FIELD_LENGTH);
        if (window.label.empty()) continue;
        window.usedPercent = std::clamp<int>(w["used"] | 0, 0, 100);
        window.resets = clipped(w["resets"] | "", MAX_FIELD_LENGTH);
        provider.windows.push_back(std::move(window));
      }
    }
    out.push_back(std::move(provider));
  }
  return !out.empty();
}

void AgentLimitsStore::toJson(JsonDocument& doc) const {
  doc["url"] = url;
  doc["updatedAt"] = updatedAt;
  doc["subtitle"] = subtitle;

  const JsonArray arr = doc["providers"].to<JsonArray>();
  for (const auto& provider : providers) {
    const JsonObject o = arr.add<JsonObject>();
    o["name"] = provider.name;
    o["status"] = provider.status;
    const JsonArray windows = o["windows"].to<JsonArray>();
    for (const auto& window : provider.windows) {
      const JsonObject w = windows.add<JsonObject>();
      w["label"] = window.label;
      w["used"] = window.usedPercent;
      w["resets"] = window.resets;
    }
  }
}

bool AgentLimitsStore::fromJson(JsonVariantConst doc) {
  url = clipped(doc["url"] | "", MAX_URL_LENGTH);
  updatedAt = doc["updatedAt"] | 0u;
  subtitle = clipped(doc["subtitle"] | "", MAX_NAME_LENGTH);
  parseProviders(doc["providers"], providers);
  return true;
}

bool AgentLimitsStore::applyResponse(const std::string& body, const uint32_t fetchedAt) {
  JsonDocument doc;
  const DeserializationError err = deserializeJson(doc, body);
  if (err) {
    LOG_ERR(TAG, "Bad JSON: %s", err.c_str());
    return false;
  }

  std::vector<AgentProvider> parsed;
  if (!parseProviders(doc["providers"], parsed)) {
    LOG_ERR(TAG, "Response listed no usable providers");
    return false;
  }

  providers = std::move(parsed);
  subtitle = clipped(doc["subtitle"] | "", MAX_NAME_LENGTH);
  updatedAt = fetchedAt;
  return true;
}
