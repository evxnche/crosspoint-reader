#include "PlanStore.h"

#include <Logging.h>

#include <algorithm>
#include <cstring>

namespace {
constexpr const char* TAG = "PLAN";
constexpr size_t MAX_TITLE_LENGTH = 24;
constexpr size_t MAX_TIME_LENGTH = 12;
constexpr size_t MAX_TEXT_LENGTH = 200;

std::string clipped(const char* value, const size_t maxLength) {
  if (!value) return {};
  std::string s(value);
  if (s.size() > maxLength) s.resize(maxLength);
  return s;
}

// Wire names, matching desk-voice lib/plan.js.
const char* kindName(const PlanRow::Kind kind) {
  switch (kind) {
    case PlanRow::Kind::Head:
      return "head";
    case PlanRow::Kind::Task:
      return "task";
    case PlanRow::Kind::Item:
      return "item";
    case PlanRow::Kind::Event:
      return "event";
    case PlanRow::Kind::Note:
    default:
      return "note";
  }
}

PlanRow::Kind kindFrom(const char* name) {
  if (!name) return PlanRow::Kind::Note;
  if (strcmp(name, "head") == 0) return PlanRow::Kind::Head;
  if (strcmp(name, "task") == 0) return PlanRow::Kind::Task;
  if (strcmp(name, "item") == 0) return PlanRow::Kind::Item;
  if (strcmp(name, "event") == 0) return PlanRow::Kind::Event;
  // Unknown kinds from a newer endpoint still show their text.
  return PlanRow::Kind::Note;
}
}  // namespace

void PlanStore::setUrl(std::string value) {
  if (value.size() > MAX_URL_LENGTH) value.resize(MAX_URL_LENGTH);
  url = std::move(value);
}

bool PlanStore::parseScreens(JsonArrayConst array, std::vector<PlanScreen>& out) {
  out.clear();
  if (array.isNull()) return false;

  out.reserve(std::min(array.size(), MAX_SCREENS));
  for (JsonVariantConst item : array) {
    if (out.size() >= MAX_SCREENS) break;
    PlanScreen screen;
    screen.title = clipped(item["title"] | "", MAX_TITLE_LENGTH);
    if (screen.title.empty()) continue;

    JsonArrayConst rows = item["rows"];
    if (!rows.isNull()) {
      screen.rows.reserve(std::min(rows.size(), MAX_ROWS));
      for (JsonVariantConst r : rows) {
        if (screen.rows.size() >= MAX_ROWS) break;
        PlanRow row;
        row.text = clipped(r["t"] | "", MAX_TEXT_LENGTH);
        if (row.text.empty()) continue;
        row.kind = kindFrom(r["k"] | "");
        row.done = r["done"] | false;
        row.time = clipped(r["time"] | "", MAX_TIME_LENGTH);
        screen.rows.push_back(std::move(row));
      }
    }
    out.push_back(std::move(screen));
  }
  return !out.empty();
}

void PlanStore::toJson(JsonDocument& doc) const {
  doc["url"] = url;
  doc["updatedAt"] = updatedAt;

  const JsonArray arr = doc["screens"].to<JsonArray>();
  for (const auto& screen : screens) {
    const JsonObject s = arr.add<JsonObject>();
    s["title"] = screen.title;
    const JsonArray rows = s["rows"].to<JsonArray>();
    for (const auto& row : screen.rows) {
      const JsonObject r = rows.add<JsonObject>();
      r["k"] = kindName(row.kind);
      r["t"] = row.text;
      if (row.done) r["done"] = true;
      if (!row.time.empty()) r["time"] = row.time;
    }
  }
}

bool PlanStore::fromJson(JsonVariantConst doc) {
  url = clipped(doc["url"] | "", MAX_URL_LENGTH);
  updatedAt = doc["updatedAt"] | 0u;
  parseScreens(doc["screens"], screens);
  return true;
}

bool PlanStore::applyResponse(const std::string& body, const uint32_t fetchedAt) {
  JsonDocument doc;
  const DeserializationError err = deserializeJson(doc, body);
  if (err) {
    LOG_ERR(TAG, "Bad JSON: %s", err.c_str());
    return false;
  }

  std::vector<PlanScreen> parsed;
  if (!parseScreens(doc["screens"], parsed)) {
    LOG_ERR(TAG, "Response listed no screens");
    return false;
  }

  screens = std::move(parsed);
  updatedAt = fetchedAt;
  return true;
}
