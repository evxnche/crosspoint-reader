#include "PdfDocument.h"

#include <Logging.h>
#include <Memory.h>

#include <algorithm>
#include <cstdlib>
#include <cstring>

#include "PdfFilters.h"
#include "PdfLexer.h"

namespace {

constexpr const char* TAG = "PDFDOC";

// Guard rails against corrupt or hostile files. Each is a hard structural
// limit, not a tuning knob: exceeding one means the document cannot be read on
// this hardware regardless.
// 12 bytes per entry, so this cap is also a memory ceiling: 32,768 objects is
// ~390KB, already past what the reader can spare. It exists to stop a corrupt
// file allocating without bound, not as a size a real document should reach.
constexpr size_t MAX_OBJECTS = 32768;
constexpr int MAX_PAGES = 4096;
constexpr int MAX_XREF_CHAIN = 32;
constexpr int MAX_TREE_DEPTH = 64;

// Build the /Filter chain for a stream, pairing each name with its
// /DecodeParms entry. Both keys may be a single value or an array.
std::vector<PdfFilters::Stage> buildStages(PdfDocument& doc, const PdfObject& streamObj) {
  std::vector<PdfFilters::Stage> stages;

  PdfObject filter = doc.lookup(streamObj, "Filter");
  if (filter.type == PdfType::Null) filter = doc.lookup(streamObj, "F");
  PdfObject parms = doc.lookup(streamObj, "DecodeParms");
  if (parms.type == PdfType::Null) parms = doc.lookup(streamObj, "DP");

  auto parmAt = [&](const size_t i) -> PdfObject {
    if (parms.type == PdfType::Array) {
      return i < parms.items.size() ? doc.resolve(parms.items[i]) : PdfObject{};
    }
    return i == 0 ? parms : PdfObject{};
  };

  if (filter.type == PdfType::Name) {
    stages.push_back({filter.text, parmAt(0)});
  } else if (filter.type == PdfType::Array) {
    stages.reserve(filter.items.size());
    for (size_t i = 0; i < filter.items.size(); ++i) {
      const PdfObject f = doc.resolve(filter.items[i]);
      if (f.type == PdfType::Name) stages.push_back({f.text, parmAt(i)});
    }
  }
  return stages;
}

// UTF-16BE (with BOM) is how PDF text strings carry anything outside PDFDoc
// encoding; everything else is close enough to Latin-1 to pass through.
std::string pdfTextToUtf8(const std::string& raw) {
  std::string out;
  auto appendCodepoint = [&out](const uint32_t cp) {
    if (cp < 0x80) {
      out.push_back(static_cast<char>(cp));
    } else if (cp < 0x800) {
      out.push_back(static_cast<char>(0xC0 | (cp >> 6)));
      out.push_back(static_cast<char>(0x80 | (cp & 0x3F)));
    } else if (cp < 0x10000) {
      out.push_back(static_cast<char>(0xE0 | (cp >> 12)));
      out.push_back(static_cast<char>(0x80 | ((cp >> 6) & 0x3F)));
      out.push_back(static_cast<char>(0x80 | (cp & 0x3F)));
    } else {
      out.push_back(static_cast<char>(0xF0 | (cp >> 18)));
      out.push_back(static_cast<char>(0x80 | ((cp >> 12) & 0x3F)));
      out.push_back(static_cast<char>(0x80 | ((cp >> 6) & 0x3F)));
      out.push_back(static_cast<char>(0x80 | (cp & 0x3F)));
    }
  };

  if (raw.size() >= 2 && static_cast<uint8_t>(raw[0]) == 0xFE && static_cast<uint8_t>(raw[1]) == 0xFF) {
    for (size_t i = 2; i + 1 < raw.size(); i += 2) {
      uint32_t cp = (static_cast<uint8_t>(raw[i]) << 8) | static_cast<uint8_t>(raw[i + 1]);
      if (cp >= 0xD800 && cp <= 0xDBFF && i + 3 < raw.size()) {
        const uint32_t lo = (static_cast<uint8_t>(raw[i + 2]) << 8) | static_cast<uint8_t>(raw[i + 3]);
        if (lo >= 0xDC00 && lo <= 0xDFFF) {
          cp = 0x10000 + ((cp - 0xD800) << 10) + (lo - 0xDC00);
          i += 2;
        }
      }
      appendCodepoint(cp);
    }
    return out;
  }

  for (const char c : raw) appendCodepoint(static_cast<uint8_t>(c));
  return out;
}

}  // namespace

