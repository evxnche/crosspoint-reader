#include "PdfTextExtractor.h"

#include <Arduino.h>
#include <HalStorage.h>
#include <Logging.h>
#include <Memory.h>

#include <algorithm>
#include <cmath>
#include <cstring>

#include "PdfFilters.h"
#include "PdfFontMap.h"
#include "PdfLexer.h"

namespace {

constexpr const char* TAG = "PDFTXT";

constexpr int MAX_FORM_DEPTH = 4;
// A single paragraph past this length is a layout the heuristics misread; break
// it rather than growing one allocation without bound.
constexpr size_t MAX_PARAGRAPH = 16384;

// Fractions of the current font size. These are the thresholds that decide
// where lines and paragraphs are, and they are the difference between prose and
// a wall of fragments.
constexpr double SAME_LINE_TOLERANCE = 0.35;  // baseline drift within one line
constexpr double WORD_GAP = 0.22;             // horizontal gap that means a space
constexpr double PARAGRAPH_GAP = 1.6;         // vertical drop that ends a paragraph
constexpr double INDENT_THRESHOLD = 1.2;      // first-line indent that starts one
// Once a paragraph has two lines its own leading is known, and extra space is
// measured against that rather than against the font size. Books set leading
// anywhere from 1.1 to 1.6 em, so a fixed multiple of the font size either
// splits every line or never splits at all.
constexpr double LEADING_SLACK = 1.35;
// A different type size is a different element: a heading is not a continuation
// of the paragraph above it, however close the two sit.
constexpr double SIZE_CHANGE = 0.18;
// Above this length an unterminated paragraph at a page break really is a
// sentence continuing overleaf, rather than a title or heading sitting alone.
constexpr size_t CONTINUATION_MIN_CHARS = 120;

// The band at the top and bottom of the page where running heads and folios sit.
constexpr double MARGIN_BAND = 0.055;
constexpr size_t MAX_MARGIN_RUN = 50;

// True when the line carries no visible character. Producers emit these as a
// space at the blank line's baseline rather than omitting the line.
bool isBlank(const std::string& s) {
  for (size_t i = 0; i < s.size(); ++i) {
    const auto c = static_cast<unsigned char>(s[i]);
    if (c == ' ' || c == '\t') continue;
    // U+00A0 and the U+2000..U+200B space family, as UTF-8.
    if (c == 0xC2 && i + 1 < s.size() && static_cast<unsigned char>(s[i + 1]) == 0xA0) {
      ++i;
      continue;
    }
    if (c == 0xE2 && i + 2 < s.size() && static_cast<unsigned char>(s[i + 1]) == 0x80 &&
        static_cast<unsigned char>(s[i + 2]) <= 0x8B) {
      i += 2;
      continue;
    }
    return false;
  }
  return true;
}

bool endsSentence(const std::string& s) {
  for (size_t i = s.size(); i-- > 0;) {
    const char c = s[i];
    if (c == '"' || c == '\'' || c == ')' || c == ']') continue;
    // Closing curly quotes are multi-byte; their final byte is enough to skip.
    if (static_cast<unsigned char>(c) >= 0x80) continue;
    return c == '.' || c == '!' || c == '?' || c == ':';
  }
  return false;
}

}  // namespace

PdfTextExtractor::Matrix PdfTextExtractor::multiply(const Matrix& m, const Matrix& n) {
  Matrix r;
  r.a = m.a * n.a + m.b * n.c;
  r.b = m.a * n.b + m.b * n.d;
  r.c = m.c * n.a + m.d * n.c;
  r.d = m.c * n.b + m.d * n.d;
  r.e = m.e * n.a + m.f * n.c + n.e;
  r.f = m.e * n.b + m.f * n.d + n.f;
  return r;
}

void PdfTextExtractor::emit(const std::string& s) {
  if (s.empty() || !out) return;
  out->write(reinterpret_cast<const uint8_t*>(s.data()), s.size());
  charCount += s.size();
}

void PdfTextExtractor::emitParagraph() {
  if (paragraph.empty()) return;
  // Trim trailing spaces so the reader never wraps on a dangling blank.
  while (!paragraph.empty() && paragraph.back() == ' ') paragraph.pop_back();
  if (!paragraph.empty()) {
    paragraph += "\n\n";
    emit(paragraph);
  }
  paragraph.clear();
  paragraphLeading = 0;
}

void PdfTextExtractor::flushParagraph() {
  flushLine();
  emitParagraph();
}

