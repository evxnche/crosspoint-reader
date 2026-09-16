#pragma once

#include <string>

#include "PdfTextExtractor.h"

// A PDF as the reader sees it: a book whose text is extracted once, cached on
// the card, and then read through the ordinary plain-text path.
//
// The extraction is deliberately one-shot. Re-running the parser on every page
// turn would make paging unusable on this hardware, and the reflowed text is
// what the user actually wants to keep.
class Pdf {
 public:
  Pdf(std::string path, std::string cacheBasePath);

  // Verify the file exists and prepare the cache directory.
  bool load();

  [[nodiscard]] const std::string& getPath() const { return filepath; }
  [[nodiscard]] const std::string& getCachePath() const { return cachePath; }
  // Reflowed UTF-8 produced by extract().
  [[nodiscard]] std::string getTextPath() const { return cachePath + "/text.txt"; }

  // True when a previous extraction is present and still matches the file.
  [[nodiscard]] bool isExtracted();

  // Parse the document and write the reflowed text. Safe to call when already
  // extracted; it does the work again.
  bool extract(PdfTextExtractor::ProgressFn progress, void* ctx);

  // /Info /Title from the last extraction, else the filename without ".pdf".
  [[nodiscard]] std::string getTitle();

  // Set after extract(): pages that produced no text at all. When this equals
  // the page count the document is a scan and there is nothing to read.
  [[nodiscard]] size_t emptyPages() const { return lastEmptyPages; }
  [[nodiscard]] size_t totalPages() const { return lastTotalPages; }

  void setupCacheDir() const;
  bool clearCache() const;

  // Cover image beside the PDF, mirroring the plain-text reader's convention.
  [[nodiscard]] std::string getCoverBmpPath() const { return cachePath + "/cover.bmp"; }
  [[nodiscard]] bool generateCoverBmp() const;

 private:
  struct Meta {
    uint32_t sourceSize = 0;
    uint32_t sourceTime = 0;
    uint32_t pageCount = 0;
    uint32_t emptyPages = 0;
    std::string title;
  };

  [[nodiscard]] std::string metaPath() const { return cachePath + "/pdf.meta"; }
  bool readMeta(Meta& out) const;
  bool writeMeta(const Meta& meta) const;
  [[nodiscard]] std::string findCoverImage() const;

  std::string filepath;
  std::string cacheBasePath;
  std::string cachePath;
  bool loaded = false;
  uint32_t fileSize = 0;
  uint32_t fileTime = 0;

  bool metaLoaded = false;
  Meta meta;
  size_t lastEmptyPages = 0;
  size_t lastTotalPages = 0;
};
