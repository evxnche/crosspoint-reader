#include "PdfSource.h"

#include <Logging.h>

#include <algorithm>
#include <cstring>

bool PdfFileSource::open(const char* tag, const std::string& path) {
  close();
  logTag = tag;
  filePath = path;
  if (!Storage.openFileForRead(tag, path, file)) {
    LOG_ERR(tag, "Cannot open %s", path.c_str());
    return false;
  }
  fileSize = file.size();
  pos = 0;
  windowStart = 0;
  windowLen = 0;
  opened = true;
  return true;
}

void PdfFileSource::close() {
  if (opened) {
    file.close();
    opened = false;
  }
  fileSize = 0;
  pos = 0;
  windowStart = 0;
  windowLen = 0;
}

bool PdfFileSource::seek(const size_t p) {
  if (!opened) return false;
  pos = std::min(p, fileSize);
  return true;
}

bool PdfFileSource::fill() {
  if (!opened || pos >= fileSize) return false;
  if (!file.seek(pos)) return false;
  const size_t want = std::min(WINDOW, fileSize - pos);
  const int got = file.read(window, want);
  if (got <= 0) {
    windowLen = 0;
    return false;
  }
  windowStart = pos;
  windowLen = static_cast<size_t>(got);
  return true;
}

int PdfFileSource::get() {
  if (!opened || pos >= fileSize) return -1;
  if (pos < windowStart || pos >= windowStart + windowLen) {
    if (!fill()) return -1;
  }
  return window[pos++ - windowStart];
}

size_t PdfFileSource::read(uint8_t* dest, size_t len) {
  if (!opened || pos >= fileSize) return 0;
  len = std::min(len, fileSize - pos);
  size_t done = 0;

  // Serve whatever the current window already covers before touching the card.
  if (pos >= windowStart && pos < windowStart + windowLen) {
    const size_t avail = std::min(len, windowStart + windowLen - pos);
    std::memcpy(dest, window + (pos - windowStart), avail);
    pos += avail;
    done += avail;
  }

  // Anything larger than the window bypasses it: copying through a 1KB staging
  // buffer would only add memcpys to a read the card can satisfy directly.
  if (done < len) {
    const size_t remaining = len - done;
    if (remaining >= WINDOW) {
      if (!file.seek(pos)) return done;
      const int got = file.read(dest + done, remaining);
      if (got > 0) {
        pos += static_cast<size_t>(got);
        done += static_cast<size_t>(got);
      }
      windowLen = 0;  // file cursor moved out from under the window
    } else {
      if (!fill()) return done;
      const size_t avail = std::min(remaining, windowLen);
      std::memcpy(dest + done, window, avail);
      pos += avail;
      done += avail;
    }
  }
  return done;
}

size_t PdfMemSource::read(uint8_t* dest, size_t n) {
  n = std::min(n, len - pos);
  std::memcpy(dest, data + pos, n);
  pos += n;
  return n;
}