void PdfTextExtractor::flushLine() {
  if (!haveLine) {
    line.clear();
    return;
  }
  haveLine = false;

  std::string current;
  current.swap(line);
  if (current.empty()) return;

  // Headers and footers: short, alone, and pressed against a page edge. Their
  // baseline must not become the reference for the next line's spacing either.
  if (opts.stripHeadersFooters && current.size() <= MAX_MARGIN_RUN && (lineY >= pageTop || lineY <= pageBottom)) {
    haveLastLine = false;
    return;
  }

  // Geometry outlives the line itself: many producers wrap every single line in
  // its own BT/ET block, so the next line arrives with no line in progress and
  // the paragraph decision has nothing else to measure against.
  lastLineY = lineY;
  lastLineScale = lineScale;
  lastLineStartX = lineStartX;
  haveLastLine = true;

  if (isBlank(current)) {
    // A line of nothing but spaces is how a laid-out page separates paragraphs.
    emitParagraph();
    return;
  }

  if (paragraph.empty()) {
    paragraph = current;
    paragraphIndent = lineStartX;
  } else if (opts.dehyphenate && paragraph.size() > 1 && paragraph.back() == '-') {
    // A hyphen at a line break splits one word; drop it and close the seam.
    paragraph.pop_back();
    paragraph += current;
  } else {
    if (paragraph.back() != ' ') paragraph += ' ';
    paragraph += current;
  }

  pageProducedText = true;
  if (paragraph.size() > MAX_PARAGRAPH) emitParagraph();
}

void PdfTextExtractor::startLine(const std::string& text, const double startX, const double y, const double scale,
                                 const double endX) {
  line = text;
  lineStartX = startX;
  lineY = y;
  lineScale = scale > 0 ? scale : 1;
  lineEndX = endX;
  haveLine = true;
}

void PdfTextExtractor::breakBeforeLine(const double startX, const double y, const double scale) {
  const double reference = lastLineScale > 0 ? lastLineScale : scale;
  const double drop = lastLineY - y;  // positive when the text moved down the page
  const double tolerance = SAME_LINE_TOLERANCE * reference;

  const bool movedUp = drop < -tolerance;  // new column, or a new block
  const bool bigGap = paragraphLeading > 0 ? drop > paragraphLeading * LEADING_SLACK : drop > PARAGRAPH_GAP * reference;
  const bool indented = !paragraph.empty() && startX - paragraphIndent > INDENT_THRESHOLD * reference;
  const bool sizeChanged = reference > 0 && scale > 0 && std::fabs(scale - reference) > SIZE_CHANGE * reference;

  if (movedUp || bigGap || indented || sizeChanged) {
    emitParagraph();
  } else if (paragraphLeading <= 0 && drop > 0) {
    // First continuation line of this paragraph: adopt its leading as the
    // yardstick for every later line in it.
    paragraphLeading = drop;
  }
}

void PdfTextExtractor::placeRun(const std::string& text, const double startX, const double y, const double scale,
                                const double endX) {
  if (text.empty()) return;

  if (haveLine) {
    if (std::fabs(y - lineY) <= SAME_LINE_TOLERANCE * lineScale) {
      // Same baseline: a wide enough gap is a space, anything less is kerning.
      if (startX - lineEndX > WORD_GAP * lineScale && !line.empty() && line.back() != ' ') line += ' ';
      line += text;
      lineEndX = std::max(lineEndX, endX);
      return;
    }
    flushLine();
  }

  if (pageJustStarted) {
    pageJustStarted = false;
    // Only a long, unfinished sentence is treated as running over the page
    // break. A short unterminated block is a title or a heading that owns its
    // page, and joining it to the next page's first line reads as nonsense.
    const bool continues = paragraph.size() >= CONTINUATION_MIN_CHARS && !endsSentence(paragraph);
    if (!paragraph.empty() && !continues) emitParagraph();
  } else if (haveLastLine) {
    breakBeforeLine(startX, y, scale > 0 ? scale : 1);
  }

  startLine(text, startX, y, scale, endX);
}

