// HOST TEST (not production): see LittleFS.h.
#include "LittleFS.h"

#include <stdio.h>

#include <algorithm>
#include <filesystem>

namespace fs = std::filesystem;

FsStats g_fs;
HostLittleFS LittleFS;

struct HostFileImpl {
  std::string path;  // as the firmware sees it ("/messages/...")
  bool dir = false;
  FILE* f = nullptr;
  std::vector<std::string> entries;  // directory: child names, sorted
  size_t next = 0;
  ~HostFileImpl() {
    if (f) fclose(f);
  }
};

static std::string hostPath(const std::string& p) { return LittleFS.root + p; }

File::operator bool() const { return p_ != nullptr; }
bool File::isDirectory() { return p_ && p_->dir; }
const char* File::path() { return p_ ? p_->path.c_str() : ""; }
const char* File::name() {
  if (!p_) return "";
  size_t s = p_->path.rfind('/');
  return p_->path.c_str() + (s == std::string::npos ? 0 : s + 1);
}
File File::openNextFile() {
  if (!p_ || !p_->dir || p_->next >= p_->entries.size()) return File();
  std::string child = (p_->path == "/" ? "" : p_->path) + "/" + p_->entries[p_->next++];
  g_fs.dirEntries++;
  return LittleFS.open(child.c_str(), "r");
}
std::string File::getNextFileName(bool* isDir) {
  if (!p_ || !p_->dir || p_->next >= p_->entries.size()) return "";
  std::string child = (p_->path == "/" ? "" : p_->path) + "/" + p_->entries[p_->next++];
  g_fs.dirEntries++;
  std::error_code ec;
  if (isDir != nullptr) *isDir = fs::is_directory(hostPath(child), ec);
  return child;
}
size_t File::size() {
  if (!p_ || p_->dir || !p_->f) return 0;
  long cur = ftell(p_->f);
  fseek(p_->f, 0, SEEK_END);
  long n = ftell(p_->f);
  fseek(p_->f, cur, SEEK_SET);
  return n < 0 ? 0 : static_cast<size_t>(n);
}
size_t File::read(uint8_t* buf, size_t len) {
  if (!p_ || !p_->f) return 0;
  g_fs.reads++;
  size_t n = fread(buf, 1, len, p_->f);
  g_fs.readBytes += n;
  return n;
}
size_t File::write(const uint8_t* buf, size_t len) {
  if (!p_ || !p_->f) return 0;
  g_fs.writes++;
  size_t n = fwrite(buf, 1, len, p_->f);
  g_fs.writeBytes += n;
  return n;
}
bool File::seek(uint32_t pos) { return p_ && p_->f && fseek(p_->f, static_cast<long>(pos), SEEK_SET) == 0; }
void File::close() { p_.reset(); }

File HostLittleFS::open(const char* path, const char* mode) {
  std::string p(path);
  if (failOpen && failOpen(p)) {
    g_fs.openFails++;
    return File();
  }
  std::error_code ec;
  fs::path hp(hostPath(p));
  bool writing = mode != nullptr && mode[0] == 'w';
  if (!writing && fs::is_directory(hp, ec)) {
    auto impl = std::make_shared<HostFileImpl>();
    impl->path = p;
    impl->dir = true;
    for (auto& e : fs::directory_iterator(hp, ec)) impl->entries.push_back(e.path().filename().string());
    std::sort(impl->entries.begin(), impl->entries.end());
    if (reverseListing) std::reverse(impl->entries.begin(), impl->entries.end());
    g_fs.dirOpens++;
    return File(impl);
  }
  FILE* f = fopen(hp.c_str(), writing ? "wb" : "rb");
  if (f == nullptr) {
    g_fs.openFails++;
    return File();
  }
  auto impl = std::make_shared<HostFileImpl>();
  impl->path = p;
  impl->f = f;
  g_fs.fileOpens++;
  return File(impl);
}
bool HostLittleFS::exists(const char* path) {
  g_fs.exists++;
  std::error_code ec;
  return fs::exists(hostPath(path), ec);
}
bool HostLittleFS::mkdir(const char* path) {
  g_fs.mkdirs++;
  std::error_code ec;
  return fs::create_directory(hostPath(path), ec) || fs::is_directory(hostPath(path), ec);
}
bool HostLittleFS::remove(const char* path) {
  g_fs.removes++;
  if (failRemove && failRemove(path)) return false;
  std::error_code ec;
  return fs::remove(hostPath(path), ec);
}
bool HostLittleFS::rmdir(const char* path) { return remove(path); }
bool HostLittleFS::rename(const char* from, const char* to) {
  g_fs.renames++;
  std::error_code ec;
  fs::rename(hostPath(from), hostPath(to), ec);
  return !ec;
}
// Not counted, and fixed: the low-space eviction path is not what these suites
// measure, and walking every file on each append would make large histories slow.
size_t HostLittleFS::usedBytes() { return 200000; }
void HostLittleFS::wipe() {
  std::error_code ec;
  fs::remove_all(root, ec);
  fs::create_directories(root, ec);
}