// ---------------------------------------------------------------- lifetime ---

bool PdfDocument::open(const std::string& path, const std::string& scratchDir) {
  close();
  filePath = path;
  scratch = scratchDir;

  if (!src.open(TAG, path)) return false;

  // The header may be preceded by junk; %PDF- within the first kilobyte is the
  // accepted tolerance and also our format check.
  uint8_t head[1024];
  const size_t got = src.read(head, sizeof(head));
  bool sawHeader = false;
  for (size_t i = 0; got >= 5 && i + 5 <= got; ++i) {
    if (std::memcmp(head + i, "%PDF-", 5) == 0) {
      sawHeader = true;
      break;
    }
  }
  if (!sawHeader) {
    LOG_ERR(TAG, "Not a PDF: %s", path.c_str());
    close();
    return false;
  }

  PdfObject catalog;
  if (readTrailerChain()) {
    if (trailer.find("Encrypt")) encrypted = true;
    catalog = lookup(trailer, "Root");
  }

  if (!catalog.isDictLike() || !catalog.find("Pages")) {
    // Damaged or unusual xref. A full object scan recovers most such files.
    LOG_DBG(TAG, "Falling back to object scan");
    if (!scanAllObjects()) {
      close();
      return false;
    }
    if (!findCatalogByScan(catalog)) {
      LOG_ERR(TAG, "No document catalog");
      close();
      return false;
    }
  }

  if (encrypted) {
    LOG_ERR(TAG, "Encrypted PDF is not supported");
    close();
    return false;
  }

  if (!buildPageList(catalog) || pages.empty()) {
    LOG_ERR(TAG, "No pages found");
    close();
    return false;
  }

  LOG_INF(TAG, "Opened %s: %u objects, %u pages", path.c_str(), static_cast<unsigned>(xref.size()),
          static_cast<unsigned>(pages.size()));
  return true;
}

void PdfDocument::close() {
  src.close();
  objStmSrc.close();
  if (objStmValid && !scratch.empty()) {
    const std::string p = scratch + "/objstm.bin";
    Storage.remove(p.c_str());
  }
  xref.clear();
  xref.shrink_to_fit();
  pages.clear();
  pages.shrink_to_fit();
  objStmIndex.clear();
  objStmIndex.shrink_to_fit();
  trailer = PdfObject{};
  info = PdfObject{};
  infoLoaded = false;
  encrypted = false;
  scanned = false;
  objStmValid = false;
  objStmCached = 0;
  for (auto& slot : cache) {
    slot.valid = false;
    slot.obj = PdfObject{};
  }
  cacheNext = 0;
}

// ------------------------------------------------------------- xref index ---

void PdfDocument::setEntry(const XrefEntry& e) {
  // An entry already present came from a newer cross-reference section and wins.
  if (findEntry(e.num)) return;
  if (xref.size() >= MAX_OBJECTS) return;
  const auto at =
      std::lower_bound(xref.begin(), xref.end(), e.num, [](const XrefEntry& x, const uint32_t n) { return x.num < n; });
  xref.insert(at, e);
}

const PdfDocument::XrefEntry* PdfDocument::findEntry(const uint32_t num) const {
  const auto at =
      std::lower_bound(xref.begin(), xref.end(), num, [](const XrefEntry& x, const uint32_t n) { return x.num < n; });
  if (at == xref.end() || at->num != num) return nullptr;
  return &*at;
}

bool PdfDocument::readTrailerChain() {
  // startxref lives in the last kilobyte or so of the file.
  const size_t fileSize = src.size();
  const size_t tailLen = std::min<size_t>(2048, fileSize);
  auto tail = makeUniqueNoThrow<char[]>(tailLen + 1);
  if (!tail) {
    LOG_ERR(TAG, "OOM: %zu bytes", tailLen + 1);
    return false;
  }
  src.seek(fileSize - tailLen);
  const size_t got = src.read(reinterpret_cast<uint8_t*>(tail.get()), tailLen);
  tail[got] = '\0';

  const std::string_view view(tail.get(), got);
  const size_t at = view.rfind("startxref");
  if (at == std::string_view::npos) return false;

  size_t p = at + 9;
  while (p < got && PdfLexer::isWhitespace(tail[p])) ++p;
  size_t offset = 0;
  bool any = false;
  while (p < got && tail[p] >= '0' && tail[p] <= '9') {
    offset = offset * 10 + static_cast<size_t>(tail[p] - '0');
    ++p;
    any = true;
  }
  if (!any || offset >= fileSize) return false;

  return readXrefSection(offset, 0, trailer);
}