void PdfTextExtractor::showString(const std::string& bytes, const double tjAdjust) {
  if (!ts.font) return;

  // TJ adjustments are applied before the string they precede.
  if (tjAdjust != 0) {
    const double shift = -tjAdjust / 1000.0 * ts.fontSize * ts.horizScale;
    Matrix t;
    t.e = shift;
    ts.tm = multiply(t, ts.tm);
  }
  if (bytes.empty()) return;

  const Matrix trm = multiply(ts.tm, currentCtm);
  const double startX = trm.e;
  const double y = trm.f;
  // The on-page size of one em, which every threshold is expressed against.
  const double scale = ts.fontSize * std::sqrt(std::fabs(currentCtm.a * currentCtm.d - currentCtm.b * currentCtm.c));

  std::string text;
  text.reserve(bytes.size());
  double advance = 0;

  const bool two = ts.font->isTwoByte();
  const size_t step = two ? 2 : 1;
  for (size_t i = 0; i + step <= bytes.size(); i += step) {
    uint32_t code = static_cast<uint8_t>(bytes[i]);
    if (two) code = (code << 8) | static_cast<uint8_t>(bytes[i + 1]);

    ts.font->appendUtf8(code, text);

    double w = ts.font->widthOf(code) / 1000.0 * ts.fontSize + ts.charSpacing;
    // Word spacing applies to the single byte 32, never to a two-byte code.
    if (!two && code == 32) w += ts.wordSpacing;
    advance += w * ts.horizScale;
  }

  Matrix t;
  t.e = advance;
  ts.tm = multiply(t, ts.tm);

  const Matrix endTrm = multiply(ts.tm, currentCtm);
  placeRun(text, startX, y, scale, endTrm.e);
}

const PdfFontMap* PdfTextExtractor::fontFor(PdfDocument& doc, const PdfObject& resources, const std::string& name) {
  const PdfObject fontsDict = doc.lookup(resources, "Font");
  if (!fontsDict.isDictLike()) return nullptr;
  const PdfObject* entry = fontsDict.find(name.c_str());
  if (!entry) return nullptr;

  // Cache on the font's object number so the same face shared across pages is
  // parsed once.
  const uint32_t key = entry->type == PdfType::Ref ? entry->refNum : 0;
  if (key != 0) {
    for (const auto& slot : fonts) {
      if (slot.valid && slot.objNum == key) return slot.map.get();
    }
  }

  const PdfObject fontDict = doc.resolve(*entry);
  if (!fontDict.isDictLike()) return nullptr;

  auto map = makeUniqueNoThrow<PdfFontMap>();
  if (!map) {
    LOG_ERR(TAG, "OOM: font map");
    return nullptr;
  }
  map->load(doc, fontDict, scratchDir);

  FontSlot& slot = fonts[fontNext];
  fontNext = (fontNext + 1) % FONT_SLOTS;
  slot.objNum = key;
  slot.valid = true;
  slot.map = std::move(map);
  return slot.map.get();
}

