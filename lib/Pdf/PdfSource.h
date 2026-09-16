#pragma once

#include <HalStorage.h>

#include <cstddef>
#include <cstdint>
#include <string>
#include <vector>

// Random-access byte source for the PDF parser.
//
// Two implementations: the book file itself, and a decoded stream that has been
// spilled to a scratch file or kept in RAM. Everything above this interface is
// unaware of which it is reading.
class PdfSource {
 public:
  virtual ~PdfSource() = default;

  virtual bool seek(size_t pos) = 0;
  [[nodiscard]] virtual size_t tell() const = 0;
  [[nodiscard]] virtual size_t size() const = 0;
  // Next byte, or -1 at end of source.
  virtual int get() = 0;

  // Read up to len bytes. Returns the count actually read.
  virtual size_t read(uint8_t* dest, size_t len) = 0;

  bool atEnd() { return tell() >= size(); }
};

// Buffered reader over a file on the SD card.
//
// The parser seeks constantly (object index, page tree, per-page content), and
// an unbuffered HalFile::read of one byte per get() is dominated by mutex and
// FAT overhead. A single block-sized window absorbs the sequential runs.
class PdfFileSource final : public PdfSource {
 public:
  PdfFileSource() = default;

  bool open(const char* tag, const std::string& path);
  void close();
  [[nodiscard]] bool isOpen() const { return opened; }

  bool seek(size_t pos) override;
  [[nodiscard]] size_t tell() const override { return pos; }
  [[nodiscard]] size_t size() const override { return fileSize; }
  int get() override;
  size_t read(uint8_t* dest, size_t len) override;

 private:
  static constexpr size_t WINDOW = 1024;

  bool fill();

  HalFile file;
  std::string filePath;
  const char* logTag = "PDF";
  bool opened = false;
  size_t fileSize = 0;
  size_t pos = 0;  // logical read position
  size_t windowStart = 0;
  size_t windowLen = 0;
  uint8_t window[WINDOW] = {};
};

// Read-only view over a buffer the caller owns for the lifetime of the source.
class PdfMemSource final : public PdfSource {
 public:
  PdfMemSource(const uint8_t* data, size_t len) : data(data), len(len) {}

  bool seek(size_t p) override {
    pos = p > len ? len : p;
    return true;
  }
  [[nodiscard]] size_t tell() const override { return pos; }
  [[nodiscard]] size_t size() const override { return len; }
  int get() override { return pos < len ? data[pos++] : -1; }
  size_t read(uint8_t* dest, size_t n) override;

 private:
  const uint8_t* data;
  size_t len;
  size_t pos = 0;
};