bool PdfDocument::readXrefSection(const size_t offset, const int depth, PdfObject& outTrailer) {
  if (depth > MAX_XREF_CHAIN || offset >= src.size()) return false;
  src.seek(offset);

  PdfLexer lexer(src);
  const size_t mark = src.tell();
  const std::string tok = lexer.nextToken();

  PdfObject sectionTrailer;
  if (tok == "xref") {
    if (!readClassicXref(depth, sectionTrailer)) return false;
  } else {
    // A cross-reference stream: "<num> <gen> obj << ... >> stream".
    src.seek(mark);
    (void)lexer.nextToken();  // object number
    (void)lexer.nextToken();  // generation
    if (lexer.nextToken() != "obj") return false;
    PdfObject streamObj = lexer.parseObject();
    if (streamObj.type != PdfType::Stream) return false;
    fixStreamLength(streamObj, src);
    if (!readXrefStream(streamObj, depth, sectionTrailer)) return false;
  }

  if (depth == 0) {
    outTrailer = sectionTrailer;
  } else {
    // Older sections contribute only keys the newest trailer left unset.
    for (auto& kv : sectionTrailer.dict) {
      if (!outTrailer.find(kv.first.c_str())) outTrailer.dict.emplace_back(kv.first, kv.second);
    }
  }

  // A hybrid-reference file points at a parallel xref stream for the objects
  // its classic table cannot describe; read it before the older sections.
  if (const PdfObject* hybrid = sectionTrailer.find("XRefStm"); hybrid && hybrid->isNumber()) {
    PdfObject ignored;
    readXrefSection(static_cast<size_t>(hybrid->asInt()), depth + 1, ignored);
  }
  if (const PdfObject* prev = sectionTrailer.find("Prev"); prev && prev->isNumber()) {
    readXrefSection(static_cast<size_t>(prev->asInt()), depth + 1, outTrailer);
  }
  return true;
}

bool PdfDocument::readClassicXref(const int depth, PdfObject& outTrailer) {
  (void)depth;
  PdfLexer lexer(src);

  while (true) {
    const size_t mark = src.tell();
    const std::string tok = lexer.nextToken();
    if (tok.empty()) return false;
    if (tok == "trailer") {
      if (lexer.nextToken() != "<<") return false;
      outTrailer = lexer.parseDictOrStream();
      return true;
    }

    // Subsection header: "<first> <count>".
    const char* p = tok.c_str();
    char* end = nullptr;
    const long first = std::strtol(p, &end, 10);
    if (end == p) {
      src.seek(mark);
      return false;
    }
    const std::string countTok = lexer.nextToken();
    const long count = std::strtol(countTok.c_str(), nullptr, 10);
    if (count < 0 || count > static_cast<long>(MAX_OBJECTS)) return false;

    // Entries are fixed 20-byte records, but tolerate producers that pad
    // differently by lexing each field.
    for (long i = 0; i < count; ++i) {
      const std::string offTok = lexer.nextToken();
      const std::string genTok = lexer.nextToken();
      const std::string typeTok = lexer.nextToken();
      if (typeTok.empty()) return false;
      if (typeTok != "n") continue;  // 'f' marks a free object
      XrefEntry e;
      e.num = static_cast<uint32_t>(first + i);
      e.offset = static_cast<uint32_t>(std::strtoul(offTok.c_str(), nullptr, 10));
      (void)genTok;
      setEntry(e);
    }
  }
}

