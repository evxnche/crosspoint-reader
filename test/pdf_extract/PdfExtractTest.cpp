// Host coverage for the PDF reading path: object parsing, filters, and the
// geometry-to-prose reflow that makes a PDF readable on a small screen.
//
// Each case builds a real PDF in a temp directory, runs the same code the
// firmware runs, and asserts on the extracted text.

#include <gtest/gtest.h>
#include <unistd.h>

#include <cstdio>
#include <cstdlib>
#include <fstream>
#include <sstream>
#include <string>
#include <vector>

#include "Pdf/PdfDocument.h"
#include "Pdf/PdfTextExtractor.h"

namespace {

// Assembles a classic single-section PDF: a catalog, a page tree with one
// page, one content stream, and one font.
class PdfBuilder {
 public:
  // Returns the object number assigned to `body`.
  int addObject(const std::string& body) {
    objects.push_back(body);
    return static_cast<int>(objects.size());
  }

  std::string build(const int rootObj) const {
    std::string out = "%PDF-1.4\n";
    std::vector<size_t> offsets;
    offsets.reserve(objects.size());

    for (size_t i = 0; i < objects.size(); ++i) {
      offsets.push_back(out.size());
      out += std::to_string(i + 1) + " 0 obj\n" + objects[i] + "\nendobj\n";
    }

    const size_t xrefStart = out.size();
    out += "xref\n0 " + std::to_string(objects.size() + 1) + "\n";
    out += "0000000000 65535 f \n";
    for (const size_t offset : offsets) {
      char entry[24];
      snprintf(entry, sizeof(entry), "%010zu 00000 n \n", offset);
      out += entry;
    }
    out += "trailer\n<< /Size " + std::to_string(objects.size() + 1) + " /Root " + std::to_string(rootObj) +
           " 0 R >>\nstartxref\n" + std::to_string(xrefStart) + "\n%%EOF\n";
    return out;
  }

 private:
  std::vector<std::string> objects;
};

std::string streamObject(const std::string& dictExtras, const std::string& payload) {
  return "<< /Length " + std::to_string(payload.size()) + dictExtras + " >>\nstream\n" + payload + "\nendstream";
}

// A deflate stream of stored (uncompressed) blocks, wrapped in zlib. Valid
// deflate that the real inflater must accept, without needing a compressor.
std::string zlibStored(const std::string& data) {
  std::string out;
  out.push_back(static_cast<char>(0x78));  // CM=8, CINFO=7
  out.push_back(static_cast<char>(0x01));  // FCHECK so the header is a multiple of 31
  out.push_back(static_cast<char>(0x01));  // BFINAL=1, BTYPE=00 (stored)
  const auto len = static_cast<uint16_t>(data.size());
  out.push_back(static_cast<char>(len & 0xFF));
  out.push_back(static_cast<char>(len >> 8));
  out.push_back(static_cast<char>(~len & 0xFF));
  out.push_back(static_cast<char>((~len >> 8) & 0xFF));
  out += data;

  uint32_t s1 = 1, s2 = 0;
  for (const unsigned char c : data) {
    s1 = (s1 + c) % 65521;
    s2 = (s2 + s1) % 65521;
  }
  const uint32_t adler = (s2 << 16) | s1;
  out.push_back(static_cast<char>(adler >> 24));
  out.push_back(static_cast<char>(adler >> 16));
  out.push_back(static_cast<char>(adler >> 8));
  out.push_back(static_cast<char>(adler));
  return out;
}

class PdfExtractTest : public ::testing::Test {
 protected:
  void SetUp() override {
    char pattern[] = "/tmp/cpr_pdf_XXXXXX";
    ASSERT_NE(mkdtemp(pattern), nullptr);
    dir = pattern;
  }

  void TearDown() override {
    // Remove whatever the run left behind, then the directory itself.
    for (const char* name :
         {"doc.pdf", "out.txt", "content.bin", "objstm.bin", "xref.bin", "tounicode.bin", "form0.bin", "form1.bin"}) {
      ::remove((dir + "/" + name).c_str());
    }
    ::rmdir(dir.c_str());
  }

  std::string writePdf(const std::string& bytes) const {
    const std::string path = dir + "/doc.pdf";
    std::ofstream out(path, std::ios::binary);
    out.write(bytes.data(), static_cast<std::streamsize>(bytes.size()));
    out.close();
    return path;
  }

  // Runs the full pipeline and returns the extracted text.
  std::string extract(const std::string& pdfBytes) {
    const std::string path = writePdf(pdfBytes);
    PdfDocument doc;
    EXPECT_TRUE(doc.open(path, dir));
    if (!doc.isOpen()) return {};

    PdfTextExtractor extractor;
    const std::string outPath = dir + "/out.txt";
    const PdfTextExtractor::Options options;
    EXPECT_TRUE(extractor.run(doc, outPath, options, nullptr, nullptr));
    doc.close();

    std::ifstream in(outPath, std::ios::binary);
    std::ostringstream buffer;
    buffer << in.rdbuf();
    return buffer.str();
  }

