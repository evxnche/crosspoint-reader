#pragma once

#include <string>
#include <vector>

#include "activities/Activity.h"
#include "components/themes/BaseTheme.h"  // Rect
#include "util/ButtonNavigator.h"

// Shows how much of each coding-agent usage window is spent.
//
// Laid out as one card per provider -- a name, then a bar per window -- because
// the point of the screen is a glance, and a flat list of "Claude session 60%"
// rows makes the eye do the grouping the layout should have done.
//
// Paged rather than scrolled: an e-ink panel redraws too slowly for smooth
// scrolling, and a page flip is one refresh.
class AgentLimitsActivity final : public Activity {
 public:
  AgentLimitsActivity(GfxRenderer& renderer, MappedInputManager& mappedInput)
      : Activity("AgentLimits", renderer, mappedInput) {}

  void onEnter() override;
  void onExit() override;
  void loop() override;
  void render(RenderLock&&) override;
  bool preventAutoSleep() override { return refreshing; }

 private:
  // One drawable unit. Cards are split into blocks so a provider can break
  // across a page boundary without a window's label parting from its bar.
  struct Block {
    enum class Kind : uint8_t { ProviderHeader, Window, Status, Message };
    Kind kind = Kind::Message;
    int height = 0;
    int providerIndex = -1;
    int windowIndex = -1;
    std::string text;       // header/status/message text
    std::string rightText;  // right-aligned companion (status, reset time)
  };

  void rebuildBlocks();
  void paginate();
  int bodyTop() const;
  int bodyHeight() const;
  // Touch devices get on-screen controls: drawButtonHints() draws nothing when
  // the panel has touch, so physical-button hints alone leave Refresh and
  // Endpoint with no visible or reachable trigger.
  bool usesOnScreenControls() const;
  int controlsTop() const;
  Rect refreshRect() const;
  Rect endpointRect() const;
  void drawControls() const;
  void drawBlock(const Block& block, int x, int y, int width) const;
  void drawBar(int x, int y, int width, int percent) const;

  void startRefresh();
  void onWifiReady(bool connected);
  void editEndpoint();

  std::vector<Block> blocks;
  // Index of the first block on each page, plus a trailing end sentinel.
  std::vector<int> pageStarts;
  int currentPage = 0;

  bool wifiStarted = false;
  bool refreshing = false;
  std::string statusText;

  ButtonNavigator buttonNavigator;
};