bool PdfTextExtractor::runContent(PdfDocument& doc, PdfSource& content, const PdfObject& resources, const Matrix& ctm,
                                  const int depth) {
  if (depth > MAX_FORM_DEPTH) return true;

  PdfLexer lexer(content);
  std::vector<PdfObject> stack;
  stack.reserve(8);
  std::vector<Matrix> ctmStack;
  std::vector<TextState> tsStack;

  currentCtm = ctm;

  while (true) {
    const size_t before = content.tell();
    PdfObject obj = lexer.parseObject();
    if (content.tell() == before) break;  // no progress: end of stream

    if (obj.type != PdfType::Keyword) {
      // Operands accumulate until an operator consumes them. A runaway stack
      // means malformed content; keep only what an operator could use.
      if (stack.size() >= 16) stack.erase(stack.begin());
      stack.push_back(std::move(obj));
      continue;
    }

    const std::string& op = obj.text;
    const auto num = [&](const size_t fromEnd) {
      return stack.size() > fromEnd ? stack[stack.size() - 1 - fromEnd].asNumber() : 0.0;
    };

    if (op == "BT") {
      ts.tm = Matrix{};
      ts.tlm = Matrix{};
    } else if (op == "ET") {
      flushLine();
    } else if (op == "q") {
      ctmStack.push_back(currentCtm);
      tsStack.push_back(ts);
    } else if (op == "Q") {
      if (!ctmStack.empty()) {
        currentCtm = ctmStack.back();
        ctmStack.pop_back();
      }
      if (!tsStack.empty()) {
        // Only the font-related parameters are part of the graphics state; the
        // text matrix belongs to the enclosing BT/ET block.
        const TextState saved = tsStack.back();
        tsStack.pop_back();
        ts.font = saved.font;
        ts.fontSize = saved.fontSize;
        ts.charSpacing = saved.charSpacing;
        ts.wordSpacing = saved.wordSpacing;
        ts.horizScale = saved.horizScale;
        ts.leading = saved.leading;
        ts.rise = saved.rise;
      }
    } else if (op == "cm" && stack.size() >= 6) {
      Matrix m;
      m.a = num(5);
      m.b = num(4);
      m.c = num(3);
      m.d = num(2);
      m.e = num(1);
      m.f = num(0);
      currentCtm = multiply(m, currentCtm);
    } else if (op == "Tf" && stack.size() >= 2) {
      ts.fontSize = num(0);
      const PdfObject& nameObj = stack[stack.size() - 2];
      if (nameObj.type == PdfType::Name) ts.font = fontFor(doc, resources, nameObj.text);
    } else if (op == "Td" && stack.size() >= 2) {
      Matrix t;
      t.e = num(1);
      t.f = num(0);
      ts.tlm = multiply(t, ts.tlm);
      ts.tm = ts.tlm;
    } else if (op == "TD" && stack.size() >= 2) {
      ts.leading = -num(0);
      Matrix t;
      t.e = num(1);
      t.f = num(0);
      ts.tlm = multiply(t, ts.tlm);
      ts.tm = ts.tlm;
    } else if (op == "Tm" && stack.size() >= 6) {
      Matrix m;
      m.a = num(5);
      m.b = num(4);
      m.c = num(3);
      m.d = num(2);
      m.e = num(1);
      m.f = num(0);
      ts.tlm = m;
      ts.tm = m;
    } else if (op == "T*") {
      Matrix t;
      t.f = -ts.leading;
      ts.tlm = multiply(t, ts.tlm);
      ts.tm = ts.tlm;
    } else if (op == "TL") {
      ts.leading = num(0);
    } else if (op == "Tc") {
      ts.charSpacing = num(0);
    } else if (op == "Tw") {
      ts.wordSpacing = num(0);
    } else if (op == "Tz") {
      ts.horizScale = num(0) / 100.0;
    } else if (op == "Ts") {
      ts.rise = num(0);
    } else if (op == "Tj" || op == "'" || op == "\"") {
      if (op != "Tj") {
        // ' and " advance to the next line first; " also sets word and char spacing.
        if (op == "\"" && stack.size() >= 3) {
          ts.wordSpacing = num(2);
          ts.charSpacing = num(1);
        }
        Matrix t;
        t.f = -ts.leading;
        ts.tlm = multiply(t, ts.tlm);
        ts.tm = ts.tlm;
      }
      if (!stack.empty() && stack.back().type == PdfType::String) showString(stack.back().text, 0);
    } else if (op == "TJ") {
      if (!stack.empty() && stack.back().type == PdfType::Array) {
        const PdfObject arr = stack.back();
        double pending = 0;
        for (const auto& item : arr.items) {
          if (item.type == PdfType::String) {
            showString(item.text, pending);
            pending = 0;
          } else if (item.isNumber()) {
            pending += item.asNumber();
          }
        }
        if (pending != 0) showString("", pending);
      }
    } else if (op == "Do") {
      // A form XObject is a nested content stream with its own matrix and,
      // optionally, its own resources. Skipping these loses real body text.
      if (!stack.empty() && stack.back().type == PdfType::Name && depth < MAX_FORM_DEPTH) {
        const std::string xname = stack.back().text;
        const PdfObject xobjects = doc.lookup(resources, "XObject");
        const PdfObject form = doc.lookup(xobjects, xname.c_str());
        const PdfObject subtype = doc.lookup(form, "Subtype");
        if (form.type == PdfType::Stream && subtype.isName("Form")) {
          const std::string formPath = scratchDir + "/form" + std::to_string(depth) + ".bin";
          if (doc.decodeStream(form, formPath)) {
            PdfFileSource formSrc;
            if (formSrc.open(TAG, formPath)) {
              Matrix formCtm = currentCtm;
              if (const PdfObject m = doc.lookup(form, "Matrix"); m.type == PdfType::Array && m.items.size() >= 6) {
                Matrix fm;
                fm.a = m.items[0].asNumber(1);
                fm.b = m.items[1].asNumber(0);
                fm.c = m.items[2].asNumber(0);
                fm.d = m.items[3].asNumber(1);
                fm.e = m.items[4].asNumber(0);
                fm.f = m.items[5].asNumber(0);
                formCtm = multiply(fm, currentCtm);
              }
              PdfObject formRes = doc.lookup(form, "Resources");
              if (!formRes.isDictLike()) formRes = resources;

              // The nested run owns the interpreter state; save and restore it.
              const TextState savedTs = ts;
              const Matrix savedCtm = currentCtm;
              runContent(doc, formSrc, formRes, formCtm, depth + 1);
              ts = savedTs;
              currentCtm = savedCtm;
              formSrc.close();
            }
            Storage.remove(formPath.c_str());
          }
        }
      }
    } else if (op == "BI") {
      // Inline image: the binary samples between ID and EI are not PDF syntax
      // and would tokenise into garbage.
      // Skip the dictionary up to ID, then the samples up to a delimited EI.
      int prev = -1;
      int c;
      while ((c = content.get()) >= 0) {
        if (prev == 'I' && c == 'D') break;
        prev = c;
      }
      (void)content.get();  // the single whitespace byte after ID
      prev = -1;
      while ((c = content.get()) >= 0) {
        if (prev == 'E' && c == 'I') {
          const int after = content.get();
          if (after < 0 || PdfLexer::isWhitespace(after) || PdfLexer::isDelimiter(after)) break;
          prev = -1;
          continue;
        }
        prev = c;
      }
    }

    stack.clear();
  }

  flushLine();
  return true;
}

