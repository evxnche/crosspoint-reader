#include "PdfFilters.h"

#include <InflateStream.h>
#include <Logging.h>

#include <algorithm>
#include <cstdlib>
#include <cstring>

#include "PdfLexer.h"

// ---------------------------------------------------------------- PdfSink ---

bool PdfSink::open(const char* tag, const std::string& path) {
  finish();
  Storage.remove(path.c_str());
  if (!Storage.openFileForWrite(tag, path, file)) {
    LOG_ERR(tag, "Cannot write %s", path.c_str());
    return false;
  }
  opened = true;
  written = 0;
  fill = 0;
  return true;
}

bool PdfSink::write(const uint8_t* data, size_t len) {
  if (!opened) return false;
  written += len;
  while (len > 0) {
    const size_t take = std::min(len, BUF - fill);
    std::memcpy(buf + fill, data, take);
    fill += take;
    data += take;
    len -= take;
    if (fill == BUF) {
      if (file.write(buf, BUF) != BUF) return false;
      fill = 0;
    }
  }
  return true;
}

bool PdfSink::finish() {
  if (!opened) return true;
  bool ok = true;
  if (fill > 0) {
    ok = file.write(buf, fill) == fill;
    fill = 0;
  }
  file.close();
  opened = false;
  return ok;
}

// ----------------------------------------------------------- PredictorSink ---

bool PredictorSink::begin(PdfSink* sink, const int pred, int colors, int bitsPerComponent, int columns) {
  out = sink;
  predictor = pred;
  rowFill = 0;
  pngTag = -1;
  if (predictor <= 1) return true;

  if (colors < 1) colors = 1;
  if (bitsPerComponent < 1) bitsPerComponent = 8;
  if (columns < 1) columns = 1;

  const long bits = static_cast<long>(colors) * bitsPerComponent * columns;
  rowLen = static_cast<size_t>((bits + 7) / 8);
  bpp = static_cast<size_t>((colors * bitsPerComponent + 7) / 8);
  if (bpp < 1) bpp = 1;
  png = predictor >= 10;

  if (rowLen == 0 || rowLen > (1u << 20)) return false;
  current.assign(rowLen, 0);
  previous.assign(rowLen, 0);
  return true;
}

bool PredictorSink::emitRow() {
  if (png) {
    switch (pngTag) {
      case 0:  // None
        break;
      case 1:  // Sub
        for (size_t i = bpp; i < rowLen; ++i) current[i] = static_cast<uint8_t>(current[i] + current[i - bpp]);
        break;
      case 2:  // Up
        for (size_t i = 0; i < rowLen; ++i) current[i] = static_cast<uint8_t>(current[i] + previous[i]);
        break;
      case 3:  // Average
        for (size_t i = 0; i < rowLen; ++i) {
          const int left = i >= bpp ? current[i - bpp] : 0;
          current[i] = static_cast<uint8_t>(current[i] + ((left + previous[i]) >> 1));
        }
        break;
      case 4:  // Paeth
        for (size_t i = 0; i < rowLen; ++i) {
          const int a = i >= bpp ? current[i - bpp] : 0;
          const int b = previous[i];
          const int c = i >= bpp ? previous[i - bpp] : 0;
          const int p = a + b - c;
          const int pa = std::abs(p - a), pb = std::abs(p - b), pc = std::abs(p - c);
          const int pr = (pa <= pb && pa <= pc) ? a : (pb <= pc ? b : c);
          current[i] = static_cast<uint8_t>(current[i] + pr);
        }
        break;
      default:
        return false;
    }
  } else if (predictor == 2) {
    // TIFF predictor 2, 8-bit components only (the only width PDF producers use).
    for (size_t i = bpp; i < rowLen; ++i) current[i] = static_cast<uint8_t>(current[i] + current[i - bpp]);
  }

  if (!out->write(current.data(), rowLen)) return false;
  previous.swap(current);
  rowFill = 0;
  pngTag = -1;
  return true;
}

bool PredictorSink::write(const uint8_t* data, size_t len) {
  if (predictor <= 1) return out->write(data, len);

  while (len > 0) {
    if (png && pngTag < 0) {
      pngTag = *data++;
      --len;
      continue;
    }
    const size_t take = std::min(len, rowLen - rowFill);
    std::memcpy(current.data() + rowFill, data, take);
    rowFill += take;
    data += take;
    len -= take;
    if (rowFill == rowLen && !emitRow()) return false;
  }
  return true;
}

