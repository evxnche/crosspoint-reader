#include "Pdf.h"

#include <FsHelpers.h>
#include <HalStorage.h>
#include <JpegToBmpConverter.h>
#include <Logging.h>
#include <Memory.h>

#include "PdfDocument.h"

namespace {
constexpr const char* TAG = "PDF";
constexpr uint32_t META_MAGIC = 0x50444631;  // "PDF1"
constexpr uint8_t META_VERSION = 1;
}  // namespace

Pdf::Pdf(std::string path, std::string cacheBase) : filepath(std::move(path)), cacheBasePath(std::move(cacheBase)) {
  const size_t hash = std::hash<std::string>{}(filepath);
  cachePath = cacheBasePath + "/pdf_" + std::to_string(hash);
}

bool Pdf::load() {
  if (loaded) return true;

  HalFile file;
  if (!Storage.openFileForRead(TAG, filepath, file)) {
    LOG_ERR(TAG, "Cannot open %s", filepath.c_str());
    return false;
  }
  fileSize = static_cast<uint32_t>(file.size());
  fileTime = file.modificationTime();
  file.close();

  setupCacheDir();
  loaded = true;
  return true;
}

void Pdf::setupCacheDir() const {
  if (!Storage.exists(cacheBasePath.c_str())) Storage.mkdir(cacheBasePath.c_str());
  if (!Storage.exists(cachePath.c_str())) Storage.mkdir(cachePath.c_str());
}

bool Pdf::readMeta(Meta& out) const {
  HalFile f;
  if (!Storage.openFileForRead(TAG, metaPath(), f)) return false;

  uint32_t magic = 0;
  uint8_t version = 0;
  if (f.read(&magic, sizeof(magic)) != static_cast<int>(sizeof(magic)) || magic != META_MAGIC) return false;
  if (f.read(&version, sizeof(version)) != static_cast<int>(sizeof(version)) || version != META_VERSION) return false;
  if (f.read(&out.sourceSize, sizeof(out.sourceSize)) != static_cast<int>(sizeof(out.sourceSize))) return false;
  if (f.read(&out.sourceTime, sizeof(out.sourceTime)) != static_cast<int>(sizeof(out.sourceTime))) return false;
  if (f.read(&out.pageCount, sizeof(out.pageCount)) != static_cast<int>(sizeof(out.pageCount))) return false;
  if (f.read(&out.emptyPages, sizeof(out.emptyPages)) != static_cast<int>(sizeof(out.emptyPages))) return false;

  uint16_t titleLen = 0;
  if (f.read(&titleLen, sizeof(titleLen)) != static_cast<int>(sizeof(titleLen))) return false;
  if (titleLen > 512) return false;
  if (titleLen > 0) {
    auto buf = makeUniqueNoThrow<char[]>(titleLen + 1);
    if (!buf) return false;
    if (f.read(buf.get(), titleLen) != static_cast<int>(titleLen)) return false;
    buf[titleLen] = '\0';
    out.title.assign(buf.get(), titleLen);
  }
  return true;
}

bool Pdf::writeMeta(const Meta& m) const {
  HalFile f;
  if (!Storage.openFileForWrite(TAG, metaPath(), f)) return false;

  const uint32_t magic = META_MAGIC;
  const uint8_t version = META_VERSION;
  f.write(&magic, sizeof(magic));
  f.write(&version, sizeof(version));
  f.write(&m.sourceSize, sizeof(m.sourceSize));
  f.write(&m.sourceTime, sizeof(m.sourceTime));
  f.write(&m.pageCount, sizeof(m.pageCount));
  f.write(&m.emptyPages, sizeof(m.emptyPages));

  const auto titleLen = static_cast<uint16_t>(std::min<size_t>(m.title.size(), 512));
  f.write(&titleLen, sizeof(titleLen));
  if (titleLen > 0) f.write(m.title.data(), titleLen);
  return true;
}

bool Pdf::isExtracted() {
  if (!Storage.exists(getTextPath().c_str())) return false;
  if (!metaLoaded) {
    metaLoaded = readMeta(meta);
    if (!metaLoaded) return false;
  }
  // A re-downloaded or edited file gets re-extracted. A zero timestamp means
  // the card gave us nothing usable, so size alone decides.
  if (meta.sourceSize != fileSize) return false;
  if (meta.sourceTime != 0 && fileTime != 0 && meta.sourceTime != fileTime) return false;

  lastTotalPages = meta.pageCount;
  lastEmptyPages = meta.emptyPages;
  return true;
}

