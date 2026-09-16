#include "PdfFontMap.h"

#include <HalStorage.h>
#include <Logging.h>

#include <algorithm>
#include <cctype>
#include <cstdlib>
#include <cstring>

#include "PdfLexer.h"

namespace {

constexpr const char* TAG = "PDFFNT";

// A ToUnicode CMap for a large CJK font can list tens of thousands of codes.
// Each mapping costs 10 bytes of index plus its UTF-8 bytes, so 24,000 of them
// is ~300KB -- the entire DRAM heap on a C3, and more than the X4 Pro can spare
// with the reader and framebuffer up. 4,000 covers every Latin font and the
// common subset of a CJK one; past that the page is not worth the crash.
constexpr size_t MAX_MAPPINGS = 4000;

void appendCodepoint(const uint32_t cp, std::string& out) {
  if (cp == 0) return;
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
}

// A CMap destination is UTF-16BE: decode surrogate pairs, and keep multi-glyph
// destinations (ligatures expanding to "ffi" and the like) intact.
std::string utf16beToUtf8(const std::string& raw) {
  std::string out;
  for (size_t i = 0; i + 1 < raw.size(); i += 2) {
    uint32_t cp = (static_cast<uint8_t>(raw[i]) << 8) | static_cast<uint8_t>(raw[i + 1]);
    if (cp >= 0xD800 && cp <= 0xDBFF && i + 3 < raw.size()) {
      const uint32_t lo = (static_cast<uint8_t>(raw[i + 2]) << 8) | static_cast<uint8_t>(raw[i + 3]);
      if (lo >= 0xDC00 && lo <= 0xDFFF) {
        cp = 0x10000 + ((cp - 0xD800) << 10) + (lo - 0xDC00);
        i += 2;
      }
    }
    appendCodepoint(cp, out);
  }
  return out;
}

// WinAnsiEncoding differs from Latin-1 only in 0x80-0x9F, so only that window
// needs a table.
constexpr uint16_t WIN_ANSI_HIGH[32] = {0x20AC, 0x0000, 0x201A, 0x0192, 0x201E, 0x2026, 0x2020, 0x2021,
                                        0x02C6, 0x2030, 0x0160, 0x2039, 0x0152, 0x0000, 0x017D, 0x0000,
                                        0x0000, 0x2018, 0x2019, 0x201C, 0x201D, 0x2022, 0x2013, 0x2014,
                                        0x02DC, 0x2122, 0x0161, 0x203A, 0x0153, 0x0000, 0x017E, 0x0178};

struct GlyphName {
  const char* name;
  uint16_t cp;
};

// The Adobe Glyph List entries that actually appear in /Differences arrays for
// Latin text. Anything outside this set falls through to the uniXXXX forms.
constexpr GlyphName GLYPH_NAMES[] = {
    {"space", 0x20},
    {"exclam", 0x21},
    {"quotedbl", 0x22},
    {"numbersign", 0x23},
    {"dollar", 0x24},
    {"percent", 0x25},
    {"ampersand", 0x26},
    {"quotesingle", 0x27},
    {"parenleft", 0x28},
    {"parenright", 0x29},
    {"asterisk", 0x2A},
    {"plus", 0x2B},
    {"comma", 0x2C},
    {"hyphen", 0x2D},
    {"period", 0x2E},
    {"slash", 0x2F},
    {"zero", 0x30},
    {"one", 0x31},
    {"two", 0x32},
    {"three", 0x33},
    {"four", 0x34},
    {"five", 0x35},
    {"six", 0x36},
    {"seven", 0x37},
    {"eight", 0x38},
    {"nine", 0x39},
    {"colon", 0x3A},
    {"semicolon", 0x3B},
    {"less", 0x3C},
    {"equal", 0x3D},
    {"greater", 0x3E},
    {"question", 0x3F},
    {"at", 0x40},
    {"bracketleft", 0x5B},
    {"backslash", 0x5C},
    {"bracketright", 0x5D},
    {"asciicircum", 0x5E},
    {"underscore", 0x5F},
    {"grave", 0x60},
    {"braceleft", 0x7B},
    {"bar", 0x7C},
    {"braceright", 0x7D},
    {"asciitilde", 0x7E},
    {"quoteleft", 0x2018},
    {"quoteright", 0x2019},
    {"quotedblleft", 0x201C},
    {"quotedblright", 0x201D},
    {"quotesinglbase", 0x201A},
    {"quotedblbase", 0x201E},
    {"endash", 0x2013},
    {"emdash", 0x2014},
    {"bullet", 0x2022},
    {"ellipsis", 0x2026},
    {"dagger", 0x2020},
    {"daggerdbl", 0x2021},
    {"perthousand", 0x2030},
    {"guilsinglleft", 0x2039},
    {"guilsinglright", 0x203A},
    {"trademark", 0x2122},
    {"fi", 0xFB01},
    {"fl", 0xFB02},
    {"ff", 0xFB00},
    {"ffi", 0xFB03},
    {"ffl", 0xFB04},
    {"nbspace", 0xA0},
    {"exclamdown", 0xA1},
    {"cent", 0xA2},
    {"sterling", 0xA3},
    {"currency", 0xA4},
    {"yen", 0xA5},
    {"section", 0xA7},
    {"dieresis", 0xA8},
    {"copyright", 0xA9},
    {"guillemotleft", 0xAB},
    {"registered", 0xAE},
    {"degree", 0xB0},
    {"plusminus", 0xB1},
    {"paragraph", 0xB6},
    {"periodcentered", 0xB7},
    {"guillemotright", 0xBB},
    {"questiondown", 0xBF},
    {"AE", 0xC6},
    {"Oslash", 0xD8},
    {"germandbls", 0xDF},
    {"ae", 0xE6},
    {"oslash", 0xF8},
    {"divide", 0xF7},
    {"multiply", 0xD7},
    {"Euro", 0x20AC},
    {"minus", 0x2212},
    {"fraction", 0x2044},
    {"florin", 0x0192},
};

// Accented Latin glyph names follow "<letter><accent>", e.g. "eacute". Handling
// the pattern covers hundreds of names that a literal table would not.
constexpr GlyphName ACCENT_SUFFIXES[] = {
    {"acute", 0}, {"grave", 1}, {"circumflex", 2}, {"tilde", 3}, {"dieresis", 4}, {"ring", 5}, {"cedilla", 6},
};

// Precomposed codepoints for A-Z/a-z crossed with the accents above; 0 where no
// such character exists.
constexpr uint16_t ACCENTED[7][52] = {
    // acute
    {0xC1,  0,    0x106,  0, 0xC9,  0,     0x1F4, 0,    0xCD, 0,      0x1E30, 0x139, 0x1E3E,
     0x143, 0xD3, 0x1E54, 0, 0x154, 0x15A, 0,     0xDA, 0,    0x1E82, 0,      0xDD,  0x179,
     0xE1,  0,    0x107,  0, 0xE9,  0,     0x1F5, 0,    0xED, 0,      0x1E31, 0x13A, 0x1E3F,
     0x144, 0xF3, 0x1E55, 0, 0x155, 0x15B, 0,     0xFA, 0,    0x1E83, 0,      0xFD,  0x17A},
    // grave
    {0xC0, 0, 0, 0, 0xC8, 0, 0, 0, 0xCC, 0, 0, 0, 0, 0x1F8, 0xD2, 0, 0, 0, 0, 0, 0xD9, 0, 0x1E80, 0, 0x1EF2, 0,
     0xE0, 0, 0, 0, 0xE8, 0, 0, 0, 0xEC, 0, 0, 0, 0, 0x1F9, 0xF2, 0, 0, 0, 0, 0, 0xF9, 0, 0x1E81, 0, 0x1EF3, 0},
    // circumflex
    {0xC2,  0, 0x108, 0, 0xCA,  0, 0x11C, 0x124, 0xCE,  0x134, 0,     0, 0,     0, 0xD4,  0,     0,    0,
     0x15C, 0, 0xDB,  0, 0x174, 0, 0x176, 0,     0xE2,  0,     0x109, 0, 0xEA,  0, 0x11D, 0x125, 0xEE, 0x135,
     0,     0, 0,     0, 0xF4,  0, 0,     0,     0x15D, 0,     0xFB,  0, 0x175, 0, 0x177, 0},
    // tilde
    {0xC3, 0, 0, 0, 0x1EBC, 0, 0, 0, 0x128, 0, 0, 0, 0, 0xD1, 0xD5, 0, 0, 0, 0, 0, 0x168, 0x1E7C, 0, 0, 0x1EF8, 0,
     0xE3, 0, 0, 0, 0x1EBD, 0, 0, 0, 0x129, 0, 0, 0, 0, 0xF1, 0xF5, 0, 0, 0, 0, 0, 0x169, 0x1E7D, 0, 0, 0x1EF9, 0},
    // dieresis
    {0xC4, 0,      0,    0, 0xCB,   0,      0,     0x1E26, 0xCF, 0, 0,    0, 0,      0,      0xD6, 0,      0,    0,
     0,    0x1E97, 0xDC, 0, 0x1E84, 0x1E8C, 0x178, 0,      0xE4, 0, 0,    0, 0xEB,   0,      0,    0x1E27, 0xEF, 0,
     0,    0,      0,    0, 0xF6,   0,      0,     0,      0,    0, 0xFC, 0, 0x1E85, 0x1E8D, 0xFF, 0},
    // ring
    {0xC5, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0x16E, 0, 0, 0, 0, 0,
     0xE5, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0x16F, 0, 0, 0, 0, 0},
    // cedilla
    {0,     0, 0xC7, 0x1E10, 0x228, 0,     0x122, 0x1E28, 0, 0, 0x136, 0x13B, 0,
     0x145, 0, 0,    0,      0x156, 0x15E, 0x162, 0,      0, 0, 0,     0,     0,
     0,     0, 0xE7, 0x1E11, 0x229, 0,     0x123, 0x1E29, 0, 0, 0x137, 0x13C, 0,
     0x146, 0, 0,    0,      0x157, 0x15F, 0x163, 0,      0, 0, 0,     0,     0},
};

uint32_t glyphNameToUnicode(const std::string& name) {
  if (name.empty()) return 0;

  // uniXXXX and uXXXX[XX] are defined by the AGL algorithm itself.
  if (name.size() >= 7 && name.compare(0, 3, "uni") == 0) {
    return static_cast<uint32_t>(std::strtoul(name.substr(3, 4).c_str(), nullptr, 16));
  }
  if (name.size() >= 5 && name[0] == 'u') {
    bool hex = true;
    for (size_t i = 1; i < name.size(); ++i) {
      if (!std::isxdigit(static_cast<unsigned char>(name[i]))) {
        hex = false;
        break;
      }
    }
    if (hex) return static_cast<uint32_t>(std::strtoul(name.c_str() + 1, nullptr, 16));
  }

  if (name.size() == 1) {
    const char c = name[0];
    if ((c >= 'A' && c <= 'Z') || (c >= 'a' && c <= 'z')) return static_cast<uint32_t>(c);
  }

  for (const auto& [gname, cp] : GLYPH_NAMES) {
    if (name == gname) return cp;
  }

  // "<letter><accent>" composites.
  if (name.size() > 1) {
    const char letter = name[0];
    int idx = -1;
    if (letter >= 'A' && letter <= 'Z') idx = letter - 'A';
    if (letter >= 'a' && letter <= 'z') idx = 26 + (letter - 'a');
    if (idx >= 0) {
      const std::string suffix = name.substr(1);
      for (const auto& [sname, row] : ACCENT_SUFFIXES) {
        if (suffix == sname) return ACCENTED[row][idx];
      }
    }
  }
  return 0;
}

// Names ending in ".sc", ".alt", ".01" and similar are styled variants of the
// base glyph; strip the suffix before looking the name up.
std::string stripGlyphSuffix(const std::string& name) {
  const size_t dot = name.find('.');
  return dot == std::string::npos ? name : name.substr(0, dot);
}

}  // namespace