bool PredictorSink::finish() {
  // A trailing partial row means the stream was truncated; emit what we have so
  // the caller still sees the bytes that did arrive.
  if (predictor > 1 && rowFill > 0) {
    std::fill(current.begin() + static_cast<long>(rowFill), current.end(), 0);
    if (!emitRow()) return false;
  }
  return true;
}

// ---------------------------------------------------------------- filters ---

namespace {

struct RangeReader {
  PdfSource* src;
  size_t remaining;
  uint8_t buf[512];
};

size_t rangeFill(void* ctx, const uint8_t** data) {
  auto* r = static_cast<RangeReader*>(ctx);
  if (r->remaining == 0) return 0;
  const size_t want = std::min(sizeof(r->buf), r->remaining);
  const size_t got = r->src->read(r->buf, want);
  r->remaining -= got;
  *data = r->buf;
  return got;
}

bool inflateRange(const char* tag, PdfSource& src, const size_t start, const size_t len, PredictorSink& out,
                  const bool zlibWrapped) {
  InflateStream inflate;
  if (!inflate.init(true)) {
    LOG_ERR(tag, "Inflate init failed");
    return false;
  }
  if (zlibWrapped) inflate.setZlibWrapped();

  RangeReader reader{&src, len, {}};
  src.seek(start);
  inflate.setFill(rangeFill, &reader);

  uint8_t chunk[512];
  size_t total = 0;
  while (true) {
    size_t produced = 0;
    const InflateStream::Status st = inflate.readAtMost(chunk, sizeof(chunk), &produced);
    if (produced > 0) {
      if (!out.write(chunk, produced)) return false;
      total += produced;
    }
    if (st == InflateStream::Status::Done) return true;
    if (st == InflateStream::Status::Error) {
      // Truncated streams are common in the wild. Keep what decoded cleanly;
      // only a stream that produced nothing counts as a failure.
      if (total > 0) {
        LOG_DBG(tag, "Flate ended early after %zu bytes", total);
        return true;
      }
      return false;
    }
  }
}

bool decodeAsciiHex(PdfSource& src, const size_t start, const size_t len, PredictorSink& out) {
  src.seek(start);
  size_t remaining = len;
  int hi = -1;
  uint8_t chunk[256];
  size_t fill = 0;

  while (remaining-- > 0) {
    const int c = src.get();
    if (c < 0 || c == '>') break;
    int v;
    if (c >= '0' && c <= '9') {
      v = c - '0';
    } else if (c >= 'a' && c <= 'f') {
      v = c - 'a' + 10;
    } else if (c >= 'A' && c <= 'F') {
      v = c - 'A' + 10;
    } else {
      continue;
    }
    if (hi < 0) {
      hi = v;
      continue;
    }
    chunk[fill++] = static_cast<uint8_t>(hi * 16 + v);
    hi = -1;
    if (fill == sizeof(chunk)) {
      if (!out.write(chunk, fill)) return false;
      fill = 0;
    }
  }
  if (hi >= 0) chunk[fill++] = static_cast<uint8_t>(hi * 16);
  return fill == 0 || out.write(chunk, fill);
}

bool decodeAscii85(PdfSource& src, const size_t start, const size_t len, PredictorSink& out) {
  src.seek(start);
  size_t remaining = len;
  uint32_t tuple = 0;
  int count = 0;
  uint8_t chunk[256];
  size_t fill = 0;
  bool ok = true;

  auto emit = [&](const uint8_t* b, const size_t n) {
    for (size_t i = 0; i < n; ++i) {
      chunk[fill++] = b[i];
      if (fill == sizeof(chunk)) {
        if (!out.write(chunk, fill)) return false;
        fill = 0;
      }
    }
    return true;
  };

  while (remaining-- > 0) {
    const int c = src.get();
    if (c < 0 || c == '~') break;
    if (PdfLexer::isWhitespace(c)) continue;
    if (c == 'z' && count == 0) {
      const uint8_t zero[4] = {0, 0, 0, 0};
      if (!emit(zero, 4)) {
        ok = false;
        break;
      }
      continue;
    }
    if (c < '!' || c > 'u') continue;
    tuple = tuple * 85 + static_cast<uint32_t>(c - '!');
    if (++count == 5) {
      const uint8_t b[4] = {static_cast<uint8_t>(tuple >> 24), static_cast<uint8_t>(tuple >> 16),
                            static_cast<uint8_t>(tuple >> 8), static_cast<uint8_t>(tuple)};
      if (!emit(b, 4)) {
        ok = false;
        break;
      }
      tuple = 0;
      count = 0;
    }
  }
  if (ok && count > 1) {
    // Pad the partial group with the maximum digit and keep count-1 bytes.
    for (int i = count; i < 5; ++i) tuple = tuple * 85 + 84;
    const uint8_t b[4] = {static_cast<uint8_t>(tuple >> 24), static_cast<uint8_t>(tuple >> 16),
                          static_cast<uint8_t>(tuple >> 8), static_cast<uint8_t>(tuple)};
    ok = emit(b, static_cast<size_t>(count - 1));
  }
  return ok && (fill == 0 || out.write(chunk, fill));
}

bool decodeRunLength(PdfSource& src, const size_t start, const size_t len, PredictorSink& out) {
  src.seek(start);
  size_t remaining = len;
  uint8_t buf[128];
  while (remaining > 0) {
    const int lengthByte = src.get();
    if (lengthByte < 0) break;
    --remaining;
    if (lengthByte == 128) break;  // EOD
    if (lengthByte < 128) {
      const size_t n = static_cast<size_t>(lengthByte) + 1;
      if (n > remaining) break;
      if (src.read(buf, n) != n) break;
      remaining -= n;
      if (!out.write(buf, n)) return false;
    } else {
      const int b = src.get();
      if (b < 0) break;
      --remaining;
      const size_t n = 257 - static_cast<size_t>(lengthByte);
      std::memset(buf, b, n);
      if (!out.write(buf, n)) return false;
    }
  }
  return true;
}

// PDF LZW (clause 7.4.4). Codes are 9..12 bits, MSB first; 256 clears the
// table, 257 ends the stream. earlyChange=1 (the default) grows the code width
// one entry sooner than the plain TIFF algorithm.
bool decodeLzw(PdfSource& src, const size_t start, const size_t len, PredictorSink& out, const int earlyChange) {
  constexpr int MAX_CODES = 4096;
  // The dictionary is a prefix chain: each entry points at its predecessor and
  // adds one byte. Storing expanded strings would need megabytes in the worst case.
  auto* prefix = static_cast<uint16_t*>(malloc(MAX_CODES * (sizeof(uint16_t) + 1)));
  if (!prefix) return false;
  auto* suffix = reinterpret_cast<uint8_t*>(prefix + MAX_CODES);

  src.seek(start);
  size_t remaining = len;
  uint32_t bitBuf = 0;
  int bitCount = 0;
  int codeWidth = 9;
  int next = 258;
  int prev = -1;
  uint8_t stack[MAX_CODES];
  bool ok = true;

  auto readCode = [&]() -> int {
    while (bitCount < codeWidth) {
      if (remaining == 0) return -1;
      const int c = src.get();
      if (c < 0) return -1;
      --remaining;
      bitBuf = (bitBuf << 8) | static_cast<uint32_t>(c);
      bitCount += 8;
    }
    bitCount -= codeWidth;
    return static_cast<int>((bitBuf >> bitCount) & ((1u << codeWidth) - 1));
  };

  while (true) {
    const int code = readCode();
    if (code < 0 || code == 257) break;
    if (code == 256) {
      codeWidth = 9;
      next = 258;
      prev = -1;
      continue;
    }

    // A code at or past `next` is the deferred case: its string is the previous
    // string followed by that string's own first byte.
    const bool deferred = code >= next;
    int walk = deferred ? prev : code;
    if (walk < 0) break;

    size_t sp = 0;
    while (walk >= 256) {
      if (walk >= MAX_CODES || sp >= sizeof(stack)) {
        ok = false;
        break;
      }
      stack[sp++] = suffix[walk];
      walk = prefix[walk];
    }
    if (!ok) break;
    stack[sp++] = static_cast<uint8_t>(walk);
    const auto firstByte = static_cast<uint8_t>(walk);

    for (size_t i = sp; i-- > 0;) {
      if (!out.write(&stack[i], 1)) {
        ok = false;
        break;
      }
    }
    if (ok && deferred) ok = out.write(&firstByte, 1);
    if (!ok) break;

    if (prev >= 0 && next < MAX_CODES) {
      prefix[next] = static_cast<uint16_t>(prev);
      suffix[next] = firstByte;
      ++next;
    }
    prev = code;

    const int limit = next + earlyChange;
    if (codeWidth == 9 && limit >= 512) {
      codeWidth = 10;
    } else if (codeWidth == 10 && limit >= 1024) {
      codeWidth = 11;
    } else if (codeWidth == 11 && limit >= 2048) {
      codeWidth = 12;
    }
  }

  free(prefix);
  return ok;
}

int parmInt(const PdfObject& parms, const char* key, const int fallback) {
  if (const PdfObject* v = parms.find(key); v && v->isNumber()) return static_cast<int>(v->asInt());
  return fallback;
}

}  // namespace

