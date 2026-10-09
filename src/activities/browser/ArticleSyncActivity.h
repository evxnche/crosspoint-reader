#pragma once

#include <string>
#include <vector>

#include "OpdsServerStore.h"
#include "activities/Activity.h"

// Downloads every article saved to the Reader Inbox that this device has not
// fetched before, then opens the article lists: X posts in /Articles/X, web
// pages in /Articles/Web, each shown newest first.
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

  static constexpr const char* FOLDER = "/Articles";

  // The configured inbox, if any: the first OPDS server whose URL ends in /api/opds.
  static bool findInboxServer(OpdsServer& out);

  // In an article list folder, reorders `files` newest download first; names
  // this device never downloaded keep their order after them. Elsewhere a no-op.
  static void sortNewestFirst(const std::string& folder, std::vector<std::string>& files);

  void onEnter() override;
  void onExit() override;
  void loop() override;
  void render(RenderLock&&) override;
  bool preventAutoSleep() override { return state == State::Connecting || state == State::Working; }

 private:
  enum class State { Connecting, Working, Done };

  struct Pending {
    std::string id;
    std::string title;
    std::string href;
    const char* folder;
  };

  void onWifiReady(bool connected);
  void sync();
  bool fetchPending(std::vector<Pending>& out);
  bool pollCancel();
  std::string destinationFor(const Pending& article) const;
  void loadSynced();
  void saveSynced() const;
  void openArticles();

  OpdsServer server;
  State state = State::Connecting;
  bool opened = false;
  std::string statusLine;

  // Progress of the current run.
  int total = 0;
  int current = 0;
  std::string currentTitle;
  size_t bytesDone = 0;
  size_t bytesTotal = 0;

  std::vector<std::string> synced;  // ids already downloaded, oldest first
  bool cancel = false;
};