void PdfFontMap::setMapping(const uint32_t code, const std::string& utf8) {
  if (utf8.empty() || mappings.size() >= MAX_MAPPINGS) return;
  const auto at = std::lower_bound(mappings.begin(), mappings.end(), code,
                                   [](const Mapping& m, const uint32_t c) { return m.code < c; });
  if (at != mappings.end() && at->code == code) {
    at->offset = static_cast<uint32_t>(blob.size());
    at->len = static_cast<uint16_t>(utf8.size());
  } else {
    mappings.insert(at, Mapping{code, static_cast<uint32_t>(blob.size()), static_cast<uint16_t>(utf8.size())});
  }
  blob += utf8;
}

void PdfFontMap::appendUtf8(const uint32_t code, std::string& out) const {
  const auto at = std::lower_bound(mappings.begin(), mappings.end(), code,
                                   [](const Mapping& m, const uint32_t c) { return m.code < c; });
  if (at != mappings.end() && at->code == code) {
    out.append(blob, at->offset, at->len);
    return;
  }
  // No mapping. A single-byte font almost always means Latin-1 here; a
  // two-byte font would produce noise, so it stays silent.
  if (!twoByte && code >= 0x20 && code < 0x100) appendCodepoint(code, out);
}

int PdfFontMap::widthOf(const uint32_t code) const {
  const auto at = std::upper_bound(widths.begin(), widths.end(), code,
                                   [](const uint32_t c, const WidthRange& r) { return c < r.first; });
  if (at != widths.begin()) {
    const WidthRange& r = *(at - 1);
    if (code >= r.first && code <= r.last) return r.width;
  }
  return defaultWidth;
}