namespace PdfFilters {

bool isImageFilter(const std::string& name) {
  return name == "DCTDecode" || name == "DCT" || name == "JPXDecode" || name == "CCITTFaxDecode" ||
         name == "JBIG2Decode";
}

bool decodeToFile(const char* tag, PdfSource& src, const size_t start, const size_t len,
                  const std::vector<Stage>& stages, const std::string& outPath, const std::string& scratchPrefix) {
  if (stages.empty()) {
    // No filter: copy the raw range out so callers have one uniform path.
    PdfSink sink;
    if (!sink.open(tag, outPath)) return false;
    src.seek(start);
    uint8_t buf[512];
    size_t remaining = len;
    while (remaining > 0) {
      const size_t got = src.read(buf, std::min(sizeof(buf), remaining));
      if (got == 0) break;
      if (!sink.write(buf, got)) return false;
      remaining -= got;
    }
    return sink.finish();
  }

  // Chained filters hand off through scratch files, so no stage ever needs the
  // whole intermediate result in RAM.
  PdfFileSource stageSrc;
  PdfSource* input = &src;
  size_t inStart = start;
  size_t inLen = len;
  std::vector<std::string> scratch;

  bool ok = true;
  for (size_t i = 0; i < stages.size() && ok; ++i) {
    const Stage& stage = stages[i];
    const bool last = i + 1 == stages.size();
    const std::string dest = last ? outPath : scratchPrefix + "_s" + std::to_string(i) + ".tmp";
    if (!last) scratch.push_back(dest);

    if (isImageFilter(stage.name)) {
      LOG_DBG(tag, "Skipping image filter %s", stage.name.c_str());
      ok = false;
      break;
    }

    // One attempt at one stage, sink and predictor included, so a retry starts
    // from a clean output file instead of appending to a half-written one.
    auto runStage = [&](const bool zlibWrapped) {
      PdfSink sink;
      if (!sink.open(tag, dest)) return false;
      PredictorSink pred;
      if (!pred.begin(&sink, parmInt(stage.parms, "Predictor", 1), parmInt(stage.parms, "Colors", 1),
                      parmInt(stage.parms, "BitsPerComponent", 8), parmInt(stage.parms, "Columns", 1))) {
        return false;
      }

      bool staged;
      if (stage.name == "FlateDecode" || stage.name == "Fl") {
        staged = inflateRange(tag, *input, inStart, inLen, pred, zlibWrapped);
      } else if (stage.name == "LZWDecode" || stage.name == "LZW") {
        staged = decodeLzw(*input, inStart, inLen, pred, parmInt(stage.parms, "EarlyChange", 1));
      } else if (stage.name == "ASCIIHexDecode" || stage.name == "AHx") {
        staged = decodeAsciiHex(*input, inStart, inLen, pred);
      } else if (stage.name == "ASCII85Decode" || stage.name == "A85") {
        staged = decodeAscii85(*input, inStart, inLen, pred);
      } else if (stage.name == "RunLengthDecode" || stage.name == "RL") {
        staged = decodeRunLength(*input, inStart, inLen, pred);
      } else {
        // Includes /Crypt, which only appears in encrypted files.
        LOG_DBG(tag, "Unsupported filter %s", stage.name.c_str());
        staged = false;
      }
      return staged && pred.finish() && sink.finish();
    };

    ok = runStage(true);
    // A few producers emit raw deflate with no zlib header; retry without it.
    if (!ok && (stage.name == "FlateDecode" || stage.name == "Fl")) ok = runStage(false);
    if (!ok) break;

    if (!last) {
      stageSrc.close();
      if (!stageSrc.open(tag, dest)) {
        ok = false;
        break;
      }
      input = &stageSrc;
      inStart = 0;
      inLen = stageSrc.size();
    }
  }

  stageSrc.close();
  for (const auto& path : scratch) Storage.remove(path.c_str());
  if (!ok) Storage.remove(outPath.c_str());
  return ok;
}

}  // namespace PdfFilters
