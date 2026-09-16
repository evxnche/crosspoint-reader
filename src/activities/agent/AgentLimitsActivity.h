#pragma once

#include <string>
#include <vector>

#include "activities/UiListActivity.h"

// Shows how much of each coding-agent usage window is spent.
//
// The values come from a URL the user points at their own machine or a small
// endpoint they host; this screen only fetches and displays them. It paints
// from the cached values first and fetches only when asked, so opening it is
// instant and the radio stays off unless the user wants fresh numbers.
class AgentLimitsActivity final : public UiListActivity {
 public:
  AgentLimitsActivity(GfxRenderer& renderer, MappedInputManager& mappedInput);

  void onEnter() override;
  void onExit() override;

 private:
  int listCount() const override { return static_cast<int>(rowItems_.size()); }
  void buildScreen(UiScreen& screen) override;
  void activateIndex(int index) override;
  const char* headerTitle() const override;

  void rebuildRows();
  void startRefresh();
  void onWifiReady(bool connected);
  void editEndpoint();

  // Index of the two trailing action rows, or -1 when they are not shown.
  int refreshRowIndex = -1;
  int endpointRowIndex = -1;

  bool wifiStarted = false;
  // Set while a fetch is in flight so the row reads as busy rather than stale.
  bool refreshing = false;
  std::string statusText;

  std::vector<std::string> rowLabels_;
  std::vector<std::string> rowValues_;
  std::vector<freeink::ui::ListItem> rowItems_;
};
