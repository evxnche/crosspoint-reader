#pragma once

#include <cstddef>
#include <cstdint>
#include <string>
#include <vector>

#include "PdfSource.h"
#include "PdfTypes.h"

// Random-access view of a PDF file: object lookup, stream decoding, and the
// page list.
//
// Objects are read from the card on demand. Nothing but the object index, the
// page list, and a small most-recently-used cache is retained, so a 400-page
// document costs roughly the same RAM as a 4-page one.
class PdfDocument {
 public:
  PdfDocument() = default;
  ~PdfDocument() { close(); }

  PdfDocument(const PdfDocument&) = delete;
  PdfDocument& operator=(const PdfDocument&) = delete;

  // `scratchDir` must already exist; decoded streams are written there and
  // removed again as they are superseded.
  bool open(const std::string& path, const std::string& scratchDir);
  void close();

  [[nodiscard]] bool isOpen() const { return src.isOpen(); }
  // True when the file declares an /Encrypt dictionary. Encrypted documents are
  // rejected rather than half-decoded into mojibake.
  [[nodiscard]] bool isEncrypted() const { return encrypted; }

  [[nodiscard]] size_t pageCount() const { return pages.size(); }

  // Page dictionary for index `i`, with /Resources inherited from ancestors
  // when the leaf does not carry its own.
  bool getPage(size_t i, PdfObject& outPage, PdfObject& outResources);

  // Follow a reference to its target. Non-references are returned unchanged.
  PdfObject resolve(const PdfObject& o);

  // resolve() of a dictionary entry, or a Null object when the key is absent.
  PdfObject lookup(const PdfObject& dict, const char* key);

  // Decode a stream object's payload into `outPath`. Returns false when the
  // stream uses a filter this reader does not implement (notably image codecs).
  bool decodeStream(const PdfObject& streamObj, const std::string& outPath);

  // /Info /Title, empty when absent or not representable.
  [[nodiscard]] std::string documentTitle();

  [[nodiscard]] const std::string& scratchPath() const { return scratch; }

 private:
  struct XrefEntry {
    uint32_t num = 0;
    uint32_t offset = 0;     // byte offset, or index within the container
    uint32_t container = 0;  // object number of the holding /ObjStm
    bool inObjStm = false;
  };

  struct CacheSlot {
    uint32_t num = 0;
    bool valid = false;
    PdfObject obj;
  };

  // Index construction.
  bool readTrailerChain();
  bool readXrefSection(size_t offset, int depth, PdfObject& outTrailer);
  bool readClassicXref(int depth, PdfObject& outTrailer);
  bool readXrefStream(const PdfObject& streamObj, int depth, PdfObject& outTrailer);
  bool scanAllObjects();
  void registerObjStmContents(uint32_t containerNum);
  bool findCatalogByScan(PdfObject& outCatalog);

  void setEntry(const XrefEntry& e);
  const XrefEntry* findEntry(uint32_t num) const;

  // Object access.
  bool loadObject(uint32_t num, PdfObject& out);
  bool loadFromObjStm(const XrefEntry& e, PdfObject& out);
  // Resolves an indirect /Length and, failing that, locates `endstream`.
  void fixStreamLength(PdfObject& streamObj, PdfSource& owner);

  // Page tree.
  bool buildPageList(const PdfObject& catalog);
  bool walkPageTree(const PdfObject& node, PdfObject inheritedResources, int depth, int& budget);

  struct PageEntry {
    uint32_t num = 0;
    PdfObject resources;  // usually a Ref, so this stays a few bytes per page
  };

  PdfFileSource src;
  std::string scratch;
  std::string filePath;

  std::vector<XrefEntry> xref;  // sorted by num
  std::vector<PageEntry> pages;
  PdfObject trailer;
  PdfObject info;
  bool infoLoaded = false;
  bool encrypted = false;
  bool scanned = false;  // brute-force scan already done

  // Decoded /ObjStm currently spilled to scratch, kept so consecutive lookups
  // into the same container do not re-inflate it.
  uint32_t objStmCached = 0;
  bool objStmValid = false;
  PdfFileSource objStmSrc;
  std::vector<std::pair<uint32_t, uint32_t>> objStmIndex;  // objNum -> offset
  size_t objStmFirst = 0;

  static constexpr size_t CACHE_SLOTS = 8;
  CacheSlot cache[CACHE_SLOTS];
  size_t cacheNext = 0;
};