void PdfFontMap::load(PdfDocument& doc, const PdfObject& fontDict, const std::string& scratchDir) {
  const PdfObject subtype = doc.lookup(fontDict, "Subtype");
  PdfObject descendant;

  if (subtype.isName("Type0")) {
    // Type0 codes are almost always two bytes: Identity-H/V and the predefined
    // CJK CMaps all use a two-byte code space.
    twoByte = true;
    const PdfObject desc = doc.lookup(fontDict, "DescendantFonts");
    if (desc.type == PdfType::Array && !desc.items.empty()) descendant = doc.resolve(desc.items[0]);
  }

  // /ToUnicode is authoritative when present.
  const PdfObject toUnicode = doc.lookup(fontDict, "ToUnicode");
  if (toUnicode.type == PdfType::Stream) parseToUnicode(doc, toUnicode, scratchDir);

  if (mappings.empty() && !twoByte) applyBaseEncoding(doc, fontDict);

  if (descendant.isDictLike()) {
    const PdfObject dw = doc.lookup(descendant, "DW");
    defaultWidth = dw.isNumber() ? static_cast<int>(dw.asInt()) : 1000;
    loadCidWidths(doc, descendant);
  } else {
    loadSimpleWidths(doc, fontDict);
  }
}

void PdfFontMap::applyBaseEncoding(PdfDocument& doc, const PdfObject& fontDict) {
  bool winAnsi = false;
  PdfObject differences;

  const PdfObject enc = doc.lookup(fontDict, "Encoding");
  if (enc.type == PdfType::Name) {
    winAnsi = enc.text == "WinAnsiEncoding";
  } else if (enc.isDictLike()) {
    const PdfObject base = doc.lookup(enc, "BaseEncoding");
    winAnsi = base.isName("WinAnsiEncoding");
    differences = doc.lookup(enc, "Differences");
  }

  // Seed the whole single-byte range, then let /Differences override.
  std::string utf8;
  for (uint32_t c = 0x20; c < 0x100; ++c) {
    uint32_t cp = c;
    if (winAnsi && c >= 0x80 && c < 0xA0) cp = WIN_ANSI_HIGH[c - 0x80];
    if (cp == 0) continue;
    utf8.clear();
    appendCodepoint(cp, utf8);
    setMapping(c, utf8);
  }

  // /Differences is [code name name ... code name ...]: each integer resets the
  // running code, each name assigns the current code and advances it.
  if (differences.type == PdfType::Array) {
    uint32_t code = 0;
    for (const auto& item : differences.items) {
      if (item.isNumber()) {
        code = static_cast<uint32_t>(item.asInt());
      } else if (item.type == PdfType::Name) {
        const uint32_t cp = glyphNameToUnicode(stripGlyphSuffix(item.text));
        if (cp != 0) {
          utf8.clear();
          appendCodepoint(cp, utf8);
          setMapping(code, utf8);
        }
        ++code;
      }
    }
  }
}

