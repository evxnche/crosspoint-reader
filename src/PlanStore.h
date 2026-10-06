#pragma once
#include <ArduinoJson.h>
#include <PersistableStore.h>

#include <string>
#include <vector>

// One line on a plan screen. The endpoint decides what goes where; the reader
// only knows how to draw each kind.
struct PlanRow {
  enum class Kind : uint8_t { Head, Task, Item, Event, Note };
  Kind kind = Kind::Note;
  bool done = false;  // Task only
  std::string time;   // Event only: "9:05am", "all day"
  std::string text;
};

// One screen: "Tasks", "Projects", "Calendar".
struct PlanScreen {
  std::string title;
  std::vector<PlanRow> rows;
};

// Endpoint URL plus the last plan fetched from it (desk-voice /api/plan).
//
// Like the old agent-limits store: the snapshot is persisted so the screens
// paint instantly from the card, and fetching is explicit, because bringing up
// WiFi costs seconds and battery.
class PlanStore : public PersistableStore<PlanStore> {
 public:
  static constexpr size_t MAX_SCREENS = 6;
  static constexpr size_t MAX_ROWS = 100;
  static constexpr size_t MAX_URL_LENGTH = 200;

  static const char* getFilePath() { return "/.crosspoint/plan.json"; }
  void toJson(JsonDocument& doc) const;
  bool fromJson(JsonVariantConst doc);

  [[nodiscard]] const std::string& getUrl() const { return url; }
  void setUrl(std::string value);

  [[nodiscard]] const std::vector<PlanScreen>& getScreens() const { return screens; }
  // Epoch seconds of the last successful fetch; 0 when never fetched.
  [[nodiscard]] uint32_t getUpdatedAt() const { return updatedAt; }

  // Replace the cached plan from an endpoint response body. Returns false when
  // the body is not the expected shape; the previous plan is kept.
  bool applyResponse(const std::string& body, uint32_t fetchedAt);

 private:
  PlanStore() = default;
  friend class PersistableStore<PlanStore>;

  // Disk and network carry the same shape, so the parse lives in one place.
  static bool parseScreens(JsonArrayConst array, std::vector<PlanScreen>& out);

  std::string url;
  std::vector<PlanScreen> screens;
  uint32_t updatedAt = 0;
};

#define PLAN PlanStore::getInstance()