bool PdfTextExtractor::run(PdfDocument& doc, const std::string& outPath, const Options& options,
                           const ProgressFn progress, void* ctx) {
  opts = options;
  scratchDir = doc.scratchPath();
  charCount = 0;
  emptyPageCount = 0;
  paragraph.clear();
  line.clear();
  haveLine = false;

  PdfSink sink;
  if (!sink.open(TAG, outPath)) return false;
  out = &sink;

  const size_t total = doc.pageCount();
  for (size_t i = 0; i < total; ++i) {
    if (progress && !progress(ctx, i, total)) break;

    PdfObject page, resources;
    if (!doc.getPage(i, page, resources)) continue;

    // The media box gives the header/footer band its coordinates.
    PdfObject box = doc.lookup(page, "MediaBox");
    if (box.type != PdfType::Array || box.items.size() < 4) {
      // US Letter, the near-universal default when the key is absent.
      pageBottom = 0;
      pageTop = 792;
    } else {
      const double y0 = doc.resolve(box.items[1]).asNumber(0);
      const double y1 = doc.resolve(box.items[3]).asNumber(792);
      pageBottom = std::min(y0, y1);
      pageTop = std::max(y0, y1);
    }
    const double height = pageTop - pageBottom;
    const double band = height * MARGIN_BAND;
    const double topCut = pageTop - band;
    const double bottomCut = pageBottom + band;
    pageTop = topCut;
    pageBottom = bottomCut;

    // Reset per-page interpreter state; the paragraph deliberately carries over.
    ts = TextState{};
    paragraphLeading = 0;
    haveLastLine = false;
    pageJustStarted = true;
    pageProducedText = false;

    // /Contents is one stream or an array that concatenates into one.
    PdfObject contents = doc.lookup(page, "Contents");
    std::vector<PdfObject> streams;
    if (contents.type == PdfType::Stream) {
      streams.push_back(contents);
    } else if (contents.type == PdfType::Array) {
      streams.reserve(contents.items.size());
      for (const auto& item : contents.items) {
        PdfObject s = doc.resolve(item);
        if (s.type == PdfType::Stream) streams.push_back(std::move(s));
      }
    }

    for (const auto& stream : streams) {
      const std::string contentPath = scratchDir + "/content.bin";
      if (!doc.decodeStream(stream, contentPath)) continue;
      PdfFileSource contentSrc;
      if (contentSrc.open(TAG, contentPath)) {
        runContent(doc, contentSrc, resources, Matrix{}, 0);
        contentSrc.close();
      }
      Storage.remove(contentPath.c_str());
    }

    flushLine();
    if (!pageProducedText) ++emptyPageCount;

    // Keep the watchdog fed and let the UI task render progress.
    vTaskDelay(1);
  }

  flushParagraph();
  out = nullptr;
  const bool ok = sink.finish();
  LOG_INF(TAG, "Extracted %u chars from %u pages (%u empty)", static_cast<unsigned>(charCount),
          static_cast<unsigned>(total), static_cast<unsigned>(emptyPageCount));
  return ok;
}
