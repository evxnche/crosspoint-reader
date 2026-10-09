#include "ArticleSyncActivity.h"

#include <ArduinoJson.h>
#include <BoardConfig.h>
#include <FontCacheManager.h>
#include <GfxRenderer.h>
#include <HalMemory.h>
#include <HalStorage.h>
#include <I18n.h>
#include <Logging.h>
#include <Memory.h>
#include <WiFi.h>

#include <algorithm>
#include <cstdio>
#include <cstring>
#include <numeric>

#include "MappedInputManager.h"
#include "SilentRestart.h"
#include "activities/network/WifiSelectionActivity.h"
#include "components/UITheme.h"
#include "fontIds.h"
#include "network/HttpDownloader.h"
#include "util/BookCacheUtils.h"
#include "util/StringUtils.h"
#include "util/UrlUtils.h"

namespace {
constexpr const char* TAG = "ARTSYNC";
constexpr const char* WEB_DIR = "/Articles/Web";
constexpr const char* X_DIR = "/Articles/X";
constexpr const char* SYNCED_PATH = "/.crosspoint/articles_synced.txt";
// Every downloaded article's path, one per line, oldest first.
constexpr const char* ORDER_PATH = "/.crosspoint/articles_order.txt";
constexpr size_t ORDER_CHUNK = 512;
constexpr size_t ORDER_LINE_MAX = 256;
constexpr const char* OPDS_SUFFIX = "/api/opds";
// Remembered ids beyond this are dropped oldest-first; the server keeps 300.
constexpr size_t MAX_SYNCED = 600;
constexpr size_t MAX_TITLE_BYTES = 80;
constexpr unsigned long PROGRESS_MIN_MS = 700;

// The articles list lives beside the OPDS feed: .../api/opds -> .../api/articles.
std::string articlesUrl(const std::string& opdsUrl) {
  std::string base = opdsUrl;
  const size_t query = base.find('?');
  if (query != std::string::npos) base.resize(query);
  while (!base.empty() && base.back() == '/') base.pop_back();
  base.resize(base.size() - strlen(OPDS_SUFFIX));
  return base + "/api/articles";
}

bool isInboxUrl(const std::string& url) {
  std::string path = url.substr(0, url.find('?'));
  while (!path.empty() && path.back() == '/') path.pop_back();
  return path.size() > strlen(OPDS_SUFFIX) &&
         path.compare(path.size() - strlen(OPDS_SUFFIX), std::string::npos, OPDS_SUFFIX) == 0;
}
}  // namespace

bool ArticleSyncActivity::findInboxServer(OpdsServer& out) {
  for (const auto& s : OPDS_STORE.getServers()) {
    if (isInboxUrl(s.url)) {
      out = s;
      return true;
    }
  }
  return false;
}

void ArticleSyncActivity::sortNewestFirst(const std::string& folder, std::vector<std::string>& files) {
  std::string prefix = folder;
  while (prefix.size() > 1 && prefix.back() == '/') prefix.pop_back();
  if (prefix != WEB_DIR && prefix != X_DIR) return;
  prefix += '/';

  HalFile f;
  if (files.size() < 2 || !Storage.openFileForRead(TAG, ORDER_PATH, f)) return;
  auto buf = makeUniqueNoThrow<char[]>(ORDER_CHUNK + ORDER_LINE_MAX);
  if (!buf) {
    LOG_ERR(TAG, "OOM: order buffer");
    return;
  }
  char* chunk = buf.get();
  char* line = chunk + ORDER_CHUNK;

  // rank[i] = manifest line of files[i]; a later line is a newer download.
  std::vector<int> rank(files.size(), -1);
  int lineNo = 0;
  size_t len = 0;
  bool overlong = false;
  const auto takeLine = [&] {
    if (!overlong && len > prefix.size() && memcmp(line, prefix.data(), prefix.size()) == 0) {
      const std::string_view name(line + prefix.size(), len - prefix.size());
      for (size_t i = 0; i < files.size(); ++i) {
        if (files[i] == name) {
          rank[i] = lineNo;
          break;
        }
      }
    }
    ++lineNo;
    len = 0;
    overlong = false;
  };
  for (int n = f.read(chunk, ORDER_CHUNK); n > 0; n = f.read(chunk, ORDER_CHUNK)) {
    for (int i = 0; i < n; ++i) {
      if (chunk[i] == '\n') {
        takeLine();
      } else if (len < ORDER_LINE_MAX) {
        line[len++] = chunk[i];
      } else {
        overlong = true;
      }
    }
  }
  if (len > 0) takeLine();

  std::vector<size_t> order(files.size());
  std::iota(order.begin(), order.end(), 0);
  std::stable_sort(order.begin(), order.end(), [&rank](const size_t a, const size_t b) { return rank[a] > rank[b]; });
  std::vector<std::string> sorted;
  sorted.reserve(files.size());
  for (const size_t i : order) sorted.push_back(std::move(files[i]));
  files = std::move(sorted);
}

