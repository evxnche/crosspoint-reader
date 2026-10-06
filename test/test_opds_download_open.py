"""Run with python3 test/test_opds_download_open.py (requires a host C++ compiler)."""

from pathlib import Path
import subprocess
import tempfile

ROOT = Path(__file__).resolve().parents[1]
source = (ROOT / "src/activities/browser/OpdsBookBrowserActivity.cpp").read_text()


def method(name):
    start = source.index(f"void OpdsBookBrowserActivity::{name}(")
    brace = source.index("{", start)
    depth = 1
    end = brace + 1
    while depth:
        depth += (source[end] == "{") - (source[end] == "}")
        end += 1
    return source[start:end]


# Compile the production transition methods; fake only device I/O and the
# ActivityManager queue. The filename policy is the real production function.
harness = r"""
#include <cassert>
#include <cstring>
#include <functional>
#include <memory>
#include <string>
#include <vector>
#include "OpdsFilename.h"

#define LOG_DBG(...) ((void)0)
#define LOG_ERR(...) ((void)0)
#define LOG_INF(...) ((void)0)
enum { STR_LOADING, STR_DOWNLOAD_FAILED, STR_MEMORY_ERROR };
const char* tr(int) { return "status"; }
unsigned long millis() { return 100; }
void delay(unsigned) {}
struct RenderLock { template<class T> explicit RenderLock(T&) {} };
struct BoardConfig {
  static inline bool x4pro = true;
  static bool isX4Pro() { return x4pro; }
};
constexpr int WIFI_MODE_NULL = 0, WIFI_OFF = 0;
struct FakeWifi {
  int modeValue = 1;
  bool poweredOff = false;
  int getMode() { return modeValue; }
  void disconnect(bool off) { poweredOff = off; }
  void mode(int value) { modeValue = value; }
} WiFi;
int restarts = 0;
void silentRestart() { ++restarts; }
struct FakeEsp {
  unsigned getFreeHeap() { return 100000; }
  unsigned getMaxAllocHeap() { return 80000; }
} ESP;
struct FontCacheManager { void releaseSdFontCaches() {} };
struct GfxRenderer { FontCacheManager* getFontCacheManager() { return nullptr; } };
struct MappedInputManager {
  enum class Button { Back };
  void update(bool) {}
  bool wasReleased(Button) { return false; }
  bool wasHomeGesture() { return false; }
};
struct Settings {
  char opdsDownloadFolder[64]{};
  unsigned char opdsFilenameFormat = 0;
} SETTINGS;
struct StorageStub {
  bool exists(const char*) { return true; }
  bool mkdir(const char*) { return true; }
} Storage;
int cacheClears = 0, dirtyMarks = 0;
void clearBookCache(const std::string&) { ++cacheClears; }
namespace library { void markLibraryIndexDirty() { ++dirtyMarks; } }
namespace UrlUtils {
std::string buildUrl(const std::string& base, const std::string& relative) { return base + relative; }
}
struct OpdsEntry { std::string title, author, href; };
struct OpdsServer { std::string url, username, password; };
struct ReaderActivity {
  static inline bool failCreate = false;
  std::string path;
  static std::unique_ptr<ReaderActivity> create(GfxRenderer&, MappedInputManager&, std::string path, bool) {
    if (failCreate) return nullptr;
    auto reader = std::make_unique<ReaderActivity>();
    reader->path = std::move(path);
    return reader;
  }
};
struct ActivityManager {
  std::string openedPath;
  void replaceActivity(std::unique_ptr<ReaderActivity> reader) { openedPath = reader->path; }
  void goToReader(std::string path) { openedPath = std::move(path); }
} activityManager;
struct Activity {
  void onExit() {}
  void onSelectBook(const std::string& path) { activityManager.goToReader(path); }
};
struct HttpDownloader {
  enum DownloadError { OK, HTTP_ERROR, FILE_ERROR, ABORTED };
  static inline DownloadError result = OK;
  static constexpr unsigned MIN_TLS_FREE_HEAP = 40000, MIN_TLS_MAX_ALLOC = 20000;
  static DownloadError downloadToFile(const std::string&, const std::string&,
      std::function<void(size_t,size_t)>, bool*, const std::string&, const std::string&) { return result; }
};
constexpr int DOWNLOAD_PROGRESS_STEP_PERCENT = 5;
constexpr unsigned long DOWNLOAD_PROGRESS_MIN_UPDATE_MS = 5000;
struct OpdsBookBrowserActivity : Activity {
  enum class BrowserState { DOWNLOADING, LOADING, ERROR };
  BrowserState state = BrowserState::LOADING;
  std::string statusMessage, errorMessage, currentPath;
  size_t downloadProgress = 0, downloadTotal = 0;
  bool cancelDownload = false, goHomeAfterCancel = false, openingDownloadedBook = false;
  std::vector<OpdsEntry> entries;
  std::vector<std::string> navigationHistory;
  OpdsServer server;
  GfxRenderer renderer;
  MappedInputManager mappedInput;
  int feedFetches = 0;
  void requestUpdate(bool = false) {}
  void releaseEntries() { entries.clear(); }
  void routeTouch(MappedInputManager&) {}
  void fetchFeed(const std::string&) { ++feedFetches; }
  void onGoHome() {}
  void downloadBook(const OpdsEntry&);
  void onExit();
};
""" + method("downloadBook") + "\n" + method("onExit") + r"""
void reset() {
  SETTINGS = Settings{};
  WiFi = FakeWifi{};
  BoardConfig::x4pro = true;
  ReaderActivity::failCreate = false;
  HttpDownloader::result = HttpDownloader::OK;
  activityManager.openedPath.clear();
  restarts = cacheClears = dirtyMarks = 0;
}
int main() {
  const OpdsEntry article{"Saved article", "Author", "/api/epub?id=1"};
  reset();
  OpdsBookBrowserActivity success;
  success.downloadBook(article);
  assert(activityManager.openedPath == "/Author - Saved article.epub");
  assert(success.feedFetches == 0 && dirtyMarks == 1 && cacheClears == 1);
  success.onExit();
  assert(restarts == 0 && WiFi.modeValue == WIFI_OFF && WiFi.poweredOff);

  reset();
  std::strcpy(SETTINGS.opdsDownloadFolder, "/Articles");
  OpdsBookBrowserActivity folder;
  folder.downloadBook(article);
  assert(activityManager.openedPath == "/Articles/Author - Saved article.epub");

  reset();
  HttpDownloader::result = HttpDownloader::HTTP_ERROR;
  OpdsBookBrowserActivity failure;
  failure.downloadBook(article);
  assert(activityManager.openedPath.empty() && failure.state == OpdsBookBrowserActivity::BrowserState::ERROR);
  assert(dirtyMarks == 0);

  reset();
  HttpDownloader::result = HttpDownloader::ABORTED;
  OpdsBookBrowserActivity cancelled;
  cancelled.downloadBook(article);
  assert(activityManager.openedPath.empty() && cancelled.feedFetches == 1);

  reset();
  ReaderActivity::failCreate = true;
  OpdsBookBrowserActivity allocationFailure;
  allocationFailure.downloadBook(article);
  assert(activityManager.openedPath.empty() && allocationFailure.state == OpdsBookBrowserActivity::BrowserState::ERROR);
  assert(!allocationFailure.errorMessage.empty());

  reset();
  BoardConfig::x4pro = false;
  OpdsBookBrowserActivity constrained;
  constrained.downloadBook(article);
  assert(activityManager.openedPath.empty() && constrained.feedFetches == 1);
  constrained.onExit();
  assert(restarts == 1);
}
"""

with tempfile.TemporaryDirectory(prefix="cpr-opds-open-") as folder:
    folder = Path(folder)
    cpp = folder / "download.cpp"
    binary = folder / "download"
    cpp.write_text(harness)
    subprocess.run([
        "c++", "-std=c++20", "-I", str(ROOT / "src/util"), "-I", str(ROOT / "lib/Utf8"),
        str(cpp), str(ROOT / "src/util/OpdsFilename.cpp"), str(ROOT / "src/util/StringUtils.cpp"),
        str(ROOT / "lib/Utf8/Utf8.cpp"), "-o", str(binary),
    ], check=True)
    subprocess.run([str(binary)], check=True)
print("PASS: inbox download opens its saved article; teardown, failure, cancel and constrained-board paths checked")
