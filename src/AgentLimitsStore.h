#pragma once
#include <ArduinoJson.h>
#include <PersistableStore.h>

#include <string>
#include <vector>

// One usage meter, as reported by the endpoint.
struct AgentLimit {
  std::string name;    // "Claude Code 5h", "Weekly", ...
  int used = 0;        // consumed so far, in `unit`
  int total = 0;       // the cap; 0 means "no cap, just show `used`"
  std::string unit;    // "%", "req", "tok" — shown after the numbers
  std::string resets;  // free text, e.g. "3h 12m" or "Mon 09:00"
};

// Endpoint URL plus the last values fetched from it.
//
// The values are persisted so the screen paints instantly from the card and
// the sleep screen can show them with the radio off. Fetching is explicit:
// bringing up WiFi costs seconds and battery, and a reader should not do that
// on its own.
class AgentLimitsStore : public PersistableStore<AgentLimitsStore> {
 public:
  static constexpr size_t MAX_LIMITS = 6;
  static constexpr size_t MAX_URL_LENGTH = 200;

  static const char* getFilePath() { return "/.crosspoint/agent_limits.json"; }
  void toJson(JsonDocument& doc) const;
  bool fromJson(JsonVariantConst doc);

  [[nodiscard]] const std::string& getUrl() const { return url; }
  void setUrl(std::string value);

  [[nodiscard]] const std::vector<AgentLimit>& getLimits() const { return limits; }
  // Epoch seconds of the last successful fetch; 0 when never fetched.
  [[nodiscard]] uint32_t getUpdatedAt() const { return updatedAt; }
  // Free-text status line from the endpoint, e.g. a plan name.
  [[nodiscard]] const std::string& getSubtitle() const { return subtitle; }

  // Replace the cached values from an endpoint response body. Returns false
  // when the body is not the expected shape; the previous values are kept.
  bool applyResponse(const std::string& body, uint32_t fetchedAt);

 private:
  AgentLimitsStore() = default;
  friend class PersistableStore<AgentLimitsStore>;

  std::string url;
  std::string subtitle;
  std::vector<AgentLimit> limits;
  uint32_t updatedAt = 0;
};

#define AGENT_LIMITS AgentLimitsStore::getInstance()