void ArticleSyncActivity::onEnter() {
  Activity::onEnter();
  {
    RenderLock lock(*this);
    state = State::Connecting;
    statusLine = tr(STR_CHECKING_WIFI);
  }
  requestUpdate();

  if (WiFi.status() == WL_CONNECTED && WiFi.localIP() != IPAddress(0, 0, 0, 0)) {
    onWifiReady(true);
    return;
  }
  startActivityForResult(std::make_unique<WifiSelectionActivity>(renderer, mappedInput),
                         [this](const ActivityResult& result) { onWifiReady(!result.isCancelled); });
}

void ArticleSyncActivity::onExit() {
  Activity::onExit();
  if (WiFi.getMode() != WIFI_MODE_NULL) {
    WiFi.disconnect(true);
    WiFi.mode(WIFI_OFF);
    delay(30);
    const auto heap = HalMemory::getInternalHeap();
    LOG_INF(TAG, "Wi-Fi stopped: internal free=%u max_block=%u", static_cast<unsigned>(heap.freeBytes),
            static_cast<unsigned>(heap.largestBlockBytes));
    if (!BoardConfig::isX4Pro() || heap.freeBytes < HttpDownloader::MIN_TLS_FREE_HEAP ||
        heap.largestBlockBytes < HttpDownloader::MIN_TLS_MAX_ALLOC)
      silentRestart();
  }
}

void ArticleSyncActivity::onWifiReady(const bool connected) {
  if (!connected) {
    LOG_ERR(TAG, "No Wi-Fi; opening the lists already on the card");
    openArticles();
    return;
  }
  sync();
}

// loop() makes the switch: this can run inside onEnter() or a result handler.
void ArticleSyncActivity::openArticles() {
  RenderLock lock(*this);
  state = State::Done;
}

void ArticleSyncActivity::loadSynced() {
  synced.clear();
  HalFile f;
  if (!Storage.openFileForRead(TAG, SYNCED_PATH, f)) return;
  synced.reserve(64);
  std::string line;
  while (f.available()) {
    const int c = f.read();
    if (c < 0) break;
    if (c == '\n') {
      if (!line.empty()) synced.push_back(line);
      line.clear();
    } else if (line.size() < 40) {
      line.push_back(static_cast<char>(c));
    }
  }
  if (!line.empty()) synced.push_back(line);
}

void ArticleSyncActivity::saveSynced() const {
  const size_t start = synced.size() > MAX_SYNCED ? synced.size() - MAX_SYNCED : 0;
  HalFile f;
  if (!Storage.openFileForWrite(TAG, SYNCED_PATH, f)) {
    LOG_ERR(TAG, "Cannot save synced ids");
    return;
  }
  for (size_t i = start; i < synced.size(); ++i) {
    f.write(reinterpret_cast<const uint8_t*>(synced[i].data()), synced[i].size());
    f.write(reinterpret_cast<const uint8_t*>("\n"), 1);
  }
}

