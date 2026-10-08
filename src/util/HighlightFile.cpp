#include "HighlightFile.h"

#include <ArduinoJson.h>
#include <FsHelpers.h>
#include <HalStorage.h>
#include <Logging.h>
#include <PersistableStore.h>

#include <algorithm>
#include <cstring>

namespace {

constexpr const char* HIGHLIGHTS_DIR = "/.crosspoint/highlights";
constexpr const char* ELLIPSIS = "\xE2\x80\xA6";

std::string highlightPath(const std::string& bookPath) {
  // Flat filename from the book path, same scheme as BookmarkUtil.
  std::string name = bookPath.empty() ? std::string() : bookPath.substr(1);
  std::replace(name.begin(), name.end(), '/', '_');
  std::replace(name.begin(), name.end(), '\\', '_');
  const size_t lastDot = name.find_last_of('.');
  if (lastDot != std::string::npos) name.erase(lastDot);
  return std::string(HIGHLIGHTS_DIR) + "/" + name + ".json";
}

bool positionLess(const HighlightEntry& a, const HighlightEntry& b) {
  return a.spineIndex != b.spineIndex ? a.spineIndex < b.spineIndex : a.start < b.start;
}

}  // namespace

bool HighlightFile::load(const std::string& bookPath, std::vector<HighlightEntry>& highlights) {
  highlights.clear();
  const std::string path = highlightPath(bookPath);
  if (!Storage.exists(path.c_str())) return false;

  JsonDocument doc;
  if (!PersistableStoreBase::readDocFromFile(path.c_str(), doc)) return false;

  JsonArray arr = doc["highlights"].as<JsonArray>();
  highlights.reserve(arr.size());
  for (JsonObject obj : arr) {
    HighlightEntry entry;
    entry.spineIndex = obj["s"] | static_cast<uint16_t>(0);
    entry.start = obj["a"] | static_cast<uint32_t>(0);
    entry.end = obj["b"] | static_cast<uint32_t>(0);
    entry.text = obj["t"] | "";
    entry.percentage = obj["p"] | 0.0f;
    if (entry.end <= entry.start) continue;
    highlights.push_back(std::move(entry));
  }
  std::sort(highlights.begin(), highlights.end(), positionLess);
  LOG_DBG("HLT", "Loaded %zu highlights", highlights.size());
  return true;
}

bool HighlightFile::save(const std::string& bookPath, const std::string& title,
                         const std::vector<HighlightEntry>& highlights) {
  const std::string path = highlightPath(bookPath);
  if (highlights.empty()) {
    if (Storage.exists(path.c_str())) Storage.remove(path.c_str());
    return true;
  }

  JsonDocument doc;
  doc["book"] = bookPath;
  doc["title"] = title;
  JsonArray arr = doc["highlights"].to<JsonArray>();
  for (const auto& entry : highlights) {
    JsonObject obj = arr.add<JsonObject>();
    obj["s"] = entry.spineIndex;
    obj["a"] = entry.start;
    obj["b"] = entry.end;
    obj["t"] = entry.text;
    obj["p"] = entry.percentage;
  }

  // writeDocToFile ensures /.crosspoint; the highlights subdirectory is ours.
  Storage.mkdir(HIGHLIGHTS_DIR);
  return PersistableStoreBase::writeDocToFile(path.c_str(), doc);
}

std::vector<HighlightFile::BookSummary> HighlightFile::listBooks() {
  std::vector<BookSummary> books;
  auto dir = Storage.open(HIGHLIGHTS_DIR);
  if (!dir || !dir.isDirectory()) return books;
  dir.rewindDirectory();

  books.reserve(16);
  char name[256];
  for (auto entry = dir.openNextFile(); entry; entry = dir.openNextFile()) {
    if (entry.isDirectory()) continue;
    entry.getName(name, sizeof(name));
    const size_t len = strlen(name);
    if (strncmp(name, "._", 2) == 0 || len <= 5 || strcmp(name + len - 5, ".json") != 0) continue;
    entry.close();

    const std::string path = std::string(HIGHLIGHTS_DIR) + "/" + name;
    JsonDocument doc;
    if (!PersistableStoreBase::readDocFromFile(path.c_str(), doc)) continue;
    BookSummary book;
    book.bookPath = doc["book"] | "";
    book.title = doc["title"] | "";
    book.count = static_cast<uint16_t>(doc["highlights"].as<JsonArray>().size());
    if (book.bookPath.empty() || book.count == 0) continue;
    if (book.title.empty()) {
      const size_t slash = book.bookPath.find_last_of('/');
      book.title = slash == std::string::npos ? book.bookPath : book.bookPath.substr(slash + 1);
    }
    books.push_back(std::move(book));
  }
  std::sort(books.begin(), books.end(),
            [](const BookSummary& a, const BookSummary& b) { return FsHelpers::naturalLess(a.title, b.title); });
  return books;
}

bool HighlightFile::addOrToggle(std::vector<HighlightEntry>& highlights, HighlightEntry entry) {
  for (auto it = highlights.begin(); it != highlights.end(); ++it) {
    if (it->spineIndex == entry.spineIndex && it->start <= entry.start && entry.end <= it->end) {
      highlights.erase(it);
      return false;
    }
  }

  // Merge every overlapping highlight into the new one. Their words are not all
  // on hand, so the list text marks the parts beyond the new selection.
  uint32_t mergedStart = entry.start;
  uint32_t mergedEnd = entry.end;
  highlights.erase(std::remove_if(highlights.begin(), highlights.end(),
                                  [&](const HighlightEntry& existing) {
                                    if (!existing.overlaps(entry.spineIndex, entry.start, entry.end)) return false;
                                    mergedStart = std::min(mergedStart, existing.start);
                                    mergedEnd = std::max(mergedEnd, existing.end);
                                    return true;
                                  }),
                   highlights.end());
  if (mergedStart < entry.start) entry.text = ELLIPSIS + entry.text;
  if (mergedEnd > entry.end) entry.text += ELLIPSIS;
  entry.start = mergedStart;
  entry.end = mergedEnd;
  if (entry.text.size() > HighlightEntry::MAX_TEXT_BYTES) {
    // Cut on a UTF-8 boundary.
    size_t cut = HighlightEntry::MAX_TEXT_BYTES;
    while (cut > 0 && (static_cast<uint8_t>(entry.text[cut]) & 0xC0) == 0x80) cut--;
    entry.text.resize(cut);
    entry.text += ELLIPSIS;
  }

  highlights.insert(std::upper_bound(highlights.begin(), highlights.end(), entry, positionLess), std::move(entry));
  return true;
}