void PdfFontMap::parseToUnicode(PdfDocument& doc, const PdfObject& streamObj, const std::string& scratchDir) {
  const std::string decoded = scratchDir + "/tounicode.bin";
  if (!doc.decodeStream(streamObj, decoded)) return;

  PdfFileSource cmap;
  if (!cmap.open(TAG, decoded)) {
    Storage.remove(decoded.c_str());
    return;
  }

  PdfLexer lexer(cmap);
  std::vector<PdfObject> operands;
  operands.reserve(8);

  while (true) {
    const size_t before = cmap.tell();
    PdfObject obj = lexer.parseObject();
    if (cmap.tell() == before) break;  // no progress: end of data

    if (obj.type != PdfType::Keyword) {
      if (operands.size() < 8) operands.push_back(std::move(obj));
      continue;
    }

    if (obj.text == "beginbfchar") {
      // Pairs of <srcCode> <dstString> until endbfchar.
      while (true) {
        PdfObject src = lexer.parseObject();
        if (src.type != PdfType::String) break;
        PdfObject dst = lexer.parseObject();
        if (dst.type != PdfType::String) break;
        uint32_t code = 0;
        for (const char c : src.text) code = (code << 8) | static_cast<uint8_t>(c);
        setMapping(code, utf16beToUtf8(dst.text));
      }
    } else if (obj.text == "beginbfrange") {
      // Triples of <lo> <hi> <dst>, where dst is a string or an array of them.
      while (true) {
        PdfObject lo = lexer.parseObject();
        if (lo.type != PdfType::String) break;
        PdfObject hi = lexer.parseObject();
        if (hi.type != PdfType::String) break;
        PdfObject dst = lexer.parseObject();

        uint32_t loCode = 0, hiCode = 0;
        for (const char c : lo.text) loCode = (loCode << 8) | static_cast<uint8_t>(c);
        for (const char c : hi.text) hiCode = (hiCode << 8) | static_cast<uint8_t>(c);
        if (hiCode < loCode) continue;
        // A malformed range must not turn into a multi-million iteration loop.
        if (hiCode - loCode > MAX_MAPPINGS) hiCode = loCode + MAX_MAPPINGS;

        if (dst.type == PdfType::Array) {
          for (uint32_t i = 0; i <= hiCode - loCode && i < dst.items.size(); ++i) {
            if (dst.items[i].type == PdfType::String) setMapping(loCode + i, utf16beToUtf8(dst.items[i].text));
          }
        } else if (dst.type == PdfType::String) {
          // The destination increments with the code, in its last two bytes.
          std::string base = dst.text;
          if (base.size() < 2) continue;
          for (uint32_t i = 0; i <= hiCode - loCode; ++i) {
            std::string cur = base;
            uint32_t tail = (static_cast<uint8_t>(cur[cur.size() - 2]) << 8) | static_cast<uint8_t>(cur.back());
            tail = (tail + i) & 0xFFFF;
            cur[cur.size() - 2] = static_cast<char>(tail >> 8);
            cur.back() = static_cast<char>(tail & 0xFF);
            setMapping(loCode + i, utf16beToUtf8(cur));
            if (mappings.size() >= MAX_MAPPINGS) break;
          }
        }
      }
    } else if (obj.text == "begincodespacerange") {
      // The code space width decides how the extractor slices a show string.
      PdfObject lo = lexer.parseObject();
      if (lo.type == PdfType::String && lo.text.size() >= 2) twoByte = true;
    }
    operands.clear();
  }

  cmap.close();
  Storage.remove(decoded.c_str());
}