bool ArticleSyncActivity::fetchPending(std::vector<Pending>& out) {
  std::string body;
  if (!HttpDownloader::fetchUrl(articlesUrl(server.url), body, server.username, server.password,
                                [this] { return pollCancel(); })) {
    LOG_ERR(TAG, "Fetching the article list failed");
    return false;
  }

  // Keep only the fields used here; the rest of each entry is never stored.
  JsonDocument filter;
  filter["articles"][0]["id"] = true;
  filter["articles"][0]["title"] = true;
  filter["articles"][0]["epub"] = true;
  filter["articles"][0]["site"] = true;
  JsonDocument doc;
  const DeserializationError err = deserializeJson(doc, body, DeserializationOption::Filter(filter));
  body.clear();
  body.shrink_to_fit();
  if (err) {
    LOG_ERR(TAG, "Bad article list: %s", err.c_str());
    return false;
  }

  const JsonArrayConst articles = doc["articles"];
  out.clear();
  out.reserve(articles.size());
  for (JsonVariantConst a : articles) {
    Pending p;
    p.id = a["id"] | "";
    p.title = a["title"] | "";
    p.href = a["epub"] | "";
    p.folder = strcmp(a["site"] | "", "X") == 0 ? X_DIR : WEB_DIR;
    if (p.id.empty() || p.href.empty()) continue;
    if (std::find(synced.begin(), synced.end(), p.id) != synced.end()) continue;
    out.push_back(std::move(p));
  }
  return true;
}

bool ArticleSyncActivity::pollCancel() {
  mappedInput.update(true);
  if (mappedInput.wasReleased(MappedInputManager::Button::Back) || mappedInput.wasBackGesture()) cancel = true;
  return cancel;
}

std::string ArticleSyncActivity::destinationFor(const Pending& article) const {
  std::string name = StringUtils::sanitizeFilename(article.title.empty() ? article.id : article.title, MAX_TITLE_BYTES);
  if (name.empty()) name = article.id;
  std::string path = std::string(article.folder) + "/" + name + ".epub";
  // Two different articles with the same title keep both files.
  if (Storage.exists(path.c_str()))
    path = std::string(article.folder) + "/" + name + " " + article.id.substr(0, 6) + ".epub";
  return path;
}

