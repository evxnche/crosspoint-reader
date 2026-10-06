#pragma once

#include <string>
#include <vector>

#include "activities/Activity.h"
#include "components/themes/BaseTheme.h"  // Rect

// The desk pal's plan on the reader: Tasks, Projects and Calendar, one screen
// each, with a tab strip naming them. The screens and their rows come from the
// endpoint (desk-voice /api/plan); this only draws them.
//
// Front Left/Right switch screens, the side buttons page within one, Confirm
// refreshes and holding Confirm edits the endpoint URL. Paged rather than
// scrolled: a page flip is one e-ink refresh.
class PlanActivity final : public Activity {
 public:
  PlanActivity(GfxRenderer& renderer, MappedInputManager& mappedInput) : Activity("Plan", renderer, mappedInput) {}

  void onEnter() override;
  void onExit() override;
  void loop() override;
  void render(RenderLock&&) override;
  bool preventAutoSleep() override { return refreshing; }

 private:
  // One row wrapped to the body width, so pagination knows its height.
  struct Block {
    int row = -1;  // index into the screen's rows; -1 = message
    std::vector<std::string> lines;
    int height = 0;
  };

  void rebuildBlocks();
  void paginate();
  int tabsTop() const;
  int bodyTop() const;
  int controlsTop() const;
  int textX(int row) const;
  Rect tabRect(int index) const;
  Rect refreshRect() const;
  Rect endpointRect() const;
  void drawTabs() const;
  void drawControls() const;
  void drawBlock(const Block& block, int y) const;

  void showScreen(int index);
  void turnPage(int direction);
  void startRefresh();
  void onWifiReady(bool connected);
  void editEndpoint();

  int screenIndex = 0;
  std::vector<Block> blocks;
  // Index of the first block on each page, plus a trailing end sentinel.
  std::vector<int> pageStarts;
  int currentPage = 0;

  bool wifiStarted = false;
  bool refreshing = false;
  std::string statusText;
};