  // A one-page document whose content stream is `content`, optionally filtered.
  static std::string onePage(const std::string& content, const std::string& streamExtras = "",
                             const std::string& fontDict = "") {
    PdfBuilder builder;
    const int catalog = builder.addObject("<< /Type /Catalog /Pages 2 0 R >>");
    builder.addObject("<< /Type /Pages /Kids [3 0 R] /Count 1 >>");
    builder.addObject(
        "<< /Type /Page /Parent 2 0 R /MediaBox [0 0 612 792] "
        "/Resources << /Font << /F1 5 0 R >> >> /Contents 4 0 R >>");
    builder.addObject(streamObject(streamExtras, content));
    builder.addObject(fontDict.empty()
                          ? "<< /Type /Font /Subtype /Type1 /BaseFont /Helvetica /Encoding /WinAnsiEncoding >>"
                          : fontDict);
    return builder.build(catalog);
  }

  std::string dir;
};

TEST_F(PdfExtractTest, RejectsNonPdf) {
  const std::string path = writePdf("this is not a PDF at all");
  PdfDocument doc;
  EXPECT_FALSE(doc.open(path, dir));
}

// The core promise: lines that share a paragraph come back as one long line, so
// the reader can wrap them to the user's font instead of showing page layout.
TEST_F(PdfExtractTest, ReflowsLinesIntoParagraphs) {
  const std::string content =
      "BT /F1 12 Tf\n"
      "1 0 0 1 72 600 Tm (The quick brown fox jumps over the lazy) Tj\n"
      "1 0 0 1 72 586 Tm (dog and keeps going) Tj\n"
      "1 0 0 1 72 572 Tm (until morning.) Tj\n"
      "1 0 0 1 72 530 Tm (A second paragraph starts here.) Tj\n"
      "ET\n";

  const std::string text = extract(onePage(content));
  EXPECT_EQ(text,
            "The quick brown fox jumps over the lazy dog and keeps going until morning.\n\n"
            "A second paragraph starts here.\n\n");
}

// A word broken across a line break is rejoined, not left with its hyphen.
TEST_F(PdfExtractTest, RejoinsHyphenatedWordAcrossLines) {
  const std::string content =
      "BT /F1 12 Tf\n"
      "1 0 0 1 72 600 Tm (The engine keeps run-) Tj\n"
      "1 0 0 1 72 586 Tm (ning until morning.) Tj\n"
      "ET\n";

  EXPECT_EQ(extract(onePage(content)), "The engine keeps running until morning.\n\n");
}

// Running heads and folios sit in the page margins and would otherwise land in
// the middle of a sentence once the text reflows.
TEST_F(PdfExtractTest, DropsHeadersAndFooters) {
  const std::string content =
      "BT /F1 12 Tf\n"
      "1 0 0 1 72 770 Tm (CHAPTER ONE) Tj\n"
      "1 0 0 1 72 600 Tm (Body text lives here.) Tj\n"
      "1 0 0 1 300 20 Tm (42) Tj\n"
      "ET\n";

  EXPECT_EQ(extract(onePage(content)), "Body text lives here.\n\n");
}

// TJ's numeric elements move the pen; a large negative one is how most
// producers write a space.
TEST_F(PdfExtractTest, TreatsLargeTjAdjustmentAsSpace) {
  const std::string content =
      "BT /F1 12 Tf\n"
      "1 0 0 1 72 600 Tm [(Hello) -600 (world)] TJ\n"
      "ET\n";

  EXPECT_EQ(extract(onePage(content)), "Hello world\n\n");
}

// Nearly every real PDF compresses its content streams.
TEST_F(PdfExtractTest, DecodesFlateContentStream) {
  const std::string content =
      "BT /F1 12 Tf\n"
      "1 0 0 1 72 600 Tm (Compressed content decodes.) Tj\n"
      "ET\n";

  EXPECT_EQ(extract(onePage(zlibStored(content), " /Filter /FlateDecode")), "Compressed content decodes.\n\n");
}

// /Differences remaps individual codes; without it the text comes out as the
// wrong characters entirely.
TEST_F(PdfExtractTest, AppliesEncodingDifferences) {
  const std::string fontDict =
      "<< /Type /Font /Subtype /Type1 /BaseFont /Helvetica "
      "/Encoding << /BaseEncoding /WinAnsiEncoding /Differences [65 /eacute 66 /emdash] >> >>";
  const std::string content =
      "BT /F1 12 Tf\n"
      "1 0 0 1 72 600 Tm (AB) Tj\n"
      "ET\n";

  EXPECT_EQ(extract(onePage(content, "", fontDict)), "\xC3\xA9\xE2\x80\x94\n\n");
}

// Text inside a form XObject is ordinary body text in many generators; skipping
// the recursion silently loses whole pages.
TEST_F(PdfExtractTest, FollowsFormXObjects) {
  const std::string formContent =
      "BT /F1 12 Tf\n"
      "1 0 0 1 72 600 Tm (Text inside a form.) Tj\n"
      "ET\n";

  PdfBuilder builder;
  const int catalog = builder.addObject("<< /Type /Catalog /Pages 2 0 R >>");
  builder.addObject("<< /Type /Pages /Kids [3 0 R] /Count 1 >>");
  builder.addObject(
      "<< /Type /Page /Parent 2 0 R /MediaBox [0 0 612 792] "
      "/Resources << /XObject << /Fm0 5 0 R >> /Font << /F1 6 0 R >> >> /Contents 4 0 R >>");
  builder.addObject(streamObject("", "/Fm0 Do\n"));
  builder.addObject(streamObject(" /Type /XObject /Subtype /Form /BBox [0 0 612 792]", formContent));
  builder.addObject("<< /Type /Font /Subtype /Type1 /BaseFont /Helvetica /Encoding /WinAnsiEncoding >>");

  EXPECT_EQ(extract(builder.build(catalog)), "Text inside a form.\n\n");
}

// Inline image samples are raw bytes in the middle of the content stream. Left
// unskipped they tokenise into garbage operators and corrupt the page.
TEST_F(PdfExtractTest, SkipsInlineImageData) {
  std::string content =
      "BT /F1 12 Tf\n"
      "1 0 0 1 72 600 Tm (Before image.) Tj\n"
      "ET\n"
      "BI /W 2 /H 2 /CS /G /BPC 8 ID ";
  // Bytes that would otherwise lex as operators and text.
  content += "(Tj\xFF\x00\x10";
  content +=
      " EI\n"
      "BT /F1 12 Tf\n"
      "1 0 0 1 72 560 Tm (After image.) Tj\n"
      "ET\n";

  const std::string text = extract(onePage(content));
  EXPECT_NE(text.find("Before image."), std::string::npos);
  EXPECT_NE(text.find("After image."), std::string::npos);
  EXPECT_EQ(text.find("Tj\xFF"), std::string::npos);
}

// A document whose pages carry no text layer is a scan; the reader needs to be
// able to tell, so it can say so instead of opening a blank book.
TEST_F(PdfExtractTest, ReportsPagesWithNoTextLayer) {
  const std::string path = writePdf(onePage("0 0 0 rg\n10 10 100 100 re f\n"));
  PdfDocument doc;
  ASSERT_TRUE(doc.open(path, dir));

  PdfTextExtractor extractor;
  const PdfTextExtractor::Options options;
  ASSERT_TRUE(extractor.run(doc, dir + "/out.txt", options, nullptr, nullptr));
  EXPECT_EQ(extractor.emptyPages(), doc.pageCount());
  doc.close();
}

// Progress drives the on-screen bar during a long extraction, and returning
// false is how the UI aborts one.
TEST_F(PdfExtractTest, ReportsProgressAndHonoursAbort) {
  const std::string path = writePdf(onePage("BT /F1 12 Tf 1 0 0 1 72 600 Tm (Page.) Tj ET\n"));
  PdfDocument doc;
  ASSERT_TRUE(doc.open(path, dir));

  struct Counter {
    int calls = 0;
  } counter;
  auto progress = [](void* ctx, size_t, size_t total) {
    EXPECT_EQ(total, 1u);
    static_cast<Counter*>(ctx)->calls++;
    return false;  // abort immediately
  };

  PdfTextExtractor extractor;
  const PdfTextExtractor::Options options;
  ASSERT_TRUE(extractor.run(doc, dir + "/out.txt", options, progress, &counter));
  EXPECT_EQ(counter.calls, 1);
  EXPECT_EQ(extractor.charactersWritten(), 0u);
  doc.close();
}

// A file whose startxref points nowhere still has intact objects; the scan
// fallback is what keeps damaged downloads readable.
TEST_F(PdfExtractTest, RecoversFromBrokenXref) {
  std::string bytes = onePage("BT /F1 12 Tf 1 0 0 1 72 600 Tm (Recovered text.) Tj ET\n");
  const size_t at = bytes.rfind("startxref\n");
  ASSERT_NE(at, std::string::npos);
  const size_t numberStart = at + std::string("startxref\n").size();
  const size_t numberEnd = bytes.find('\n', numberStart);
  ASSERT_NE(numberEnd, std::string::npos);
  bytes.replace(numberStart, numberEnd - numberStart, "999999");

  EXPECT_EQ(extract(bytes), "Recovered text.\n\n");
}

}  // namespace