bool PdfDocument::readXrefStream(const PdfObject& streamObj, const int depth, PdfObject& outTrailer) {
  (void)depth;
  const std::string decoded = scratch + "/xref.bin";
  if (!decodeStream(streamObj, decoded)) return false;

  PdfFileSource data;
  if (!data.open(TAG, decoded)) {
    Storage.remove(decoded.c_str());
    return false;
  }

  // /W gives the byte width of each of the three fields; a zero width means the
  // field is absent and takes its default (type 1, everything else 0).
  int w[3] = {1, 1, 1};
  if (const PdfObject* wObj = streamObj.find("W"); wObj && wObj->type == PdfType::Array) {
    for (size_t i = 0; i < 3 && i < wObj->items.size(); ++i) w[i] = static_cast<int>(wObj->items[i].asInt(0));
  }
  const size_t rowLen = static_cast<size_t>(w[0] + w[1] + w[2]);
  if (rowLen == 0 || rowLen > 32) {
    data.close();
    Storage.remove(decoded.c_str());
    return false;
  }

  // /Index lists [first count] pairs; its default covers 0..Size.
  std::vector<std::pair<long, long>> ranges;
  if (const PdfObject* idx = streamObj.find("Index"); idx && idx->type == PdfType::Array) {
    ranges.reserve(idx->items.size() / 2);
    for (size_t i = 0; i + 1 < idx->items.size(); i += 2) {
      ranges.emplace_back(idx->items[i].asInt(), idx->items[i + 1].asInt());
    }
  } else {
    const PdfObject* size = streamObj.find("Size");
    ranges.emplace_back(0, size ? size->asInt() : 0);
  }

  uint8_t row[32];
  for (const auto& [first, count] : ranges) {
    for (long i = 0; i < count; ++i) {
      if (data.read(row, rowLen) != rowLen) break;
      size_t at = 0;
      auto field = [&](const int width, const uint64_t fallback) {
        if (width == 0) return fallback;
        uint64_t v = 0;
        for (int b = 0; b < width; ++b) v = (v << 8) | row[at++];
        return v;
      };
      const uint64_t type = field(w[0], 1);
      const uint64_t f2 = field(w[1], 0);
      const uint64_t f3 = field(w[2], 0);

      XrefEntry e;
      e.num = static_cast<uint32_t>(first + i);
      if (type == 1) {
        e.offset = static_cast<uint32_t>(f2);
        setEntry(e);
      } else if (type == 2) {
        e.inObjStm = true;
        e.container = static_cast<uint32_t>(f2);
        e.offset = static_cast<uint32_t>(f3);  // index within the container
        setEntry(e);
      }
      // type 0 is a free object.
    }
  }

  data.close();
  Storage.remove(decoded.c_str());

  // The stream dictionary doubles as the trailer.
  outTrailer.type = PdfType::Dict;
  outTrailer.dict = streamObj.dict;
  return true;
}

bool PdfDocument::scanAllObjects() {
  if (scanned) return true;
  scanned = true;
  xref.clear();

  // One forward pass looking for "<num> <gen> obj". The window carries enough
  // bytes back that a header split across reads is still recognised.
  constexpr size_t CHUNK = 2048;
  constexpr size_t BACK = 32;
  auto buf = makeUniqueNoThrow<uint8_t[]>(CHUNK + BACK);
  if (!buf) {
    LOG_ERR(TAG, "OOM: object scan buffer");
    return false;
  }

  std::vector<XrefEntry> found;
  found.reserve(1024);

  size_t filePos = 0;
  size_t carry = 0;  // valid bytes retained at the front of buf
  const size_t fileSize = src.size();

  while (filePos < fileSize) {
    src.seek(filePos);
    const size_t got = src.read(buf.get() + carry, CHUNK);
    if (got == 0) break;
    const size_t total = carry + got;
    const size_t windowStart = filePos - carry;

    for (size_t i = 0; i + 3 <= total; ++i) {
      if (buf[i] != 'o' || buf[i + 1] != 'b' || buf[i + 2] != 'j') continue;
      if (i + 3 < total && !PdfLexer::isWhitespace(buf[i + 3]) && !PdfLexer::isDelimiter(buf[i + 3])) continue;
      if (i == 0 || !PdfLexer::isWhitespace(buf[i - 1])) continue;

      // Walk back over "<num> <gen> ".
      size_t j = i - 1;
      while (j > 0 && PdfLexer::isWhitespace(buf[j])) --j;
      if (buf[j] < '0' || buf[j] > '9') continue;
      while (j > 0 && buf[j - 1] >= '0' && buf[j - 1] <= '9') --j;
      if (j == 0) continue;
      size_t k = j - 1;
      while (k > 0 && PdfLexer::isWhitespace(buf[k])) --k;
      if (buf[k] < '0' || buf[k] > '9') continue;
      size_t numEnd = k + 1;
      while (k > 0 && buf[k - 1] >= '0' && buf[k - 1] <= '9') --k;

      uint32_t num = 0;
      bool overflow = false;
      for (size_t d = k; d < numEnd; ++d) {
        num = num * 10 + static_cast<uint32_t>(buf[d] - '0');
        if (num > MAX_OBJECTS) {
          overflow = true;
          break;
        }
      }
      if (overflow || found.size() >= MAX_OBJECTS) continue;

      XrefEntry e;
      e.num = num;
      e.offset = static_cast<uint32_t>(windowStart + k);
      found.push_back(e);
    }

    filePos += got;
    carry = std::min(BACK, total);
    std::memmove(buf.get(), buf.get() + total - carry, carry);
  }

  // Later definitions supersede earlier ones (incremental updates append).
  std::stable_sort(found.begin(), found.end(), [](const XrefEntry& a, const XrefEntry& b) { return a.num < b.num; });
  xref.reserve(found.size());
  for (const auto& e : found) {
    if (!xref.empty() && xref.back().num == e.num) {
      xref.back() = e;
    } else {
      xref.push_back(e);
    }
  }

  // Objects hidden inside /ObjStm containers are not visible to a byte scan.
  // Collect the container numbers first: registering entries reorders `xref`.
  std::vector<uint32_t> containers;
  for (const auto& entry : xref) {
    PdfObject obj;
    if (!loadObject(entry.num, obj)) continue;
    if (obj.type != PdfType::Stream) continue;
    const PdfObject* typeName = obj.find("Type");
    if (typeName && typeName->isName("ObjStm")) containers.push_back(entry.num);
  }
  for (const uint32_t containerNum : containers) registerObjStmContents(containerNum);

  LOG_DBG(TAG, "Scan found %u objects", static_cast<unsigned>(xref.size()));
  return !xref.empty();
}

