#pragma once
#include <ArduinoJson.h>
#include <PersistableStore.h>

#include <string>
#include <vector>

// One usage window within a provider, e.g. Claude's rolling 5 hours.
struct AgentWindow {
  std::string label;    // "5-hr", "Weekly"
  int usedPercent = 0;  // 0-100
  std::string resets;   // free text, e.g. "2h 36m"
};

// One provider and every window it reports.
struct AgentProvider {
  std::string name;    // "Claude", "Codex"
  std::string status;  // "live", "3h old", "not connected"
  std::vector<AgentWindow> windows;
};

// Endpoint URL plus the last snapshot fetched from it.
//
// The snapshot is persisted so the screen paints instantly from the card and
// shows how old the numbers are. Fetching is explicit: bringing up WiFi costs
// seconds and battery, and a reader should not do that on its own.
class AgentLimitsStore : public PersistableStore<AgentLimitsStore> {
 public:
  // Providers with nothing to report are still listed, so a signed-out one
  // reads as signed out rather than silently vanishing.
  static constexpr size_t MAX_PROVIDERS = 12;
  static constexpr size_t MAX_WINDOWS = 4;
  static constexpr size_t MAX_URL_LENGTH = 200;

  static const char* getFilePath() { return "/.crosspoint/agent_limits.json"; }
  void toJson(JsonDocument& doc) const;
  bool fromJson(JsonVariantConst doc);

  [[nodiscard]] const std::string& getUrl() const { return url; }
  void setUrl(std::string value);

  [[nodiscard]] const std::vector<AgentProvider>& getProviders() const { return providers; }
  // Epoch seconds of the last successful fetch; 0 when never fetched.
  [[nodiscard]] uint32_t getUpdatedAt() const { return updatedAt; }
  // Free-text status line from the endpoint, e.g. "CodexBar · live".
  [[nodiscard]] const std::string& getSubtitle() const { return subtitle; }

  // Replace the cached snapshot from an endpoint response body. Returns false
  // when the body is not the expected shape; the previous values are kept.
  bool applyResponse(const std::string& body, uint32_t fetchedAt);

 private:
  AgentLimitsStore() = default;
  friend class PersistableStore<AgentLimitsStore>;

  // Shared by fromJson (disk) and applyResponse (network): the two carry the
  // same shape, so the parse lives in one place.
  static bool parseProviders(JsonArrayConst array, std::vector<AgentProvider>& out);

  std::string url;
  std::string subtitle;
  std::vector<AgentProvider> providers;
  uint32_t updatedAt = 0;
};

#define AGENT_LIMITS AgentLimitsStore::getInstance()
