#pragma once

#include <cstddef>
#include <cstdint>
#include <string>
#include <utility>
#include <vector>

// Minimal PDF object model.
//
// Only the shapes the text extractor needs are represented. Streams carry the
// byte range of their raw (still encoded) payload inside the source, never the
// payload itself: content streams routinely decompress to hundreds of KB and
// must not be held whole on an ESP32.
enum class PdfType : uint8_t {
  Null,
  Bool,
  Int,
  Real,
  String,  // literal (..) or hex <..>, already unescaped into `text`
  Name,    // /Name, stored in `text` without the slash
  Array,
  Dict,
  Ref,      // "12 0 R"
  Stream,   // dict + raw payload range
  Keyword,  // bare token: obj, endobj, stream, an operator, ...
};

struct PdfObject {
  PdfType type = PdfType::Null;

  bool boolean = false;
  double number = 0;
  std::string text;

  std::vector<PdfObject> items;                         // Array
  std::vector<std::pair<std::string, PdfObject>> dict;  // Dict / Stream dict

  uint32_t refNum = 0;
  uint16_t refGen = 0;

  // Stream only: byte range of the encoded payload within the owning source.
  size_t streamStart = 0;
  size_t streamLen = 0;

  [[nodiscard]] bool isNumber() const { return type == PdfType::Int || type == PdfType::Real; }
  [[nodiscard]] bool isDictLike() const { return type == PdfType::Dict || type == PdfType::Stream; }
  [[nodiscard]] double asNumber(double fallback = 0) const { return isNumber() ? number : fallback; }
  [[nodiscard]] long asInt(long fallback = 0) const { return isNumber() ? static_cast<long>(number) : fallback; }

  // Dictionary lookup. Returns nullptr when absent. Linear scan: PDF dicts in
  // practice hold a handful of keys, and a map per object would cost more.
  [[nodiscard]] const PdfObject* find(const char* key) const {
    if (!isDictLike()) return nullptr;
    for (const auto& kv : dict) {
      if (kv.first == key) return &kv.second;
    }
    return nullptr;
  }

  [[nodiscard]] bool isName(const char* name) const { return type == PdfType::Name && text == name; }
};