void PdfDocument::registerObjStmContents(const uint32_t containerNum) {
  // Parsing slot 0 populates objStmIndex for the whole container.
  XrefEntry probe;
  probe.num = 0;
  probe.inObjStm = true;
  probe.container = containerNum;
  probe.offset = 0;
  PdfObject ignored;
  (void)loadFromObjStm(probe, ignored);
  if (!objStmValid || objStmCached != containerNum) return;

  for (size_t slot = 0; slot < objStmIndex.size(); ++slot) {
    XrefEntry e;
    e.num = objStmIndex[slot].first;
    e.inObjStm = true;
    e.container = containerNum;
    e.offset = static_cast<uint32_t>(slot);
    setEntry(e);
  }
}

bool PdfDocument::findCatalogByScan(PdfObject& outCatalog) {
  if (!scanAllObjects()) return false;
  // Prefer the catalog that actually has a page tree: some files carry a stale
  // catalog from an earlier revision.
  for (const auto& e : xref) {
    PdfObject obj;
    if (!loadObject(e.num, obj)) continue;
    const PdfObject* type = obj.find("Type");
    if (type && type->isName("Catalog") && obj.find("Pages")) {
      outCatalog = obj;
      return true;
    }
  }
  // No catalog at all: synthesise one around the first /Pages node with no parent.
  for (const auto& e : xref) {
    PdfObject obj;
    if (!loadObject(e.num, obj)) continue;
    const PdfObject* type = obj.find("Type");
    if (type && type->isName("Pages") && !obj.find("Parent")) {
      outCatalog.type = PdfType::Dict;
      PdfObject ref;
      ref.type = PdfType::Ref;
      ref.refNum = e.num;
      outCatalog.dict.emplace_back("Pages", ref);
      return true;
    }
  }
  return false;
}

// ---------------------------------------------------------- object access ---

