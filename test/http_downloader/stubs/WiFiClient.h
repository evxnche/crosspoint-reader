#pragma once
#include <Client.h>

#include <algorithm>
#include <cstring>

namespace httpTest {
inline std::string response;
inline size_t bodyRead = 0;
inline unsigned connections = 0;
}  // namespace httpTest

class WiFiClient : public Client {
 public:
  int connect(IPAddress, uint16_t) override { return connect("host", 80); }
  int connect(const char*, uint16_t) override {
    ++httpTest::connections;
    open = true;
    position = 0;
    return 1;
  }
  void setConnectionTimeout(uint32_t) {}
  size_t write(uint8_t) override { return 1; }
  size_t write(const uint8_t*, size_t len) override { return len; }
  int available() override { return static_cast<int>(httpTest::response.size() - position); }
  int read() override { return available() ? static_cast<unsigned char>(httpTest::response[position++]) : -1; }
  int read(uint8_t* out, size_t len) override {
    len = std::min(len, static_cast<size_t>(available()));
    std::memcpy(out, httpTest::response.data() + position, len);
    position += len;
    httpTest::bodyRead += len;
    return static_cast<int>(len);
  }
  int peek() override { return available() ? static_cast<unsigned char>(httpTest::response[position]) : -1; }
  void flush() override {}
  void stop() override { open = false; }
  uint8_t connected() override { return open; }
  operator bool() override { return open; }

 private:
  bool open = false;
  size_t position = 0;
};
