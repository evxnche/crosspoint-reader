#pragma once
#include <cstddef>
#include <cstdint>
#include <string>

// One highlighted passage in an EPUB. The range is in visible-codepoint offsets
// within its spine item (TextBlock::wordVisibleOffset), so it survives font,
// margin and orientation changes.
struct HighlightEntry {
  static constexpr std::size_t MAX_TEXT_BYTES = 600;

  uint16_t spineIndex = 0;
  uint32_t start = 0;  // offset of the first highlighted word
  uint32_t end = 0;    // offset just past the last highlighted word
  std::string text;    // the highlighted words, shown in the Highlights list
  float percentage = 0.0f;  // book progress at the highlight (0.0 to 1.0)

  bool overlaps(const uint16_t spine, const uint32_t rangeStart, const uint32_t rangeEnd) const {
    return spineIndex == spine && start < rangeEnd && rangeStart < end;
  }
};
