#include <HttpDownloader.h>
#include <SecureClient.h>
#include <gtest/gtest.h>
#include <unistd.h>

// Keep the real SDK HTTP parser and wait loops; replace only the TCP/TLS socket.
namespace freeink {
SecureClient::~SecureClient() = default;
void SecureClient::setCACert(const char*) {}
void SecureClient::setInsecure() {}
int SecureClient::connect(IPAddress ip, uint16_t port) { return _transport.connect(ip, port); }
int SecureClient::connect(const char* host, uint16_t port) { return _transport.connect(host, port); }
size_t SecureClient::write(uint8_t value) { return _transport.write(value); }
size_t SecureClient::write(const uint8_t* data, size_t len) { return _transport.write(data, len); }
int SecureClient::available() { return _transport.available(); }
int SecureClient::read() { return _transport.read(); }
int SecureClient::read(uint8_t* data, size_t len) { return _transport.read(data, len); }
int SecureClient::peek() { return _transport.peek(); }
void SecureClient::flush() {}
void SecureClient::stop() { _transport.stop(); }
uint8_t SecureClient::connected() { return _transport.connected(); }
}  // namespace freeink

class HttpDownloaderTest : public ::testing::Test {
 protected:
  void SetUp() override {
    httpTest::response.clear();
    httpTest::bodyRead = 0;
    httpTest::connections = 0;
    httpTestClock = 0;
  }
  static void respond(const std::string& body) {
    httpTest::response = "HTTP/1.1 200 OK\r\nContent-Length: " + std::to_string(body.size()) + "\r\n\r\n" + body;
  }
};

TEST_F(HttpDownloaderTest, CancelsWhileWaitingForResponseHeaders) {
  for (const char* url : {"http://host/list", "https://host/plan"}) {
    unsigned polls = 0;
    std::string body = "old content";
    EXPECT_FALSE(HttpDownloader::fetchUrl(url, body, "", "", [&polls] { return ++polls >= 3; }));
    EXPECT_EQ(polls, 3u);
    EXPECT_LT(httpTestClock, 1000u);
    EXPECT_TRUE(body.empty());
  }
}

TEST_F(HttpDownloaderTest, ClearsPartialBodyAfterCancellation) {
  respond(std::string(4096, 'x'));
  std::string body;
  EXPECT_FALSE(HttpDownloader::fetchUrl("https://host/plan", body, "", "", [] { return httpTest::bodyRead > 0; }));
  EXPECT_GT(httpTest::bodyRead, 0u);
  EXPECT_LT(httpTest::bodyRead, 4096u);
  EXPECT_TRUE(body.empty());
}

TEST_F(HttpDownloaderTest, CancelBeforeRequestAvoidsConnecting) {
  std::string body;
  EXPECT_FALSE(HttpDownloader::fetchUrl("https://host/list", body, "", "", [] { return true; }));
  EXPECT_EQ(httpTest::connections, 0u);
}

TEST_F(HttpDownloaderTest, NormalFetchStillReturnsTheCompleteBody) {
  respond("{\"articles\":[]}");
  std::string body;
  EXPECT_TRUE(HttpDownloader::fetchUrl("https://host/list", body));
  EXPECT_EQ(body, "{\"articles\":[]}");
}

TEST_F(HttpDownloaderTest, CancelledDownloadRemovesItsPartialFile) {
  char name[] = "/tmp/cpr_http_XXXXXX";
  const int fd = mkstemp(name);
  ASSERT_GE(fd, 0);
  close(fd);
  const auto result =
      HttpDownloader::downloadToFile("https://host/book", name, nullptr, nullptr, "", "", false, [] { return true; });
  EXPECT_EQ(result, HttpDownloader::ABORTED);
  EXPECT_FALSE(Storage.exists(name));
  ::remove(name);
}