void PdfFontMap::loadSimpleWidths(PdfDocument& doc, const PdfObject& fontDict) {
  const PdfObject firstChar = doc.lookup(fontDict, "FirstChar");
  const PdfObject widthsArr = doc.lookup(fontDict, "Widths");
  if (widthsArr.type != PdfType::Array || widthsArr.items.empty()) {
    // No /Widths: a Courier-style monospace guess is wrong for most fonts, but
    // half an em is the right average for the proportional faces PDFs embed.
    defaultWidth = 500;
    return;
  }

  const auto first = static_cast<uint32_t>(firstChar.asInt(0));
  widths.reserve(widthsArr.items.size());
  for (size_t i = 0; i < widthsArr.items.size(); ++i) {
    const PdfObject w = doc.resolve(widthsArr.items[i]);
    if (!w.isNumber()) continue;
    const auto code = static_cast<uint32_t>(first + i);
    widths.push_back(WidthRange{code, code, static_cast<int>(w.asNumber())});
  }
  const PdfObject descriptor = doc.lookup(fontDict, "FontDescriptor");
  const PdfObject missing = doc.lookup(descriptor, "MissingWidth");
  // The spec default is 0, but a zero advance makes every unlisted code look
  // like a zero-width glyph and floods the output with spurious spaces.
  defaultWidth = missing.isNumber() ? static_cast<int>(missing.asInt()) : 500;
}

