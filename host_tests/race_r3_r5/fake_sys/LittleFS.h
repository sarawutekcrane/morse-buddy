#pragma once
// HOST STUB (not production): syntax-check surface only.
struct File {
  explicit operator bool() const { return false; }
  bool isDirectory() { return false; }
  File openNextFile() { return File(); }
  const char* path() { return ""; }
  void close() {}
};
struct HostLittleFS {
  bool begin(bool, const char*, int, const char*) { return true; }
  File open(const char*) { return File(); }
  bool remove(const char*) { return true; }
  bool rmdir(const char*) { return true; }
};
extern HostLittleFS LittleFS;