bool Pdf::extract(const PdfTextExtractor::ProgressFn progress, void* ctx) {
  if (!load()) return false;
  setupCacheDir();

  PdfDocument doc;
  if (!doc.open(filepath, cachePath)) return false;

  const std::string title = doc.documentTitle();
  const size_t pageCount = doc.pageCount();

  PdfTextExtractor extractor;
  const PdfTextExtractor::Options options;
  const std::string tempPath = cachePath + "/text.tmp";
  if (!extractor.run(doc, tempPath, options, progress, ctx)) {
    Storage.remove(tempPath.c_str());
    doc.close();
    return false;
  }
  doc.close();

  // Publish atomically so an interrupted extraction never looks complete.
  Storage.remove(getTextPath().c_str());
  if (!Storage.rename(tempPath.c_str(), getTextPath().c_str())) {
    LOG_ERR(TAG, "Cannot publish extracted text");
    Storage.remove(tempPath.c_str());
    return false;
  }

  meta.sourceSize = fileSize;
  meta.sourceTime = fileTime;
  meta.pageCount = static_cast<uint32_t>(pageCount);
  meta.emptyPages = static_cast<uint32_t>(extractor.emptyPages());
  meta.title = title;
  metaLoaded = true;
  writeMeta(meta);

  lastTotalPages = pageCount;
  lastEmptyPages = extractor.emptyPages();
  LOG_INF(TAG, "Extracted %s: %u pages, %u chars", filepath.c_str(), static_cast<unsigned>(pageCount),
          static_cast<unsigned>(extractor.charactersWritten()));
  return true;
}

std::string Pdf::getTitle() {
  if (!metaLoaded) metaLoaded = readMeta(meta);
  if (metaLoaded && !meta.title.empty()) return meta.title;

  const size_t lastSlash = filepath.find_last_of('/');
  std::string filename = lastSlash != std::string::npos ? filepath.substr(lastSlash + 1) : filepath;
  if (FsHelpers::hasPdfExtension(filename)) filename.resize(filename.length() - 4);
  return filename;
}

bool Pdf::clearCache() const {
  if (!Storage.exists(cachePath.c_str())) return true;
  if (!Storage.removeDir(cachePath.c_str())) {
    LOG_ERR(TAG, "Failed to clear cache");
    return false;
  }
  return true;
}

std::string Pdf::findCoverImage() const {
  const size_t lastSlash = filepath.find_last_of('/');
  std::string folder = lastSlash != std::string::npos ? filepath.substr(0, lastSlash) : "";
  if (folder.empty()) folder = "/";

  std::string baseName = lastSlash != std::string::npos ? filepath.substr(lastSlash + 1) : filepath;
  if (FsHelpers::hasPdfExtension(baseName)) baseName.resize(baseName.length() - 4);

  static const char* const EXTENSIONS[] = {".bmp", ".jpg", ".jpeg", ".BMP", ".JPG", ".JPEG"};
  for (const auto& ext : EXTENSIONS) {
    const std::string candidate = folder + "/" + baseName + ext;
    if (Storage.exists(candidate.c_str())) return candidate;
  }
  return "";
}

bool Pdf::generateCoverBmp() const {
  if (Storage.exists(getCoverBmpPath().c_str())) return true;

  const std::string source = findCoverImage();
  if (source.empty()) return false;
  setupCacheDir();

  HalFile src, dst;
  if (!Storage.openFileForRead(TAG, source, src)) return false;
  if (!Storage.openFileForWrite(TAG, getCoverBmpPath(), dst)) return false;

  if (FsHelpers::hasBmpExtension(source)) {
    uint8_t buffer[512];
    while (src.available()) {
      const int read = src.read(buffer, sizeof(buffer));
      if (read <= 0) break;
      dst.write(buffer, static_cast<size_t>(read));
    }
    return true;
  }

  if (!JpegToBmpConverter::jpegFileToBmpStream(src, dst)) {
    dst.close();
    Storage.remove(getCoverBmpPath().c_str());
    return false;
  }
  return true;
}
