#include "PdfLexer.h"

#include <cstdlib>
#include <cstring>

namespace {

int hexVal(const int c) {
  if (c >= '0' && c <= '9') return c - '0';
  if (c >= 'a' && c <= 'f') return c - 'a' + 10;
  if (c >= 'A' && c <= 'F') return c - 'A' + 10;
  return -1;
}

bool tokenIsNumber(const std::string& t) {
  if (t.empty()) return false;
  bool digit = false;
  for (size_t i = 0; i < t.size(); ++i) {
    const char c = t[i];
    if (c >= '0' && c <= '9') {
      digit = true;
    } else if (c == '+' || c == '-') {
      if (i != 0) return false;
    } else if (c == '.') {
      // A second '.' is malformed, but strtod stopping early is harmless here.
    } else {
      return false;
    }
  }
  return digit;
}

bool tokenIsInteger(const std::string& t) {
  return tokenIsNumber(t) && t.find('.') == std::string::npos && t.find('-') == std::string::npos &&
         t.find('+') == std::string::npos;
}

}  // namespace

bool PdfLexer::isWhitespace(const int c) {
  return c == 0x20 || c == 0x0A || c == 0x0D || c == 0x09 || c == 0x0C || c == 0x00;
}

bool PdfLexer::isDelimiter(const int c) {
  return c == '(' || c == ')' || c == '<' || c == '>' || c == '[' || c == ']' || c == '{' || c == '}' || c == '/' ||
         c == '%';
}

bool PdfLexer::skipWhitespace() {
  while (true) {
    const int c = src.get();
    if (c < 0) return false;
    if (isWhitespace(c)) continue;
    if (c == '%') {
      // Comment runs to the end of the line.
      int d = src.get();
      while (d >= 0 && d != 0x0A && d != 0x0D) d = src.get();
      continue;
    }
    src.seek(src.tell() - 1);
    return true;
  }
}

std::string PdfLexer::nextToken() {
  if (!skipWhitespace()) return {};

  const int c = src.get();
  if (c < 0) return {};

  if (c == '<') {
    const int d = src.get();
    if (d == '<') return "<<";
    if (d >= 0) src.seek(src.tell() - 1);
    return "<";
  }
  if (c == '>') {
    const int d = src.get();
    if (d == '>') return ">>";
    if (d >= 0) src.seek(src.tell() - 1);
    return ">";
  }
  if (c == '/') {
    std::string t = "/";
    while (true) {
      const int d = src.get();
      if (d < 0) break;
      if (isWhitespace(d) || isDelimiter(d)) {
        src.seek(src.tell() - 1);
        break;
      }
      t.push_back(static_cast<char>(d));
    }
    return t;
  }
  if (isDelimiter(c)) return std::string(1, static_cast<char>(c));

  std::string t(1, static_cast<char>(c));
  while (true) {
    const int d = src.get();
    if (d < 0) break;
    if (isWhitespace(d) || isDelimiter(d)) {
      src.seek(src.tell() - 1);
      break;
    }
    t.push_back(static_cast<char>(d));
  }
  return t;
}

PdfObject PdfLexer::parseName() {
  // '/' already consumed.
  PdfObject o;
  o.type = PdfType::Name;
  while (true) {
    const int c = src.get();
    if (c < 0) break;
    if (isWhitespace(c) || isDelimiter(c)) {
      src.seek(src.tell() - 1);
      break;
    }
    if (c == '#') {
      const int h1 = hexVal(src.get());
      const int h2 = hexVal(src.get());
      if (h1 >= 0 && h2 >= 0) {
        o.text.push_back(static_cast<char>(h1 * 16 + h2));
        continue;
      }
      o.text.push_back('#');
      continue;
    }
    o.text.push_back(static_cast<char>(c));
  }
  return o;
}

PdfObject PdfLexer::parseLiteralString() {
  // '(' already consumed.
  PdfObject o;
  o.type = PdfType::String;
  int nesting = 1;
  while (true) {
    int c = src.get();
    if (c < 0) break;
    if (c == '\\') {
      c = src.get();
      if (c < 0) break;
      switch (c) {
        case 'n':
          o.text.push_back('\n');
          break;
        case 'r':
          o.text.push_back('\r');
          break;
        case 't':
          o.text.push_back('\t');
          break;
        case 'b':
          o.text.push_back('\b');
          break;
        case 'f':
          o.text.push_back('\f');
          break;
        case '(':
          o.text.push_back('(');
          break;
        case ')':
          o.text.push_back(')');
          break;
        case '\\':
          o.text.push_back('\\');
          break;
        case 0x0D: {
          // Line continuation; swallow an following LF.
          const int d = src.get();
          if (d >= 0 && d != 0x0A) src.seek(src.tell() - 1);
          break;
        }
        case 0x0A:
          break;  // line continuation
        default:
          if (c >= '0' && c <= '7') {
            int val = c - '0';
            for (int i = 0; i < 2; ++i) {
              const int d = src.get();
              if (d < '0' || d > '7') {
                if (d >= 0) src.seek(src.tell() - 1);
                break;
              }
              val = val * 8 + (d - '0');
            }
            o.text.push_back(static_cast<char>(val & 0xFF));
          } else {
            o.text.push_back(static_cast<char>(c));
          }
          break;
      }
      continue;
    }
    if (c == '(') {
      ++nesting;
    } else if (c == ')') {
      if (--nesting == 0) break;
    }
    o.text.push_back(static_cast<char>(c));
  }
  return o;
}