void PdfFontMap::loadCidWidths(PdfDocument& doc, const PdfObject& descendant) {
  const PdfObject w = doc.lookup(descendant, "W");
  if (w.type != PdfType::Array) return;

  // /W is a mix of "<c> [w w w ...]" and "<cFirst> <cLast> <w>" groups.
  widths.reserve(w.items.size() / 2);
  size_t i = 0;
  while (i < w.items.size()) {
    const PdfObject a = doc.resolve(w.items[i]);
    if (!a.isNumber() || i + 1 >= w.items.size()) break;
    const PdfObject b = doc.resolve(w.items[i + 1]);

    if (b.type == PdfType::Array) {
      const auto start = static_cast<uint32_t>(a.asInt());
      for (size_t k = 0; k < b.items.size(); ++k) {
        const PdfObject val = doc.resolve(b.items[k]);
        if (!val.isNumber()) continue;
        const auto code = static_cast<uint32_t>(start + k);
        widths.push_back(WidthRange{code, code, static_cast<int>(val.asNumber())});
      }
      i += 2;
    } else if (b.isNumber() && i + 2 < w.items.size()) {
      const PdfObject val = doc.resolve(w.items[i + 2]);
      if (val.isNumber()) {
        widths.push_back(WidthRange{static_cast<uint32_t>(a.asInt()), static_cast<uint32_t>(b.asInt()),
                                    static_cast<int>(val.asNumber())});
      }
      i += 3;
    } else {
      break;
    }
  }
  std::sort(widths.begin(), widths.end(), [](const WidthRange& x, const WidthRange& y) { return x.first < y.first; });
}
