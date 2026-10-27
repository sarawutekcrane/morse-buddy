#pragma once
// HOST TEST (not production). LittleFS over a real host directory, for
// running the ACTUAL src/core/storage_messages.cpp: files and directories
// are real (under LittleFS.root), every operation is counted in g_fs, and
// opens can be failed on purpose (failOpen) to model unreadable directories
// or records. Directory listings are returned in name order (reverse order
// with reverseListing).
#include <stddef.h>
#include <stdint.h>
#include <functional>
#include <memory>
#include <string>
#include <vector>

// Arduino String stand-in for getNextFileName().
using String = std::string;

struct FsStats {
  uint64_t fileOpens = 0, dirOpens = 0, openFails = 0, dirEntries = 0;
  uint64_t reads = 0, readBytes = 0, writes = 0, writeBytes = 0;
  uint64_t removes = 0, renames = 0, mkdirs = 0, exists = 0;
  void clear() { *this = FsStats(); }
  uint64_t fileWrites() const { return writes; }
};
extern FsStats g_fs;

struct HostFileImpl;
class File {
 public:
  File() = default;
  explicit File(std::shared_ptr<HostFileImpl> p) : p_(std::move(p)) {}
  explicit operator bool() const;
  bool isDirectory();
  File openNextFile();                       // opens the entry (as the ESP32 VFS does)
  std::string getNextFileName(bool* isDir);  // readdir only: full path, "" at the end
  const char* path();
  const char* name();
  size_t size();
  size_t read(uint8_t* buf, size_t len);
  size_t write(const uint8_t* buf, size_t len);
  bool seek(uint32_t pos);
  void flush() {}
  void close();

 private:
  std::shared_ptr<HostFileImpl> p_;
};

struct HostLittleFS {
  std::string root;  // host directory standing for the flash filesystem
  std::function<bool(const std::string&)> failOpen;    // true -> this open() fails
  std::function<bool(const std::string&)> failRemove;  // true -> this remove() fails (nothing removed)
  bool reverseListing = false;  // directories list in reverse name order (LittleFS promises no order)
  bool begin(bool = false, const char* = "/littlefs", int = 10, const char* = "spiffs") { return true; }
  File open(const char* path, const char* mode = "r");
  bool exists(const char* path);
  bool mkdir(const char* path);
  bool remove(const char* path);
  bool rmdir(const char* path);
  bool rename(const char* from, const char* to);
  size_t totalBytes() { return 1441792; }
  size_t usedBytes();
  void wipe();  // removes everything under root (test setup only)
};
extern HostLittleFS LittleFS;
