#pragma once
#include <string>
#include <vector>

#include "../HighlightEntry.h"

// Per-book highlight persistence at /.crosspoint/highlights/<flattened book path>.json.
// Each file also records the book's path and title so the Home Highlights screen
// can list every highlighted book without opening the EPUBs.
namespace HighlightFile {

struct BookSummary {
  std::string bookPath;
  std::string title;
  uint16_t count = 0;
};

// Loads bookPath's highlights sorted by position. A missing file yields an empty
// list and returns false.
bool load(const std::string& bookPath, std::vector<HighlightEntry>& highlights);

// Saves the highlights (removing the file when the list is empty).
bool save(const std::string& bookPath, const std::string& title, const std::vector<HighlightEntry>& highlights);

// Every book with at least one highlight, sorted by title.
std::vector<BookSummary> listBooks();

// Adds `entry` to `highlights` (kept sorted). A selection that lies inside one
// existing highlight removes that highlight instead; overlapping highlights are
// merged into one. Returns true when a highlight was added or merged, false
// when one was removed.
bool addOrToggle(std::vector<HighlightEntry>& highlights, HighlightEntry entry);

}  // namespace HighlightFile
