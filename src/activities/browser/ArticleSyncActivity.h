#pragma once

#include <string>
#include <vector>

#include "OpdsServerStore.h"
#include "activities/Activity.h"

// Downloads every article saved to the Reader Inbox that this device has not
// fetched before, into /Articles.
//
// The inbox is configured as an ordinary OPDS server (its URL ends in
// /api/opds), so the same entry also browses it in the catalog browser; this
// screen reads the sibling /api/articles list instead and fetches the EPUBs
// it does not have. Downloaded ids are remembered, so an article deleted on
// the device is not fetched again.
class ArticleSyncActivity final : public Activity {
 public:
  ArticleSyncActivity(GfxRenderer& renderer, MappedInputManager& mappedInput, OpdsServer server)
      : Activity("ArticleSync", renderer, mappedInput), server(std::move(server)) {}

  // The configured inbox, if any: the first OPDS server whose URL ends in /api/opds.
  static bool findInboxServer(OpdsServer& out);

  void onEnter() override;
  void onExit() override;
  void loop() override;
  void render(RenderLock&&) override;
  bool preventAutoSleep() override { return state == State::Connecting || state == State::Working; }

 private:
  enum class State { Connecting, Working, Done, Error };

  struct Pending {
    std::string id;
    std::string title;
    std::string href;
  };

  void onWifiReady(bool connected);
  void sync();
  bool fetchPending(std::vector<Pending>& out);
  std::string destinationFor(const Pending& article) const;
  void loadSynced();
  void saveSynced() const;
  void fail(const char* message);

  OpdsServer server;
  State state = State::Connecting;
  std::string statusLine;
  std::string errorText;

  // Progress of the current run.
  int total = 0;
  int current = 0;
  std::string currentTitle;
  size_t bytesDone = 0;
  size_t bytesTotal = 0;
  std::vector<std::string> fetchedTitles;
  int failures = 0;

  std::vector<std::string> synced;  // ids already downloaded, oldest first
  bool cancel = false;
};
