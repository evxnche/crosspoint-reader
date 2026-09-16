#include "AgentLimitsStore.h"

#include <Logging.h>

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

void AgentLimitsStore::toJson(JsonDocument& doc) const {
  doc["url"] = url;
  doc["updatedAt"] = updatedAt;
  doc["subtitle"] = subtitle;

  const JsonArray arr = doc["limits"].to<JsonArray>();
  for (const auto& limit : limits) {
    const JsonObject o = arr.add<JsonObject>();
    o["name"] = limit.name;
    o["used"] = limit.used;
    o["total"] = limit.total;
    o["unit"] = limit.unit;
    o["resets"] = limit.resets;
  }
}

bool AgentLimitsStore::fromJson(JsonVariantConst doc) {
  url = clipped(doc["url"] | "", MAX_URL_LENGTH);
  updatedAt = doc["updatedAt"] | 0u;
  subtitle = clipped(doc["subtitle"] | "", MAX_NAME_LENGTH);

  limits.clear();
  JsonArrayConst arr = doc["limits"];
  if (arr.isNull()) return true;

  limits.reserve(std::min<size_t>(arr.size(), MAX_LIMITS));
  for (JsonVariantConst item : arr) {
    if (limits.size() >= MAX_LIMITS) break;
    AgentLimit limit;
    limit.name = clipped(item["name"] | "", MAX_NAME_LENGTH);
    limit.used = item["used"] | 0;
    limit.total = item["total"] | 0;
    limit.unit = clipped(item["unit"] | "", MAX_FIELD_LENGTH);
    limit.resets = clipped(item["resets"] | "", MAX_FIELD_LENGTH);
    if (!limit.name.empty()) limits.push_back(std::move(limit));
  }
  return true;
}

bool AgentLimitsStore::applyResponse(const std::string& body, const uint32_t fetchedAt) {
  // Sized for MAX_LIMITS entries with the field caps above, plus slack for the
  // whitespace and key names a hand-written endpoint is likely to emit.
  JsonDocument doc;
  const DeserializationError err = deserializeJson(doc, body);
  if (err) {
    LOG_ERR(TAG, "Bad JSON: %s", err.c_str());
    return false;
  }

  JsonArrayConst arr = doc["limits"];
  if (arr.isNull()) {
    LOG_ERR(TAG, "Response has no 'limits' array");
    return false;
  }

  std::vector<AgentLimit> parsed;
  parsed.reserve(std::min<size_t>(arr.size(), MAX_LIMITS));
  for (JsonVariantConst item : arr) {
    if (parsed.size() >= MAX_LIMITS) break;
    AgentLimit limit;
    limit.name = clipped(item["name"] | "", MAX_NAME_LENGTH);
    if (limit.name.empty()) continue;
    limit.used = item["used"] | 0;
    limit.total = item["total"] | 0;
    limit.unit = clipped(item["unit"] | "", MAX_FIELD_LENGTH);
    limit.resets = clipped(item["resets"] | "", MAX_FIELD_LENGTH);
    parsed.push_back(std::move(limit));
  }

  if (parsed.empty()) {
    LOG_ERR(TAG, "Response listed no usable limits");
    return false;
  }

  limits = std::move(parsed);
  subtitle = clipped(doc["subtitle"] | "", MAX_NAME_LENGTH);
  updatedAt = fetchedAt;
  return true;
}
