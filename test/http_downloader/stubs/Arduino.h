#pragma once
#include <cstdint>
#include <string>

using String = std::string;
struct IPAddress {};
inline unsigned long httpTestClock = 0;
inline unsigned long millis() { return ++httpTestClock; }
inline void delay(unsigned long ms) { httpTestClock += ms; }
class Stream {
 public:
  virtual ~Stream() = default;
  virtual size_t write(const uint8_t*, size_t) = 0;
};
