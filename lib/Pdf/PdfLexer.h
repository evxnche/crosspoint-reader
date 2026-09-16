#pragma once

#include <cstddef>
#include <string>

#include "PdfSource.h"
#include "PdfTypes.h"

// Tokenizer and object parser for PDF syntax (ISO 32000-1 clause 7.2/7.3).
//
// The same grammar covers file bodies, object streams, and content streams, so
// one lexer drives all three; the caller decides what to do with the keywords
// that come back.
class PdfLexer {
 public:
  explicit PdfLexer(PdfSource& src) : src(src) {}

  static bool isWhitespace(int c);
  static bool isDelimiter(int c);

  // Advance past whitespace and % comments. Returns false at end of source.
  bool skipWhitespace();

  // Next raw token. Delimiters come back as themselves ("<<", "[", "/Name"),
  // everything else as a run of regular characters. Empty at end of source.
  std::string nextToken();

  // Parse one object at the current position.
  //
  // Number-number-R sequences collapse into Ref, which needs two tokens of
  // lookahead; the lexer restores the position when the sequence turns out to
  // be two plain integers. Arrays and dictionaries nest up to `maxDepth`;
  // beyond that the value is returned as Null rather than recursing, because a
  // hostile or corrupt file must not be able to exhaust a 4KB FreeRTOS stack.
  PdfObject parseObject(int depth = 0);

  // Parse the body of a dictionary, with "<<" already consumed. When the dict
  // is followed by the `stream` keyword the result is a Stream whose
  // streamStart/streamLen frame the raw payload.
  PdfObject parseDictOrStream(int depth = 0);

  static constexpr int MAX_DEPTH = 24;

 private:
  PdfObject parseLiteralString();
  PdfObject parseHexString();
  PdfObject parseName();

  PdfSource& src;
};