void PdfDocument::fixStreamLength(PdfObject& streamObj, PdfSource& owner) {
  if (streamObj.type != PdfType::Stream) return;

  if (const PdfObject* len = streamObj.find("Length")) {
    if (len->type == PdfType::Ref) {
      PdfObject resolved;
      if (loadObject(len->refNum, resolved) && resolved.isNumber()) {
        streamObj.streamLen = static_cast<size_t>(resolved.asInt());
        return;
      }
    } else if (len->isNumber() && streamObj.streamLen != 0) {
      return;
    }
  }

  // No usable /Length: scan forward for `endstream`. The window overlaps by
  // more than the keyword so a match split across reads is still found.
  constexpr size_t CHUNK = 512;
  constexpr size_t BACK = 16;
  auto buf = makeUniqueNoThrow<uint8_t[]>(CHUNK + BACK);
  if (!buf) {
    streamObj.streamLen = 0;
    return;
  }

  owner.seek(streamObj.streamStart);
  size_t carry = 0;
  size_t windowStart = 0;  // payload offset of buf[0]
  while (true) {
    const size_t got = owner.read(buf.get() + carry, CHUNK);
    if (got == 0) break;
    const size_t total = carry + got;
    for (size_t i = 0; i + 9 <= total; ++i) {
      if (std::memcmp(buf.get() + i, "endstream", 9) != 0) continue;
      // Trim the EOL the writer placed before the keyword.
      size_t end = i;
      while (end > 0 && (buf[end - 1] == 0x0A || buf[end - 1] == 0x0D)) --end;
      streamObj.streamLen = windowStart + end;
      return;
    }
    carry = std::min(BACK, total);
    windowStart += total - carry;
    std::memmove(buf.get(), buf.get() + total - carry, carry);
  }
  streamObj.streamLen = 0;
}

bool PdfDocument::loadObject(const uint32_t num, PdfObject& out) {
  for (const auto& slot : cache) {
    if (slot.valid && slot.num == num) {
      out = slot.obj;
      return true;
    }
  }

  const XrefEntry* e = findEntry(num);
  if (!e) {
    // The index may be from a broken xref; a scan can still find the object.
    if (!scanned) {
      if (!scanAllObjects()) return false;
      e = findEntry(num);
    }
    if (!e) return false;
  }

  bool ok;
  if (e->inObjStm) {
    XrefEntry copy = *e;
    ok = loadFromObjStm(copy, out);
  } else {
    src.seek(e->offset);
    PdfLexer lexer(src);
    const std::string numTok = lexer.nextToken();
    (void)lexer.nextToken();  // generation
    if (lexer.nextToken() != "obj") return false;
    if (std::strtoul(numTok.c_str(), nullptr, 10) != num) return false;
    out = lexer.parseObject();
    if (out.type == PdfType::Stream) fixStreamLength(out, src);
    ok = true;
  }

  if (ok) {
    CacheSlot& slot = cache[cacheNext];
    cacheNext = (cacheNext + 1) % CACHE_SLOTS;
    slot.num = num;
    slot.obj = out;
    slot.valid = true;
  }
  return ok;
}

bool PdfDocument::loadFromObjStm(const XrefEntry& e, PdfObject& out) {
  if (!objStmValid || objStmCached != e.container) {
    objStmSrc.close();
    objStmIndex.clear();
    objStmValid = false;

    PdfObject container;
    // Read the container directly rather than through loadObject, which would
    // recurse back into this function for a malformed self-referential file.
    const XrefEntry* ce = findEntry(e.container);
    if (!ce || ce->inObjStm) return false;
    src.seek(ce->offset);
    PdfLexer lexer(src);
    (void)lexer.nextToken();
    (void)lexer.nextToken();
    if (lexer.nextToken() != "obj") return false;
    container = lexer.parseObject();
    if (container.type != PdfType::Stream) return false;
    fixStreamLength(container, src);

    const std::string decoded = scratch + "/objstm.bin";
    if (!decodeStream(container, decoded)) return false;
    if (!objStmSrc.open(TAG, decoded)) return false;

    const PdfObject* nObj = container.find("N");
    const PdfObject* firstObj = container.find("First");
    const long n = nObj ? nObj->asInt() : 0;
    objStmFirst = firstObj ? static_cast<size_t>(firstObj->asInt()) : 0;
    if (n <= 0 || n > static_cast<long>(MAX_OBJECTS)) return false;

    // The header is N pairs of "<objnum> <offset>" before /First.
    objStmSrc.seek(0);
    PdfLexer headerLexer(objStmSrc);
    objStmIndex.reserve(static_cast<size_t>(n));
    for (long i = 0; i < n; ++i) {
      const std::string a = headerLexer.nextToken();
      const std::string b = headerLexer.nextToken();
      if (a.empty() || b.empty()) break;
      objStmIndex.emplace_back(static_cast<uint32_t>(std::strtoul(a.c_str(), nullptr, 10)),
                               static_cast<uint32_t>(std::strtoul(b.c_str(), nullptr, 10)));
    }
    objStmCached = e.container;
    objStmValid = true;
  }

  if (e.offset >= objStmIndex.size()) return false;
  objStmSrc.seek(objStmFirst + objStmIndex[e.offset].second);
  PdfLexer lexer(objStmSrc);
  out = lexer.parseObject();
  return true;
}

