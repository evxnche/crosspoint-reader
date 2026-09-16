#pragma once

#include <HalStorage.h>

#include <cstddef>
#include <cstdint>
#include <string>
#include <vector>

#include "PdfSource.h"
#include "PdfTypes.h"

// Buffered byte sink. Decoded stream data is written to a scratch file rather
// than a heap buffer: a content stream for a dense page decompresses well past
// what the DRAM heap can spare once the reader, fonts, and framebuffer are up.
class PdfSink {
 public:
  ~PdfSink() { finish(); }

  bool open(const char* tag, const std::string& path);
  bool write(const uint8_t* data, size_t len);
  bool finish();

  [[nodiscard]] size_t bytesWritten() const { return written; }

 private:
  static constexpr size_t BUF = 512;

  HalFile file;
  bool opened = false;
  size_t written = 0;
  size_t fill = 0;
  uint8_t buf[BUF] = {};
};

// Post-decompression predictor (PDF 32000-1 table 10). Wraps a sink and
// reconstructs each row from the previous one before passing it along.
// Predictor <= 1 is a pass-through.
class PredictorSink {
 public:
  bool begin(PdfSink* out, int predictor, int colors, int bitsPerComponent, int columns);
  bool write(const uint8_t* data, size_t len);
  bool finish();

 private:
  bool emitRow();

  PdfSink* out = nullptr;
  int predictor = 1;
  size_t bpp = 1;     // bytes per pixel, rounded up, minimum 1
  size_t rowLen = 0;  // data bytes per row, excluding any PNG tag byte
  bool png = false;

  std::vector<uint8_t> current;
  std::vector<uint8_t> previous;
  size_t rowFill = 0;
  int pngTag = -1;  // PNG filter type for the row being accumulated
};

namespace PdfFilters {

// One stage of a /Filter chain.
struct Stage {
  std::string name;
  PdfObject parms;  // matching /DecodeParms entry (may be Null)
};

// True when the named filter carries image data this extractor does not decode
// (DCTDecode, JPXDecode, CCITTFaxDecode, JBIG2Decode).
bool isImageFilter(const std::string& name);

// Decode `len` raw bytes starting at `start` in `src` through `stages`, writing
// the result to `outPath`. `scratchPrefix` names the temporary files used
// between chained stages; they are removed before returning.
//
// Returns false when a stage is unsupported or the data is corrupt.
bool decodeToFile(const char* tag, PdfSource& src, size_t start, size_t len, const std::vector<Stage>& stages,
                  const std::string& outPath, const std::string& scratchPrefix);

}  // namespace PdfFilters