void ArticleSyncActivity::sync() {
  {
    RenderLock lock(*this);
    state = State::Working;
    statusLine = tr(STR_ARTICLES_CHECKING);
    total = current = 0;
  }
  requestUpdate(true);

  for (const char* dir : {FOLDER, WEB_DIR, X_DIR}) {
    if (!Storage.exists(dir)) Storage.mkdir(dir);
  }
  loadSynced();
  std::vector<Pending> pending;
  if (!fetchPending(pending)) {
    if (cancel) {
      finish();
      return;
    }
    openArticles();
    return;
  }
  // Rebuildable SD-font caches give TLS the room a multi-MB transfer needs.
  {
    RenderLock lock(*this);
    if (auto* fcm = renderer.getFontCacheManager()) fcm->releaseSdFontCaches();
  }

  {
    RenderLock lock(*this);
    total = static_cast<int>(pending.size());
  }

  for (size_t i = 0; i < pending.size() && !cancel; ++i) {
    const Pending& article = pending[i];
    if (ESP.getFreeHeap() < HttpDownloader::MIN_TLS_FREE_HEAP ||
        ESP.getMaxAllocHeap() < HttpDownloader::MIN_TLS_MAX_ALLOC) {
      LOG_ERR(TAG, "Low heap (%u free, %u max block)", ESP.getFreeHeap(), ESP.getMaxAllocHeap());
      break;
    }
    {
      RenderLock lock(*this);
      current = static_cast<int>(i) + 1;
      currentTitle = article.title;
      bytesDone = bytesTotal = 0;
    }
    requestUpdate(true);

    const std::string dest = destinationFor(article);
    // Downloaded under a temporary name and renamed when complete, so a power
    // cut mid-transfer leaves no half-written book in /Articles.
    const std::string part = dest + ".part";
    Storage.remove(part.c_str());
    unsigned long lastDraw = 0;
    auto result = HttpDownloader::downloadToFile(
        UrlUtils::buildUrl(server.url, article.href), part,
        [this, &lastDraw](const size_t done, const size_t size) {
          {
            RenderLock lock(*this);
            bytesDone = done;
            bytesTotal = size;
          }
          const unsigned long now = millis();
          if (now - lastDraw >= PROGRESS_MIN_MS || (size > 0 && done >= size)) {
            lastDraw = now;
            requestUpdate(true);
          }
        },
        &cancel, server.username, server.password, false, [this] { return pollCancel(); });
    if (result == HttpDownloader::OK && !Storage.rename(part.c_str(), dest.c_str())) {
      LOG_ERR(TAG, "Rename failed: %s", part.c_str());
      Storage.remove(part.c_str());
      result = HttpDownloader::FILE_ERROR;
    }

    if (result == HttpDownloader::OK) {
      clearBookCache(dest);
      synced.push_back(article.id);
      // Saved per article, so a run cut short (power off, a crash) does not
      // download everything again next time under " <id>" names.
      saveSynced();
      if (HalFile order = Storage.open(ORDER_PATH, O_WRONLY | O_CREAT | O_APPEND)) {
        order.write(dest.data(), dest.size());
        order.write("\n", 1);
      } else {
        LOG_ERR(TAG, "Cannot record article order");
      }
      LOG_INF(TAG, "Saved %s", dest.c_str());
    } else if (result != HttpDownloader::ABORTED) {
      LOG_ERR(TAG, "Download failed (%d): %s", static_cast<int>(result), article.id.c_str());
    }
  }

  if (cancel) {
    finish();
    return;
  }
  openArticles();
}

void ArticleSyncActivity::loop() {
  if (state != State::Done || opened) return;
  opened = true;
  activityManager.goToFileBrowser(FOLDER);
}

void ArticleSyncActivity::render(RenderLock&&) {
  const auto& metrics = UITheme::getInstance().getMetrics();
  const int pageWidth = renderer.getScreenWidth();
  const int x = metrics.contentSidePadding;
  const int width = pageWidth - x * 2;
  const int lineHeight = renderer.getLineHeight(UI_10_FONT_ID) + 6;

  renderer.clearScreen();
  GUI.drawHeader(renderer, Rect{0, metrics.topPadding, pageWidth, metrics.headerHeight}, tr(STR_ARTICLES), nullptr);
  int y = metrics.topPadding + metrics.headerHeight + metrics.verticalSpacing + lineHeight;

  char buf[96];
  switch (state) {
    case State::Connecting:
      renderer.drawText(UI_10_FONT_ID, x, y, statusLine.c_str());
      break;

    case State::Working:
      if (total == 0) {
        renderer.drawText(UI_10_FONT_ID, x, y, statusLine.c_str());
        break;
      }
      snprintf(buf, sizeof(buf), tr(STR_ARTICLES_PROGRESS), current, total);
      renderer.drawText(UI_10_FONT_ID, x, y, buf, true, EpdFontFamily::BOLD);
      y += lineHeight;
      renderer.drawText(UI_10_FONT_ID, x, y,
                        renderer.truncatedText(UI_10_FONT_ID, currentTitle.c_str(), width).c_str());
      y += lineHeight / 2;
      GUI.drawProgressBar(renderer, Rect{x, y, width, 12}, bytesDone, bytesTotal > 0 ? bytesTotal : 1);
      break;

    case State::Done:
      break;
  }

  const auto labels = mappedInput.mapLabels(tr(STR_BACK), "", "", "");
  GUI.drawButtonHints(renderer, labels.btn1, labels.btn2, labels.btn3, labels.btn4);
  renderer.displayBuffer();
}