PdfObject PdfDocument::resolve(const PdfObject& o) {
  if (o.type != PdfType::Ref) return o;
  PdfObject target;
  if (!loadObject(o.refNum, target)) return PdfObject{};
  // One hop is enough in practice; a reference to a reference is malformed.
  return target;
}

PdfObject PdfDocument::lookup(const PdfObject& dict, const char* key) {
  const PdfObject* v = dict.find(key);
  if (!v) return PdfObject{};
  return resolve(*v);
}

bool PdfDocument::decodeStream(const PdfObject& streamObj, const std::string& outPath) {
  if (streamObj.type != PdfType::Stream || streamObj.streamLen == 0) return false;
  const auto stages = buildStages(*this, streamObj);
  // The payload lives in whichever source the object came from. Object streams
  // never nest, so a stream object always belongs to the file itself.
  return PdfFilters::decodeToFile(TAG, src, streamObj.streamStart, streamObj.streamLen, stages, outPath,
                                  scratch + "/f");
}

// --------------------------------------------------------------- page tree ---

bool PdfDocument::buildPageList(const PdfObject& catalog) {
  pages.clear();
  const PdfObject pagesRoot = lookup(catalog, "Pages");
  if (!pagesRoot.isDictLike()) return false;

  int budget = MAX_PAGES;
  PdfObject inherited = pagesRoot.find("Resources") ? *pagesRoot.find("Resources") : PdfObject{};
  if (!walkPageTree(pagesRoot, inherited, 0, budget)) return false;

  if (pages.empty() && scanAllObjects()) {
    // A broken /Kids chain still leaves the page objects themselves intact.
    for (const auto& e : xref) {
      PdfObject obj;
      if (!loadObject(e.num, obj)) continue;
      const PdfObject* type = obj.find("Type");
      if (!type || !type->isName("Page")) continue;
      PageEntry pe;
      pe.num = e.num;
      if (const PdfObject* r = obj.find("Resources")) pe.resources = *r;
      pages.push_back(pe);
      if (pages.size() >= static_cast<size_t>(MAX_PAGES)) break;
    }
  }
  return !pages.empty();
}

bool PdfDocument::walkPageTree(const PdfObject& node, PdfObject inheritedResources, const int depth, int& budget) {
  if (depth > MAX_TREE_DEPTH || budget <= 0) return true;
  if (const PdfObject* r = node.find("Resources")) inheritedResources = *r;

  const PdfObject* type = node.find("Type");
  const PdfObject* kids = node.find("Kids");

  // Treat a node as a leaf when it has no /Kids, regardless of a missing /Type.
  if (!kids || (type && type->isName("Page"))) {
    return true;  // leaves are recorded by the caller, which knows the object number
  }

  const PdfObject kidsArr = resolve(*kids);
  if (kidsArr.type != PdfType::Array) return true;

  for (const auto& kid : kidsArr.items) {
    if (budget <= 0) break;
    if (kid.type != PdfType::Ref) continue;
    PdfObject child;
    if (!loadObject(kid.refNum, child)) continue;

    const PdfObject* childType = child.find("Type");
    const bool isLeaf = !child.find("Kids") || (childType && childType->isName("Page"));
    if (isLeaf) {
      PageEntry pe;
      pe.num = kid.refNum;
      pe.resources = child.find("Resources") ? *child.find("Resources") : inheritedResources;
      pages.push_back(pe);
      --budget;
    } else {
      walkPageTree(child, inheritedResources, depth + 1, budget);
    }
  }
  return true;
}

bool PdfDocument::getPage(const size_t i, PdfObject& outPage, PdfObject& outResources) {
  if (i >= pages.size()) return false;
  if (!loadObject(pages[i].num, outPage)) return false;
  outResources = resolve(pages[i].resources);
  return true;
}

std::string PdfDocument::documentTitle() {
  if (!infoLoaded) {
    infoLoaded = true;
    info = lookup(trailer, "Info");
  }
  if (!info.isDictLike()) return "";
  const PdfObject t = lookup(info, "Title");
  if (t.type != PdfType::String || t.text.empty()) return "";
  return pdfTextToUtf8(t.text);
}