PdfObject PdfLexer::parseHexString() {
  // '<' already consumed and known not to be '<<'.
  PdfObject o;
  o.type = PdfType::String;
  int hi = -1;
  while (true) {
    const int c = src.get();
    if (c < 0 || c == '>') break;
    const int v = hexVal(c);
    if (v < 0) continue;  // whitespace and junk are skipped per spec
    if (hi < 0) {
      hi = v;
    } else {
      o.text.push_back(static_cast<char>(hi * 16 + v));
      hi = -1;
    }
  }
  // An odd trailing digit is padded with 0.
  if (hi >= 0) o.text.push_back(static_cast<char>(hi * 16));
  return o;
}

PdfObject PdfLexer::parseDictOrStream(const int depth) {
  PdfObject o;
  o.type = PdfType::Dict;

  while (true) {
    if (!skipWhitespace()) return o;
    const size_t mark = src.tell();
    const int c = src.get();
    if (c < 0) return o;
    if (c == '>') {
      const int d = src.get();
      if (d == '>') break;
      continue;  // stray '>', keep going rather than abandoning the dict
    }
    if (c != '/') {
      // Malformed key. Skip one object so a single bad entry cannot wedge us.
      src.seek(mark);
      (void)parseObject(depth + 1);
      continue;
    }
    const PdfObject key = parseName();
    PdfObject value = parseObject(depth + 1);
    o.dict.emplace_back(key.text, std::move(value));
  }

  // A dictionary followed by `stream` introduces a stream object.
  const size_t afterDict = src.tell();
  if (!skipWhitespace()) return o;
  const size_t kwPos = src.tell();
  const std::string kw = nextToken();
  if (kw != "stream") {
    src.seek(afterDict);
    return o;
  }
  (void)kwPos;

  // The keyword is followed by CRLF or LF (never a bare CR).
  int c = src.get();
  if (c == 0x0D) c = src.get();
  if (c != 0x0A && c >= 0) src.seek(src.tell() - 1);

  o.type = PdfType::Stream;
  o.streamStart = src.tell();
  o.streamLen = 0;

  // A direct /Length lets us skip the payload now. An indirect one is resolved
  // by PdfDocument, which rescans for `endstream` if it has to.
  if (const PdfObject* len = o.find("Length"); len && len->type == PdfType::Int) {
    o.streamLen = static_cast<size_t>(len->asInt());
    src.seek(o.streamStart + o.streamLen);
  }
  return o;
}

PdfObject PdfLexer::parseObject(const int depth) {
  PdfObject o;
  if (depth > MAX_DEPTH) {
    // Refuse to recurse further; consume one token so the caller still advances.
    (void)nextToken();
    return o;
  }
  if (!skipWhitespace()) return o;

  const size_t start = src.tell();
  const int c = src.get();
  if (c < 0) return o;

  if (c == '/') return parseName();
  if (c == '(') return parseLiteralString();

  if (c == '<') {
    const int d = src.get();
    if (d == '<') return parseDictOrStream(depth);
    if (d >= 0) src.seek(src.tell() - 1);
    return parseHexString();
  }

  if (c == '[') {
    o.type = PdfType::Array;
    while (true) {
      if (!skipWhitespace()) break;
      const size_t mark = src.tell();
      const int e = src.get();
      if (e < 0) break;
      if (e == ']') break;
      src.seek(mark);
      PdfObject item = parseObject(depth + 1);
      // A parse that made no progress would spin forever on malformed input.
      if (src.tell() == mark) {
        src.seek(mark + 1);
        continue;
      }
      o.items.push_back(std::move(item));
    }
    return o;
  }

  if (c == ']' || c == '>' || c == '}' || c == ')') {
    o.type = PdfType::Keyword;
    o.text = std::string(1, static_cast<char>(c));
    return o;
  }

  src.seek(start);
  const std::string tok = nextToken();
  if (tok.empty()) return o;

  if (tokenIsNumber(tok)) {
    // Look ahead for "<gen> R": only an integer can begin a reference.
    if (tokenIsInteger(tok)) {
      const size_t afterFirst = src.tell();
      const std::string t2 = nextToken();
      if (tokenIsInteger(t2)) {
        const std::string t3 = nextToken();
        if (t3 == "R") {
          o.type = PdfType::Ref;
          o.refNum = static_cast<uint32_t>(std::strtoul(tok.c_str(), nullptr, 10));
          o.refGen = static_cast<uint16_t>(std::strtoul(t2.c_str(), nullptr, 10));
          return o;
        }
      }
      src.seek(afterFirst);
      o.type = PdfType::Int;
      o.number = std::strtod(tok.c_str(), nullptr);
      return o;
    }
    o.type = PdfType::Real;
    o.number = std::strtod(tok.c_str(), nullptr);
    return o;
  }

  if (tok == "true" || tok == "false") {
    o.type = PdfType::Bool;
    o.boolean = tok == "true";
    return o;
  }
  if (tok == "null") {
    o.type = PdfType::Null;
    return o;
  }

  o.type = PdfType::Keyword;
  o.text = tok;
  return o;
}
