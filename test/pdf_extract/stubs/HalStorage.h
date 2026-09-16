#pragma once

// Host stand-in for the SD card: HalFile over stdio, rooted in the test's
// temporary directory. Only the surface lib/Pdf touches is provided.

#include <sys/stat.h>
#include <unistd.h>

#include <cstdint>
#include <cstdio>
#include <string>

class HalFile {
 public:
  HalFile() = default;
  ~HalFile() { close(); }

  HalFile(const HalFile&) = delete;
  HalFile& operator=(const HalFile&) = delete;

  bool openFile(const std::string& path, const char* mode) {
    close();
    file_ = std::fopen(path.c_str(), mode);
    return file_ != nullptr;
  }

  size_t size() {
    if (!file_) return 0;
    const long here = std::ftell(file_);
    std::fseek(file_, 0, SEEK_END);
    const long end = std::ftell(file_);
    std::fseek(file_, here, SEEK_SET);
    return end > 0 ? static_cast<size_t>(end) : 0;
  }

  uint32_t modificationTime() { return 1; }

  bool seek(const size_t pos) { return file_ && std::fseek(file_, static_cast<long>(pos), SEEK_SET) == 0; }

  int available() {
    if (!file_) return 0;
    const long here = std::ftell(file_);
    return static_cast<int>(size()) - static_cast<int>(here);
  }

  int read(void* buffer, const size_t count) {
    if (!file_) return -1;
    return static_cast<int>(std::fread(buffer, 1, count, file_));
  }

  size_t write(const void* buffer, const size_t count) { return file_ ? std::fwrite(buffer, 1, count, file_) : 0; }

  bool close() {
    if (!file_) return false;
    const bool ok = std::fclose(file_) == 0;
    file_ = nullptr;
    return ok;
  }

  bool isOpen() const { return file_ != nullptr; }
  explicit operator bool() const { return file_ != nullptr; }

 private:
  std::FILE* file_ = nullptr;
};

class HalStorage {
 public:
  static HalStorage& getInstance() {
    static HalStorage instance;
    return instance;
  }

  bool openFileForRead(const char*, const std::string& path, HalFile& file) { return file.openFile(path, "rb"); }
  bool openFileForRead(const char* tag, const char* path, HalFile& file) {
    return openFileForRead(tag, std::string(path), file);
  }
  bool openFileForWrite(const char*, const std::string& path, HalFile& file) { return file.openFile(path, "wb"); }
  bool openFileForWrite(const char* tag, const char* path, HalFile& file) {
    return openFileForWrite(tag, std::string(path), file);
  }

  bool exists(const char* path) const { return ::access(path, F_OK) == 0; }
  bool remove(const char* path) { return ::remove(path) == 0; }
  bool rename(const char* from, const char* to) { return ::rename(from, to) == 0; }
  bool mkdir(const char* path, bool = true) { return ::mkdir(path, 0755) == 0 || exists(path); }
  bool removeDir(const char* path) { return ::rmdir(path) == 0; }
};

#define Storage HalStorage::getInstance()
