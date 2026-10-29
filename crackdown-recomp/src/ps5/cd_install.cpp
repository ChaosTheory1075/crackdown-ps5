// Crackdown PS5 - see cd_install.h.
#include "cd_install.h"
#if REX_PLATFORM_PS5

#include <algorithm>
#include <cctype>
#include <cerrno>
#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <ctime>
#include <exception>
#include <fstream>
#include <iterator>
#include <memory>
#include <span>
#include <thread>
#include <utility>

#include <arpa/inet.h>
#include <netinet/in.h>
#include <sys/socket.h>
#include <sys/stat.h>
#include <sys/time.h>

#include <rex/filesystem.h>
#include <rex/filesystem/devices/disc_image_device.h>
#include <rex/filesystem/devices/disc_image_entry.h>
#include <rex/filesystem/devices/stfs_container_device.h>
#include <rex/filesystem/entry.h>
#include <rex/filesystem/file.h>
#include <rex/logging.h>
#include <rex/system/xam/content_manager.h>
#include <rex/system/xcontent.h>
#include <rex/system/xtypes.h>

#if defined(__PROSPERO__)
#include <fcntl.h>
#include <sys/mount.h>
#include <unistd.h>
extern "C" int _fstatfs(int fd, struct statfs* buf);
extern "C" int32_t sceKernelAvailableFlexibleMemorySize(size_t* size);
// (in libc.a, which the app isn't linked with: used where it's there)
extern "C" int posix_fadvise(int fd, off_t offset, off_t length, int advice) __attribute__((weak));
#endif

namespace cdps5::install {

using rex::X_STATUS;  // (X_STATUS_SUCCESS names it unqualified)

namespace {

constexpr uint32_t kTitleId = 0x4D5307DC;
constexpr uint32_t kTypeSave = 0x00000001, kTypeDlc = 0x00000002, kTypeUpdate = 0x000B0000;
// Title Update 6's Default.xexp (TU5's is 1841152 bytes)
constexpr uint64_t kUpdate6XexpSize = 1853440;
// The DLC packages' names on the Xbox 360, the folders the runtime finds them in.
const char* const kDlcFolders[kDlcCount] = {"AA10729E62721B4EAF674486684E31CD9B94AC4F4D",
                                            "E4DDBC100F97B6954B84E82ED807E79B5A4314C04D"};
const char* const kDlcNames[kDlcCount] = {"FREE-FOR-ALL PACK", "FOUR-PLAY PACK"};
// Where an imported save waits (saves/.import/4D5307DC/...) until the game starts: the
// runtime makes its profile (a random XUID, its saves' folder) on its first start, so
// AdoptImportedSaves moves it into that profile then.
constexpr const char* kImportFolder = ".import";
// The runtime's fixed profile before it got a random one: saves an earlier build imported there.
constexpr const char* kOldDefaultXuid = "B13EBABEBABEBABE";
// The game's one save package: it looks for no other name.
constexpr const char* kSaveName = "Crackdown.sav";
constexpr const char* kBackupMarker = "crackdown-backup.txt";

std::string Lower(std::string s) {
  for (char& c : s) c = char(std::tolower(static_cast<unsigned char>(c)));
  return s;
}

std::string Ext(const fs::path& p) { return Lower(p.extension().string()); }

// `dir`'s child named `name` in any case (empty: none).
fs::path FindChild(const fs::path& dir, const std::string& name) {
  std::error_code ec;
  const std::string want = Lower(name);
  for (fs::directory_iterator it(dir, ec), end; !ec && it != end; it.increment(ec))
    if (Lower(it->path().filename().string()) == want) return it->path();
  return {};
}

bool IsPackage(const fs::path& file) {
  char magic[4] = {};
  std::ifstream in(file, std::ios::binary);
  if (!in.read(magic, 4)) return false;
  return !std::memcmp(magic, "CON ", 4) || !std::memcmp(magic, "LIVE", 4) || !std::memcmp(magic, "PIRS", 4);
}

std::string Narrow(const std::u16string& s) {
  std::string out;
  for (char16_t c : s) {
    if (!c) break;
    out += c < 0x80 ? char(c) : '?';
  }
  return out;
}

struct Header {
  uint32_t title = 0, type = 0;
  std::u16string name;
};
bool ReadHeader(const fs::path& file, Header& h) {
  const auto header = rex::filesystem::StfsContainerDevice::ReadPackageHeader(file);
  if (!header) return false;
  h.title = uint32_t(header->metadata.execution_info.title_id);
  h.type = static_cast<uint32_t>(static_cast<rex::system::XContentType>(header->metadata.content_type));
  h.name = header->metadata.display_name(rex::system::XLanguage::kEnglish);
  return true;
}

int DlcOf(const fs::path& file, const std::string& name) {
  const std::string base = file.filename().string(), lower = Lower(name);
  for (int i = 0; i < kDlcCount; ++i)
    if (Lower(base) == Lower(kDlcFolders[i])) return i;
  if (lower.find("free") != std::string::npos) return 0;
  if (lower.find("four") != std::string::npos) return 1;
  return -1;
}

std::string Gb(uint64_t bytes) {
  char s[32];
  std::snprintf(s, sizeof(s), "%.1f GB", double(bytes) / (1024.0 * 1024.0 * 1024.0));
  return s;
}

// ---- copying out of a disc image or a package ----

uint64_t TotalSize(const rex::filesystem::Entry* e) {
  if (e->attributes() & rex::filesystem::kFileAttributeDirectory) {
    uint64_t total = 0;
    for (const auto& c : e->children()) total += TotalSize(c.get());
    return total;
  }
  return e->size();
}

// The copying, done here and not through the framework's file classes: those go through the
// file cache quietly when the kernel won't move data past it (through /app0, the nullfs mount
// of the app's folder, it may not), and on the PS5 the cache keeps what it holds in the app's
// memory - after about 2 GB everything waits on it (85 MB/s to a few: the first installs). So:
//  - reads and writes go past the cache (O_DIRECT, 64 KB-aligned, 8 MB at a time) where the
//    kernel takes that;
//  - where it doesn't, they go through it, and each range read is dropped from it right after
//    (posix_fadvise DONTNEED), each 16 MB written flushed and dropped the same way;
//  - on a drive that isn't the internal one, each file is made its full size first (exFAT
//    extends a growing file's cluster chain over and over: 6 MB/s falling to 1).
// Every 512 MB the log says the speed, the time spent reading, writing and flushing, and
// whether the reads and writes really went past the cache.

constexpr size_t kAlign = size_t(64) << 10, kChunk = size_t(8) << 20;
constexpr uint64_t kFlushEvery = 16ull << 20;

uint64_t NowNs() {
  return uint64_t(
      std::chrono::duration_cast<std::chrono::nanoseconds>(std::chrono::steady_clock::now().time_since_epoch())
          .count());
}

struct IoStats {
  uint64_t read_ns = 0, write_ns = 0, sync_ns = 0;
  bool read_direct = false, write_direct = false;
};

// The disc image, read at the files' offsets.
class ImageReader {
 public:
  ~ImageReader() {
    if (fd_ >= 0) ::close(fd_);
    std::free(buffer_);
  }
  bool Open(const fs::path& path, IoStats& stats) {
    stats_ = &stats;
    buffer_ = static_cast<uint8_t*>(std::aligned_alloc(kAlign, kChunk + 2 * kAlign));
    if (!buffer_) return false;
#if defined(__PROSPERO__)
    fd_ = ::open(path.c_str(), O_RDONLY | O_DIRECT);
    direct_ = fd_ >= 0;
#endif
    if (fd_ < 0) fd_ = ::open(path.c_str(), O_RDONLY);
    stats.read_direct = direct_;
    return fd_ >= 0;
  }
  // `length` (at most kChunk) bytes at `offset` into `out`.
  bool Read(uint64_t offset, uint8_t* out, size_t length) {
    const uint64_t start = NowNs();
    bool ok = direct_ ? ReadDirect(offset, out, length) : false;
    if (!direct_) ok = ReadCached(offset, out, length);
    stats_->read_ns += NowNs() - start;
    return ok;
  }

 private:
  bool ReadDirect(uint64_t offset, uint8_t* out, size_t length) {
    const uint64_t from = offset / kAlign * kAlign, to = (offset + length + kAlign - 1) / kAlign * kAlign;
    size_t got = 0;
    const size_t want = size_t(to - from), need = size_t(offset + length - from);
    while (got < need) {
      const ssize_t n = ::pread(fd_, buffer_ + got, want - got, off_t(from + got));
      if (n < 0 && errno == EINTR) continue;
      if (n < 0 && errno == EINVAL) {
        // (refused past the cache: through it from here on, dropping what's read)
        REXLOG_WARN("Crackdown install: the disc image can't be read past the file cache; reading through it");
        const int flags = ::fcntl(fd_, F_GETFL);
        if (flags != -1) ::fcntl(fd_, F_SETFL, flags & ~O_DIRECT);
        direct_ = false;
        stats_->read_direct = false;
        return false;
      }
      if (n <= 0) break;
      got += size_t(n);
    }
    if (got < need) return false;
    std::memcpy(out, buffer_ + (offset - from), length);
    return true;
  }
  bool ReadCached(uint64_t offset, uint8_t* out, size_t length) {
    size_t got = 0;
    while (got < length) {
      const ssize_t n = ::pread(fd_, out + got, length - got, off_t(offset + got));
      if (n < 0 && errno == EINTR) continue;
      if (n <= 0) return false;
      got += size_t(n);
    }
#if defined(__PROSPERO__)
    if (&::posix_fadvise) ::posix_fadvise(fd_, off_t(offset), off_t(length), POSIX_FADV_DONTNEED);
#endif
    return true;
  }

  int fd_ = -1;
  bool direct_ = false;
  uint8_t* buffer_ = nullptr;
  IoStats* stats_ = nullptr;
};

// One file written start to end, in 8 MB pieces from an aligned buffer.
class FileWriter {
 public:
  // (whether this drive took a write past the cache: tried at the first file, then kept)
  static std::atomic<int>& DirectWorks() {
    static std::atomic<int> works{-1};  // -1 not tried yet
    return works;
  }

  FileWriter(uint8_t* buffer, IoStats& stats, bool preallocate)
      : buffer_(buffer), stats_(stats), preallocate_(preallocate) {}
  ~FileWriter() {
    if (fd_ >= 0) ::close(fd_);
  }

  bool Open(const fs::path& path, uint64_t size) {
    size_ = size;
    written_ = pending_ = flushed_ = 0;
#if defined(__PROSPERO__)
    if (DirectWorks().load() != 0) {
      fd_ = ::open(path.c_str(), O_WRONLY | O_CREAT | O_TRUNC | O_DIRECT, 0666);
      direct_ = fd_ >= 0;
    }
#endif
    if (fd_ < 0) fd_ = ::open(path.c_str(), O_WRONLY | O_CREAT | O_TRUNC, 0666);
    if (fd_ < 0) return false;
    if (preallocate_ && size) ::ftruncate(fd_, off_t(size));  // (a failure here is only slower)
    return true;
  }

  bool Write(const uint8_t* data, size_t length) {
    while (length) {
      const size_t take = std::min(length, kChunk - pending_);
      std::memcpy(buffer_ + pending_, data, take);
      pending_ += take, data += take, length -= take;
      if (pending_ == kChunk && !Put(buffer_, kChunk)) return false;
    }
    return true;
  }

  bool Close() {
    bool ok = true;
    if (pending_) {
      // the last piece: past the cache only in whole 64 KB blocks
      const size_t whole = pending_ / kAlign * kAlign;
      if (whole && !Put(buffer_, whole)) ok = false;
      if (ok && pending_) {
        std::memmove(buffer_, buffer_ + whole, pending_);
        if (direct_) {
#if defined(__PROSPERO__)
          const int flags = ::fcntl(fd_, F_GETFL);
          if (flags != -1) ::fcntl(fd_, F_SETFL, flags & ~O_DIRECT);
#endif
          direct_ = false;
        }
        if (!Put(buffer_, pending_)) ok = false;
      }
    }
    if (ok && written_ > flushed_) Flush();
    if (ok && preallocate_ && size_ != written_ && ::ftruncate(fd_, off_t(written_)) != 0) ok = false;
    if (::close(fd_) != 0) ok = false;
    fd_ = -1;
    return ok;
  }

 private:
  bool Put(const uint8_t* data, size_t length) {
    const uint64_t start = NowNs();
    size_t done = 0;
    while (done < length) {
      const ssize_t n = ::pwrite(fd_, data + done, length - done, off_t(written_ + done));
      if (n < 0 && errno == EINTR) continue;
      if (n < 0 && errno == EINVAL && direct_) {
        // (refused past the cache: through it, this file and the ones after)
#if defined(__PROSPERO__)
        const int flags = ::fcntl(fd_, F_GETFL);
        if (flags != -1) ::fcntl(fd_, F_SETFL, flags & ~O_DIRECT);
#endif
        direct_ = false;
        if (DirectWorks().exchange(0) != 0)
          REXLOG_WARN("Crackdown install: the drive takes no writes past the file cache; writing through it");
        continue;
      }
      if (n <= 0) {
        stats_.write_ns += NowNs() - start;
        return false;
      }
      done += size_t(n);
    }
    if (direct_) DirectWorks().store(1);
    stats_.write_direct = direct_;
    written_ += length;
    pending_ -= std::min(pending_, length);
    stats_.write_ns += NowNs() - start;
    if (!direct_ && written_ - flushed_ >= kFlushEvery) Flush();
    return true;
  }
  // What's written through the cache: on the drive, then out of the cache.
  void Flush() {
    const uint64_t start = NowNs();
    ::fsync(fd_);
#if defined(__PROSPERO__)
    if (&::posix_fadvise) ::posix_fadvise(fd_, off_t(flushed_), off_t(written_ - flushed_), POSIX_FADV_DONTNEED);
#endif
    flushed_ = written_;
    stats_.sync_ns += NowNs() - start;
  }

  uint8_t* buffer_;
  IoStats& stats_;
  bool preallocate_;
  int fd_ = -1;
  bool direct_ = false;
  uint64_t size_ = 0, written_ = 0, flushed_ = 0;
  size_t pending_ = 0;
};

class Copier {
 public:
  // `image`: the disc image the entries are in (read here, at their offsets); empty: the
  // entries are read through their device (a package).
  Copier(std::atomic<uint64_t>& done, bool preallocate, const fs::path& image = {})
      : done_(done), preallocate_(preallocate) {
    in_ = static_cast<uint8_t*>(std::aligned_alloc(kAlign, kChunk));
    out_ = static_cast<uint8_t*>(std::aligned_alloc(kAlign, kChunk));
    if (!image.empty()) {
      image_ = std::make_unique<ImageReader>();
      if (!image_->Open(image, stats_)) image_.reset();
    }
    started_ = last_ns_ = NowNs();
  }
  ~Copier() {
    std::free(in_);
    std::free(out_);
    const double s = double(NowNs() - started_) / 1e9;
    REXLOG_INFO("Crackdown install: {} MB in {:.0f} s ({:.0f} MB/s): reading {:.0f} s ({}), writing {:.0f} s ({}), "
                "flushing {:.0f} s",
                copied_ >> 20, s, s > 0 ? double(copied_ >> 20) / s : 0.0, double(stats_.read_ns) / 1e9,
                stats_.read_direct ? "past the cache" : "through the cache", double(stats_.write_ns) / 1e9,
                stats_.write_direct ? "past the cache" : "through the cache", double(stats_.sync_ns) / 1e9);
  }

  // `e`'s children into `dest`.
  bool Children(rex::filesystem::Entry* e, const fs::path& dest, std::string& error) {
    if (!in_ || !out_) {
      error = "out of memory";
      return false;
    }
    std::error_code ec;
    fs::create_directories(dest, ec);
    for (const auto& c : e->children())
      if (!One(c.get(), dest / c->name(), error)) return false;
    return true;
  }

 private:
  bool One(rex::filesystem::Entry* e, const fs::path& dest, std::string& error) {
    if (e->attributes() & rex::filesystem::kFileAttributeDirectory) return Children(e, dest, error);
    const size_t size = e->size();
    rex::filesystem::File* in = nullptr;
    uint64_t base = 0;
    if (image_) {
      base = static_cast<rex::filesystem::DiscImageEntry*>(e)->data_offset();
    } else if (e->Open(rex::filesystem::FileAccess::kFileReadData, &in) != X_STATUS_SUCCESS || !in) {
      error = "couldn't read " + e->path();
      return false;
    }
    FileWriter out(out_, stats_, preallocate_);
    if (!out.Open(dest, size)) {
      if (in) in->Destroy();
      error = "couldn't write " + dest.string() + " (is the drive full?)";
      return false;
    }
    size_t offset = 0;
    bool ok = true;
    while (ok && offset < size) {
      const size_t want = std::min(kChunk, size - offset);
      size_t got = 0;
      if (image_) {
        got = image_->Read(base + offset, in_, want) ? want : 0;
      } else {
        const uint64_t start = NowNs();
        in->ReadSync(std::span<uint8_t>(in_, want), offset, &got);
        stats_.read_ns += NowNs() - start;
      }
      ok = got && out.Write(in_, got);
      offset += got;
      Progress(got);
    }
    ok = out.Close() && ok && offset == size;
    if (in) in->Destroy();
    if (!ok) error = "couldn't copy " + e->path() + " (is the drive full?)";
    return ok;
  }

  void Progress(uint64_t bytes) {
    copied_ += bytes;
    done_.fetch_add(bytes, std::memory_order_relaxed);
    if (copied_ < logged_ + (512ull << 20)) return;
    const uint64_t now = NowNs();
    const double s = double(now - last_ns_) / 1e9;
    size_t flexible = 0;
#if defined(__PROSPERO__)
    sceKernelAvailableFlexibleMemorySize(&flexible);
#endif
    REXLOG_INFO("Crackdown install: {} MB copied ({:.0f} MB/s; reading {:.1f} s {}, writing {:.1f} s {}, flushing "
                "{:.1f} s so far), {} MB of memory free",
                copied_ >> 20, s > 0 ? double((copied_ - logged_) >> 20) / s : 0.0, double(stats_.read_ns) / 1e9,
                stats_.read_direct ? "direct" : "cached", double(stats_.write_ns) / 1e9,
                stats_.write_direct ? "direct" : "cached", double(stats_.sync_ns) / 1e9, flexible >> 20);
    logged_ = copied_;
    last_ns_ = now;
  }

  std::atomic<uint64_t>& done_;
  bool preallocate_;
  uint8_t *in_ = nullptr, *out_ = nullptr;
  std::unique_ptr<ImageReader> image_;
  IoStats stats_;
  uint64_t copied_ = 0, logged_ = 0, started_ = 0, last_ns_ = 0;
};

// The drive the app's folder is really on, wherever that is and whatever it's called
// (/data/homebrew/PPSA53070, /mnt/usb2/games/Crackdown, ...): /app0 is a nullfs mount of it.
struct Drive {
  std::string type;    // "ufs", "exfatfs", ... ("" not known)
  std::string folder;  // the app folder's real path ("" not known)
};

#if defined(__PROSPERO__)
bool StatFsOf(const fs::path& path, struct statfs& st) {
  std::error_code ec;
  fs::path probe;
  int fd = -1;
  if (fs::is_directory(path, ec)) {
    probe = path / ".drive-probe";
    fd = ::open(probe.c_str(), O_WRONLY | O_CREAT | O_TRUNC, 0666);
    if (fd < 0) fd = ::open(path.c_str(), O_RDONLY);  // (a folder that takes no new file)
  } else {
    fd = ::open(path.c_str(), O_RDONLY);
  }
  if (fd < 0) return false;
  const bool ok = _fstatfs(fd, &st) == 0;
  ::close(fd);
  if (!probe.empty()) ::unlink(probe.c_str());
  return ok;
}

#endif

Drive DriveOf(const fs::path& app) {
  Drive d;
#if defined(__PROSPERO__)
  struct statfs st {};
  if (!StatFsOf(app / "install", st) && !StatFsOf(app, st)) return d;
  d.type = st.f_fstypename;
  if (d.type == "nullfs") {
    d.folder = st.f_mntfromname;
    struct statfs lower {};
    if (StatFsOf(fs::path(d.folder), lower)) {
      d.type = lower.f_fstypename;
    } else {
      // (the real folder out of the app's reach: told from where it is - /data, /user/data
      // the internal drive; /mnt/ext*, /mnt/usb* an external one)
      const std::string& f = d.folder;
      d.type = f.rfind("/data/", 0) == 0 || f.rfind("/user/data/", 0) == 0                ? "ufs"
               : f.rfind("/mnt/ext", 0) == 0 || f.rfind("/mnt/usb", 0) == 0             ? "exfatfs"
                                                                                         : "";
    }
  }
#else
  (void)app;
#endif
  return d;
}

// The writing for the drive the app is on, as GTA IV for PS5 sets it: an internal (UFS) drive
// written past the file cache; an exFAT (or FAT) one through it, each file made its full size
// first (128 MB at a time) - a file that grows write by write there gets slower and slower. A
// drive not known is tried: past the cache if it takes that, through it otherwise. (Through the
// cache on the internal drive, an install fills the console's memory after about 2 GB and crawls.)
bool g_preallocate = true;

void SetWritePolicy(const fs::path& app) {
  const Drive drive = DriveOf(app);
  g_preallocate = drive.type != "ufs";
  REXLOG_INFO("Crackdown install: the app is at {} on {}{}", drive.folder.empty() ? "/app0" : drive.folder,
              drive.type.empty() ? "a drive not known" : drive.type, g_preallocate ? " (files sized first)" : "");
}

bool EnoughSpace(const fs::path& app, uint64_t need, std::string& error) {
  std::error_code ec;
  const auto space = fs::space(app, ec);
  if (!ec && space.available < need + (256ull << 20)) {
    error = "Not enough free space: it needs " + Gb(need) + ", the drive has " + Gb(space.available) + ".";
    return false;
  }
  return true;
}

void RemoveSource(const fs::path& path) {
  std::error_code ec;
  fs::remove_all(path, ec);
  if (ec) REXLOG_WARN("Crackdown install: couldn't remove {} ({})", path.string(), ec.message());
}

// ---- the installer payload ----
//
// The PS5 lets an app write about a gigabyte quickly, then slows its writing to a couple of MB/s
// (an install from inside the app took most of an hour). So the big copies are written by the
// installer payload (ps5/payload/installer, GTA IV for PS5's), which the app sends to the
// console's payload loader on 127.0.0.1:9021 (etaHEN, elfldr, ...): a process of its own, which
// writes at the drive's full speed. The app leaves it a job in its own folder and follows its
// status.txt there; the payload writes into the staging folder, and the app checks what it wrote
// and moves it into place. With no payload loader, the app copies by itself (slowly).

constexpr const char* kPayloadElf = "payloads/crackdown-ps5-installer.elf";
constexpr const char* kJobRel = "UserData/crackdown/.install-job";
constexpr const char* kStagingRel = "UserData/crackdown/.install-staging";

fs::path JobDir(const fs::path& app) { return app / kJobRel; }
fs::path Staging(const fs::path& app) { return app / kStagingRel; }

// `path` as a path inside the app's folder ("install/x.iso"), or "" when it isn't in it.
std::string InsideApp(const fs::path& app, const fs::path& path) {
  const fs::path rel = path.lexically_normal().lexically_relative(app.lexically_normal());
  if (rel.empty() || rel.is_absolute()) return {};
  const std::string first = rel.begin()->string();
  if (first == ".." || first == ".") return {};
  return rel.generic_string();
}

bool ReadText(const fs::path& path, std::string& out) {
  std::ifstream in(path, std::ios::binary);
  if (!in) return false;
  out.assign(std::istreambuf_iterator<char>(in), std::istreambuf_iterator<char>());
  return true;
}

// Whole, at once: written beside, then renamed over (never read half done).
bool WriteText(const fs::path& path, const std::string& text) {
  const fs::path tmp = path.string() + ".tmp";
  {
    std::ofstream out(tmp, std::ios::binary | std::ios::trunc);
    if (!out.write(text.data(), std::streamsize(text.size()))) return false;
  }
  std::error_code ec;
  fs::rename(tmp, path, ec);
  return !ec;
}

std::string Field(const std::string& text, const char* key) {
  const std::string k = std::string(key) + "=";
  size_t at = 0;
  while (at < text.size()) {
    size_t end = text.find('\n', at);
    if (end == std::string::npos) end = text.size();
    if (text.compare(at, k.size(), k) == 0) return text.substr(at + k.size(), end - at - k.size());
    at = end + 1;
  }
  return {};
}

uint64_t Number(const std::string& text, const char* key) {
  return std::strtoull(Field(text, key).c_str(), nullptr, 10);
}

// Seconds since `path` was written (a lot when it isn't there).
double Age(const fs::path& path) {
  struct stat st {};
  if (::stat(path.c_str(), &st) != 0) return 1e9;
  return std::difftime(std::time(nullptr), st.st_mtime);
}

struct PayloadStatus {
  std::string token, phase, error, drive;
  uint64_t copy_done = 0, copy_total = 0, write_bytes = 0, write_ns = 0, beat = 0;
  bool Working() const { return phase != "done" && phase != "failed" && phase != "stopped"; }
};

bool ReadStatus(const fs::path& dir, PayloadStatus& s) {
  std::string text;
  if (!ReadText(dir / "status.txt", text)) return false;
  s.token = Field(text, "token");
  s.phase = Field(text, "phase");
  s.error = Field(text, "error");
  s.drive = Field(text, "drive");
  s.copy_done = Number(text, "copy_done");
  s.copy_total = Number(text, "copy_total");
  s.write_bytes = Number(text, "write_bytes");
  s.write_ns = Number(text, "write_ns");
  s.beat = Number(text, "beat");
  return !s.token.empty() && !s.phase.empty();
}

// The payload to the payload loader on 127.0.0.1:9021, which runs it as a process of its own.
bool SendToLoader(const std::string& elf, int& err) {
  err = 0;
  const int fd = ::socket(AF_INET, SOCK_STREAM, 0);
  if (fd < 0) {
    err = errno;
    return false;
  }
  timeval limit{};
  limit.tv_sec = 3;
  ::setsockopt(fd, SOL_SOCKET, SO_RCVTIMEO, &limit, sizeof(limit));
  ::setsockopt(fd, SOL_SOCKET, SO_SNDTIMEO, &limit, sizeof(limit));
#ifdef SO_NOSIGPIPE
  const int one = 1;  // (a loader that hangs up mustn't end the app)
  ::setsockopt(fd, SOL_SOCKET, SO_NOSIGPIPE, &one, sizeof(one));
#endif
#ifdef MSG_NOSIGNAL
  const int flags = MSG_NOSIGNAL;
#else
  const int flags = 0;
#endif
  sockaddr_in sa{};
  sa.sin_family = AF_INET;
  sa.sin_port = htons(9021);
  sa.sin_addr.s_addr = htonl(0x7F000001);
  if (::connect(fd, reinterpret_cast<sockaddr*>(&sa), sizeof(sa)) != 0) {
    err = errno;
    ::close(fd);
    return false;
  }
  size_t at = 0;
  while (at < elf.size()) {
    const ssize_t n = ::send(fd, elf.data() + at, elf.size() - at, flags);
    if (n < 0 && errno == EINTR) continue;
    if (n <= 0) {
      err = errno;
      ::close(fd);
      return false;
    }
    at += size_t(n);
  }
  ::shutdown(fd, SHUT_WR);
  ::close(fd);
  return true;
}

// The job's files, once it's over ("running" stays: the payload's lock).
void ClearJob(const fs::path& dir) {
  std::error_code ec;
  for (const char* name : {"job.txt", "job.txt.tmp", "job.taken", "cancel", "cancel.tmp", "status.txt", "status.tmp"})
    fs::remove(dir / name, ec);
}

enum class Ran {
  kUnavailable,  // no payload loader (or no payload in the app): the app copies by itself
  kDone,         // the files are in the staging folder
  kFailed,
};

// One job (`body`: its fields, the token aside) written by the payload; `done`/`total` follow its
// copying. kDone: what it wrote is in the staging folder, marked as this job's.
Ran ByPayload(const fs::path& app, const std::string& body, std::atomic<uint64_t>& done,
              std::atomic<uint64_t>& total, std::string& error) {
  std::string elf;
  if (!ReadText(app / kPayloadElf, elf) || elf.size() < 64 || elf.compare(0, 4, "\x7F" "ELF") != 0) {
    REXLOG_WARN("Crackdown install: the installer payload ({}) is missing from the app", kPayloadElf);
    return Ran::kUnavailable;
  }
  const fs::path dir = JobDir(app);
  std::error_code ec;
  fs::create_directories(dir, ec);
  // An earlier one still at it (the app was closed during an install): asked to stop, and waited for.
  // (While one runs, another payload leaves the job to it, and nobody would take this one.)
  for (int i = 0; i < 240; ++i) {
    PayloadStatus old;
    if (!ReadStatus(dir, old) || !old.Working() || Age(dir / "status.txt") > 15) break;
    if (i == 0) {
      REXLOG_INFO("Crackdown install: an earlier install is still running ({}); stopping it", old.phase);
      WriteText(dir / "cancel", "stop\n");
    }
    std::this_thread::sleep_for(std::chrono::milliseconds(250));
  }
  ClearJob(dir);
  char made[48];
  std::snprintf(made, sizeof(made), "%lld-%lld", (long long)std::time(nullptr),
                (long long)(std::chrono::steady_clock::now().time_since_epoch().count() % 1000000));
  const std::string token = made;
  if (!WriteText(dir / "job.txt", "token=" + token + "\n" + body)) {
    REXLOG_WARN("Crackdown install: couldn't write the install job in {}", dir.string());
    return Ran::kUnavailable;
  }
  int err = 0;
  if (!SendToLoader(elf, err)) {
    fs::remove(dir / "job.txt", ec);
    REXLOG_WARN("Crackdown install: no payload loader answered on port 9021 (errno {}); installing in the app", err);
    return Ran::kUnavailable;
  }
  REXLOG_INFO("Crackdown install: sent the installer payload ({} KB) to the payload loader on port 9021, job {}",
              elf.size() / 1024, token);
  using Clock = std::chrono::steady_clock;
  const auto sent = Clock::now();
  auto changed = sent, logged = sent;
  uint64_t beat = 0;
  bool running = false;
  for (;;) {
    std::this_thread::sleep_for(std::chrono::milliseconds(250));
    const auto now = Clock::now();
    PayloadStatus s;
    if (ReadStatus(dir, s) && s.token == token) {
      if (!running)
        REXLOG_INFO("Crackdown install: the payload started after {:.1f} s ({})",
                    std::chrono::duration<float>(now - sent).count(), s.drive);
      running = true;
      if (s.beat != beat) beat = s.beat, changed = now;
      if (s.copy_total) {
        total = s.copy_total;
        done = std::min(s.copy_done, s.copy_total);
      }
      if (now - logged >= std::chrono::seconds(10) || s.phase == "done") {
        logged = now;
        REXLOG_INFO("Crackdown install: payload {}: {} of {} ({:.0f} MB/s writing)", s.phase, Gb(s.copy_done),
                    Gb(s.copy_total),
                    s.write_ns ? double(s.write_bytes) / 1048576.0 / (double(s.write_ns) / 1e9) : 0.0);
      }
      if (s.phase == "done") {
        std::string mark;
        ClearJob(dir);
        if (!ReadText(Staging(app) / ".token", mark) || mark != token) {
          error = "the installed files aren't from this install";
          return Ran::kFailed;
        }
        return Ran::kDone;
      }
      if (s.phase == "failed" || s.phase == "stopped") {
        error = s.error.empty() ? std::string("the installer payload stopped with an error") : s.error;
        ClearJob(dir);
        return Ran::kFailed;
      }
    }
    if (!running && now - sent > std::chrono::seconds(15)) {
      // Not started: the job taken back (one that starts late finds none), and the app copies by
      // itself. Taken but quiet: given longer.
      if (fs::remove(dir / "job.txt", ec) && !ec) {
        REXLOG_WARN("Crackdown install: the payload loader didn't start the installer; installing in the app");
        return Ran::kUnavailable;
      }
      if (now - sent > std::chrono::seconds(45)) {
        WriteText(dir / "cancel", "stop\n");
        error = "the installer payload didn't start";
        return Ran::kFailed;
      }
    }
    if (running && now - changed > std::chrono::seconds(30)) {
      WriteText(dir / "cancel", "stop\n");
      error = "the installer payload stopped answering";
      return Ran::kFailed;
    }
  }
}

// What the payload wrote (<staging>/<rel>) moved to `to` (the staging folder emptied).
bool TakeStaged(const fs::path& app, const std::string& rel, const fs::path& to, std::string& error) {
  std::error_code ec;
  fs::remove_all(to, ec);
  fs::create_directories(to.parent_path(), ec);
  ec.clear();
  fs::rename(Staging(app) / rel, to, ec);
  if (ec) {
    error = "couldn't move the installed files into place (" + ec.message() + ")";
    return false;
  }
  fs::remove_all(Staging(app), ec);
  return true;
}

// A package's install by the payload, into `to`: kUnavailable, or done (true) / failed.
Ran PackageByPayload(const fs::path& app, const fs::path& package, const std::string& target, uint64_t size,
                     const fs::path& to, std::atomic<uint64_t>& done, std::atomic<uint64_t>& total,
                     std::string& error) {
  const std::string source = InsideApp(app, package);
  if (source.empty()) return Ran::kUnavailable;
  const Ran ran = ByPayload(app,
                            std::string("kind=package\nstaging=") + kStagingRel + "\nsource=" + source +
                                "\nsource_kind=package\nroot=\ntarget=" + target +
                                "\nneeded=" + std::to_string(size + (64ull << 20)) + "\n",
                            done, total, error);
  if (ran == Ran::kDone && !TakeStaged(app, target, to, error)) return Ran::kFailed;
  return ran;
}

}  // namespace

const char* DlcName(int dlc) { return dlc >= 0 && dlc < kDlcCount ? kDlcNames[dlc] : "DLC"; }

std::string Where(const fs::path& app) {
  static std::string where;
  if (where.empty()) {
    const std::string folder = DriveOf(app).folder;
    where = folder.empty() ? "the app's install folder" : folder + "/install";
  }
  return where;
}

fs::path Folder(const fs::path& app) {
  std::error_code ec;
  fs::create_directories(app / "install", ec);
  return app / "install";
}

Installed Check(const fs::path& app) {
  Installed in;
  std::error_code ec;
  const fs::path game = app / "game";
  in.game = !FindChild(game, "default.xex").empty();
  const fs::path xexp = FindChild(game, "default.xexp");
  in.update = !xexp.empty() && fs::file_size(xexp, ec) == kUpdate6XexpSize;
  for (int i = 0; i < kDlcCount; ++i) {
    const fs::path d = app / "dlc" / kDlcFolders[i];
    in.dlc[i] = fs::is_directory(d, ec) && !fs::is_empty(d, ec);
  }
  for (fs::directory_iterator x(app / "saves", ec), end; !ec && x != end; x.increment(ec)) {
    std::error_code ec2;
    for (fs::directory_iterator s(x->path() / "4D5307DC" / "00000001", ec2), e2; !ec2 && s != e2; s.increment(ec2))
      if (s->is_directory(ec2)) ++in.saves;
  }
  return in;
}

bool Automatic(Kind kind) {
  return kind == Kind::kGameImage || kind == Kind::kGameFolder || kind == Kind::kUpdate || kind == Kind::kDlc;
}

namespace {

// What a file is, from what's in it (false: nothing to list).
bool Classify(const fs::path& p, Source& out) {
  const std::string ext = Ext(p), file = p.filename().string();
  if (ext == ".iso" || ext == ".xiso") {
    out = {Kind::kGameImage, p, file};
    return true;
  }
  if (ext == ".7z" || ext == ".rar" || ext == ".zip" || ext == ".001" || ext == ".gz" || ext == ".tar") {
    out = {Kind::kArchive, p, file};
    return true;
  }
  if (!IsPackage(p)) return false;
  Header h;
  if (!ReadHeader(p, h) || h.title != kTitleId) {
    out = {Kind::kOther, p, file + "  (not a Crackdown package)"};
    return true;
  }
  const std::string name = Narrow(h.name);
  if (h.type == kTypeUpdate) {
    // "Crackdown Title Update #6"; the install checks the patch itself
    const bool other = name.find('#') != std::string::npos && name.find("#6") == std::string::npos;
    out = {other ? Kind::kOtherUpdate : Kind::kUpdate, p, name.empty() ? file : name};
  } else if (h.type == kTypeDlc) {
    out = {Kind::kDlc, p, name.empty() ? file : name};
    out.dlc = DlcOf(p, name);
    if (out.dlc < 0) out.kind = Kind::kOther;
  } else if (h.type == kTypeSave) {
    out = {Kind::kSave, p, name.empty() ? file : name};
  } else {
    out = {Kind::kOther, p, file};
  }
  return true;
}

}  // namespace

std::vector<Source> Scan(const fs::path& app) {
  std::vector<Source> out;
  const fs::path root = Folder(app);
  std::error_code ec;
  // (the app's backups: install/backups/<one>/, with the marker file)
  const fs::path backups = root / "backups";
  for (fs::directory_iterator it(backups, ec), end; !ec && it != end; it.increment(ec)) {
    std::error_code ec2;
    if (it->is_directory(ec2) && fs::exists(it->path() / kBackupMarker, ec2))
      out.push_back({Kind::kBackup, it->path(), it->path().filename().string()});
  }
  ec.clear();
  // install/ and what's in its folders (four deep)
  for (fs::recursive_directory_iterator it(root, fs::directory_options::skip_permission_denied, ec), end;
       !ec && it != end; it.increment(ec)) {
    const fs::path p = it->path();
    std::error_code ec2;
    if (it->is_directory(ec2)) {
      if (p == backups) {
        it.disable_recursion_pending();
        continue;
      }
      if (!FindChild(p, "default.xex").empty()) {
        out.push_back({Kind::kGameFolder, p, p.filename().string() + "/"});
        it.disable_recursion_pending();
      } else if (it.depth() >= 4) {
        it.disable_recursion_pending();
      }
      continue;
    }
    Source s;
    if (it->is_regular_file(ec2) && Classify(p, s)) out.push_back(s);
  }
  ec.clear();
  // and the app's own folder, for what was put there instead of install/ (its own parts aside)
  static const char* const kAppParts[] = {"game",     "dlc",           "saves",  "userdata", "install", "hub",
                                          "sce_sys",  "sce_module",    "logs",   "payloads", "game.installing",
                                          "update.installing"};
  for (fs::directory_iterator it(app, ec), end; !ec && it != end; it.increment(ec)) {
    const fs::path p = it->path();
    const std::string name = Lower(p.filename().string());
    if (std::find(std::begin(kAppParts), std::end(kAppParts), name) != std::end(kAppParts)) continue;
    std::error_code ec2;
    if (it->is_directory(ec2)) {
      if (!FindChild(p, "default.xex").empty()) out.push_back({Kind::kGameFolder, p, p.filename().string() + "/"});
      continue;
    }
    Source s;
    if (it->is_regular_file(ec2) && Classify(p, s)) out.push_back(s);
  }
  // the game first, then the update, then the DLC (the order they install in)
  std::stable_sort(out.begin(), out.end(), [](const Source& a, const Source& b) { return a.kind < b.kind; });
  return out;
}

// ---- the jobs ----

float Job::Progress() const {
  const uint64_t total = total_.load(std::memory_order_relaxed);
  return total ? std::min(1.0f, float(double(done_.load(std::memory_order_relaxed)) / double(total))) : 0.0f;
}

std::string Job::Title() const {
  std::lock_guard<std::mutex> lock(mutex_);
  return title_;
}

std::string Job::Result() const {
  std::lock_guard<std::mutex> lock(mutex_);
  return result_;
}

template <typename F>
bool Job::Start(std::string title, F&& work) {
  bool expected = false;
  if (!busy_.compare_exchange_strong(expected, true)) return false;
  {
    std::lock_guard<std::mutex> lock(mutex_);
    title_ = title;
  }
  done_ = 0;
  total_ = 0;
  REXLOG_INFO("Crackdown install: {}", title);
  std::thread([this, work = std::forward<F>(work)]() mutable {
    std::string result;
    bool ok = false;
    try {
      ok = work(result);
    } catch (const std::exception& e) {
      result = std::string("Failed: ") + e.what();
    }
    REXLOG_INFO("Crackdown install: {} {}", ok ? "done:" : "FAILED:", result);
    {
      std::lock_guard<std::mutex> lock(mutex_);
      result_ = result;
    }
    failed_.store(!ok, std::memory_order_release);
    generation_.fetch_add(1, std::memory_order_acq_rel);
    busy_.store(false, std::memory_order_release);
  }).detach();
  return true;
}

bool Job::Install(const fs::path& app, const Source& source) {
  if (Busy()) return false;
  SetWritePolicy(app);
  switch (source.kind) {
    case Kind::kGameImage:
    case Kind::kGameFolder:
      return Start("Installing the game from " + source.label, [this, app, source](std::string& result) {
        std::error_code ec;
        const fs::path game = app / "game", tmp = app / "game.installing";
        fs::remove_all(tmp, ec);
        if (source.kind == Kind::kGameImage) {
          rex::filesystem::DiscImageDevice disc("\\Device\\CdInstall", source.path);
          if (!disc.Initialize()) {
            result = source.label + " isn't an Xbox 360 disc image.";
            return false;
          }
          rex::filesystem::Entry* root = disc.ResolvePath("");
          if (!root) {
            result = "Couldn't read " + source.label + ".";
            return false;
          }
          total_ = TotalSize(root);
          if (!EnoughSpace(app, total_, result)) return false;
          std::string error;
          const std::string image = InsideApp(app, source.path);
          const Ran ran =
              image.empty()
                  ? Ran::kUnavailable
                  : ByPayload(app,
                              std::string("kind=game\nstaging=") + kStagingRel + "\nimage=" + image +
                                  "\nimage_kind=iso\ndisc_root=\nupdate=\nupdate_kind=none\nupdate_root=\n"
                                  "update_xexp=\nneeded=" + std::to_string(total_ + (256ull << 20)) + "\n",
                              done_, total_, error);
          bool ok = false;
          if (ran == Ran::kDone)
            ok = TakeStaged(app, "game", tmp, error);
          else if (ran == Ran::kUnavailable)
            ok = Copier(done_, g_preallocate, source.path).Children(root, tmp, error);
          if (!ok) {
            fs::remove_all(tmp, ec);
            result = "Installing from " + source.label + " failed: " + error;
            return false;
          }
        } else {
          fs::rename(source.path, tmp, ec);
          if (ec) {
            result = "Couldn't move " + source.label + " (" + ec.message() + ").";
            return false;
          }
        }
        if (FindChild(tmp, "default.xex").empty() || FindChild(tmp, "fullcity.dff").empty()) {
          fs::remove_all(tmp, ec);
          result = source.label + " isn't Crackdown (no default.xex and fullcity.dff in it).";
          return false;
        }
        // an installed title update stays
        for (const char* keep : {"default.xexp", "Default.xexp", "update"}) {
          const fs::path was = game / keep;
          if (fs::exists(was, ec) && !fs::exists(tmp / keep, ec)) fs::rename(was, tmp / keep, ec);
        }
        fs::remove_all(game, ec);
        fs::rename(tmp, game, ec);
        if (ec) {
          result = "Couldn't put the game in place (" + ec.message() + ").";
          return false;
        }
        if (source.kind == Kind::kGameImage) Keep(source.path);
        result = "The game is installed.";
        return true;
      });
    case Kind::kUpdate:
    case Kind::kOtherUpdate:
      return Start("Installing " + source.label, [this, app, source](std::string& result) {
        std::error_code ec;
        const fs::path tmp = app / "update.installing", game = app / "game";
        fs::remove_all(tmp, ec);
        rex::filesystem::StfsContainerDevice package("\\Device\\CdUpdate", source.path);
        rex::filesystem::Entry* root = package.Initialize() ? package.ResolvePath("") : nullptr;
        if (!root) {
          result = "Couldn't read " + source.label + ".";
          return false;
        }
        total_ = TotalSize(root);
        std::string error;
        const Ran ran = PackageByPayload(app, source.path, "update", total_, tmp, done_, total_, error);
        if (ran == Ran::kFailed || (ran == Ran::kUnavailable && !Copier(done_, g_preallocate).Children(root, tmp, error))) {
          fs::remove_all(tmp, ec);
          result = "Installing " + source.label + " failed: " + error;
          return false;
        }
        const fs::path xexp = FindChild(tmp, "default.xexp");
        if (xexp.empty() || fs::file_size(xexp, ec) != kUpdate6XexpSize) {
          fs::remove_all(tmp, ec);
          result = source.label + " isn't Title Update 6, which Crackdown: ReActivated needs.";
          return false;
        }
        fs::create_directories(game, ec);
        fs::copy_file(xexp, game / "default.xexp", fs::copy_options::overwrite_existing, ec);
        fs::copy_file(xexp, game / "Default.xexp", fs::copy_options::overwrite_existing, ec);
        fs::remove(xexp, ec);
        fs::remove_all(game / "update", ec);
        fs::rename(tmp, game / "update", ec);
        if (ec) {
          result = "Couldn't put Title Update 6 in place (" + ec.message() + ").";
          return false;
        }
        Keep(source.path);
        result = "Title Update 6 is installed.";
        return true;
      });
    case Kind::kDlc:
      return Start(std::string("Installing the ") + DlcName(source.dlc), [this, app, source](std::string& result) {
        std::error_code ec;
        const fs::path dest = app / "dlc" / kDlcFolders[source.dlc];
        const fs::path tmp = dest.string() + ".installing";
        fs::remove_all(tmp, ec);
        rex::filesystem::StfsContainerDevice package("\\Device\\CdDlc", source.path);
        rex::filesystem::Entry* root = package.Initialize() ? package.ResolvePath("") : nullptr;
        if (!root) {
          result = "Couldn't read " + source.label + ".";
          return false;
        }
        total_ = TotalSize(root);
        std::string error;
        if (!EnoughSpace(app, total_, result)) return false;
        const Ran ran = PackageByPayload(app, source.path, std::string("dlc/") + kDlcFolders[source.dlc], total_, tmp,
                                         done_, total_, error);
        if (ran == Ran::kFailed || (ran == Ran::kUnavailable && !Copier(done_, g_preallocate).Children(root, tmp, error))) {
          fs::remove_all(tmp, ec);
          result = "Installing the " + std::string(DlcName(source.dlc)) + " failed: " + error;
          return false;
        }
        fs::remove_all(dest, ec);
        fs::rename(tmp, dest, ec);
        if (ec) {
          result = "Couldn't put the DLC in place (" + ec.message() + ").";
          return false;
        }
        Keep(source.path);
        result = std::string("The ") + DlcName(source.dlc) + " is installed.";
        return true;
      });
    case Kind::kSave:
      return Start("Importing the save " + source.label, [this, app, source](std::string& result) {
        std::error_code ec;
        // (into the profile when the game starts: AdoptImportedSaves; named as the game looks for it)
        const std::string name = kSaveName;
        const fs::path title = app / "saves" / kImportFolder / "4D5307DC";
        const fs::path dest = title / "00000001" / name, tmp = dest.string() + ".installing";
        fs::remove_all(tmp, ec);
        rex::filesystem::StfsContainerDevice package("\\Device\\CdSave", source.path);
        rex::filesystem::Entry* root = package.Initialize() ? package.ResolvePath("") : nullptr;
        Header h;
        if (!root || !ReadHeader(source.path, h)) {
          result = "Couldn't read the save " + source.label + ".";
          return false;
        }
        total_ = TotalSize(root);
        std::string error;
        if (!Copier(done_, g_preallocate).Children(root, tmp, error)) {
          fs::remove_all(tmp, ec);
          result = "Importing " + source.label + " failed: " + error;
          return false;
        }
        fs::remove_all(dest, ec);
        fs::rename(tmp, dest, ec);
        // its header, as the runtime writes one (ContentManager::WriteContentHeaderFile)
        rex::system::xam::XCONTENT_AGGREGATE_DATA data{};
        std::memset(&data, 0, sizeof(data));
        data.device_id = 1;
        data.content_type = rex::system::XContentType::kSavedGame;
        data.set_display_name(h.name);
        data.set_file_name(name);
        data.xuid = 0;  // (as the runtime writes its own: the folder says whose it is)
        data.title_id = kTitleId;
        const fs::path headers = title / "Headers" / "00000001";
        fs::create_directories(headers, ec);
        std::ofstream(headers / (name + ".header"), std::ios::binary)
            .write(reinterpret_cast<const char*>(&data), sizeof(data));
        Keep(source.path);
        result = "Imported the save " + source.label + ". The game loads it when you PLAY.";
        return true;
      });
    case Kind::kBackup:
      return Start("Restoring " + source.label, [app, source](std::string& result) {
        std::error_code ec;
        const auto options = fs::copy_options::recursive | fs::copy_options::overwrite_existing;
        for (const char* what : {"saves", "UserData"}) {
          if (!fs::exists(source.path / what, ec)) continue;
          fs::copy(source.path / what, app / what, options, ec);
          if (ec) {
            result = "Restoring " + source.label + " failed (" + ec.message() + ").";
            return false;
          }
        }
        if (fs::exists(source.path / "menu.ini", ec))
          fs::copy_file(source.path / "menu.ini", app / "menu.ini", fs::copy_options::overwrite_existing, ec);
        result = "Restored the backup " + source.label + ".";
        return true;
      });
    default:
      return false;
  }
}

bool Job::Uninstall(const fs::path& app, Remove what) {
  const char* name = what == Remove::kGame     ? "the game"
                     : what == Remove::kUpdate ? "Title Update 6"
                                               : DlcName(what == Remove::kDlc0 ? 0 : 1);
  return Start(std::string("Removing ") + name, [app, what, name](std::string& result) {
    std::error_code ec;
    const fs::path game = app / "game";
    switch (what) {
      case Remove::kGame:
        fs::remove_all(game, ec);
        break;
      case Remove::kUpdate:
        fs::remove(game / "default.xexp", ec);
        fs::remove(game / "Default.xexp", ec);
        fs::remove_all(game / "update", ec);
        break;
      default:
        fs::remove_all(app / "dlc" / kDlcFolders[what == Remove::kDlc0 ? 0 : 1], ec);
        break;
    }
    result = std::string("Removed ") + name + ".";
    return !ec;
  });
}

bool Job::Backup(const fs::path& app) {
  return Start("Backing up the saves", [app](std::string& result) {
    char stamp[32];
    const std::time_t now = std::time(nullptr);
    std::strftime(stamp, sizeof(stamp), "%Y-%m-%d_%H-%M-%S", std::localtime(&now));
    const fs::path dest = Folder(app) / "backups" / (std::string("crackdown-backup_") + stamp);
    std::error_code ec;
    fs::create_directories(dest, ec);
    const auto options = fs::copy_options::recursive | fs::copy_options::overwrite_existing;
    if (fs::exists(app / "saves", ec)) fs::copy(app / "saves", dest / "saves", options, ec);
    const fs::path trophies = app / "UserData" / "crackdown" / "achievements";
    if (!ec && fs::exists(trophies, ec)) {
      fs::create_directories(dest / "UserData" / "crackdown", ec);
      fs::copy(trophies, dest / "UserData" / "crackdown" / "achievements", options, ec);
    }
    if (!ec && fs::exists(app / "menu.ini", ec)) fs::copy_file(app / "menu.ini", dest / "menu.ini", ec);
    if (ec) {
      result = "The backup failed (" + ec.message() + ").";
      return false;
    }
    std::ofstream(dest / kBackupMarker) << "Crackdown: ReActivated backup: saves/, trophies (UserData/) and menu.ini.\n"
                                           "Restore it from the menu's SAVES page.\n";
    result = "Backed up to install/backups/" + dest.filename().string() + ".";
    return true;
  });
}

void Job::Keep(const fs::path& source) {
  std::lock_guard<std::mutex> lock(mutex_);
  if (std::find(leftovers_.begin(), leftovers_.end(), source) == leftovers_.end()) leftovers_.push_back(source);
}

std::vector<fs::path> Job::Leftovers() const {
  std::lock_guard<std::mutex> lock(mutex_);
  return leftovers_;
}

void Job::ForgetLeftovers() {
  std::lock_guard<std::mutex> lock(mutex_);
  leftovers_.clear();
}

bool Job::DeleteLeftovers() {
  const std::vector<fs::path> files = Leftovers();
  if (files.empty()) return false;
  return Start(files.size() == 1 ? "Deleting " + files[0].filename().string() : "Deleting the installed files",
               [this, files](std::string& result) {
                 total_ = files.size();
                 for (const fs::path& f : files) {
                   RemoveSource(f);
                   ++done_;
                 }
                 ForgetLeftovers();
                 result = files.size() == 1 ? "Deleted " + files[0].filename().string() + "."
                                            : "Deleted the " + std::to_string(files.size()) + " installed files.";
                 return true;
               });
}

void AdoptImportedSaves(const fs::path& saves, uint64_t xuid) {
  char own[17];
  std::snprintf(own, sizeof(own), "%016llX", static_cast<unsigned long long>(xuid));
  std::vector<std::string> from = {kImportFolder};
  if (std::string(own) != kOldDefaultXuid) from.push_back(kOldDefaultXuid);
  for (const std::string& folder : from) {
    const fs::path src = saves / folder / "4D5307DC";
    std::error_code ec;
    for (fs::directory_iterator it(src / "00000001", ec), end; !ec && it != end; it.increment(ec)) {
      std::error_code ec2;
      const std::string name = it->path().filename().string();
      if (!it->is_directory(ec2) || name.ends_with(".installing")) continue;
      const fs::path dest = saves / own / "4D5307DC" / "00000001" / name;
      const fs::path header = src / "Headers" / "00000001" / (name + ".header");
      const fs::path dest_header = saves / own / "4D5307DC" / "Headers" / "00000001" / (name + ".header");
      // (an import replaces the profile's save of that name: that's what importing it asks for)
      fs::remove_all(dest, ec2);
      fs::create_directories(dest.parent_path(), ec2);
      fs::rename(it->path(), dest, ec2);
      if (ec2) {
        REXLOG_ERROR("Crackdown saves: couldn't move {} into the profile ({})", it->path().string(), ec2.message());
        continue;
      }
      fs::create_directories(dest_header.parent_path(), ec2);
      fs::remove(dest_header, ec2);
      std::string bytes;
      if (std::ifstream in{header, std::ios::binary}) bytes.assign(std::istreambuf_iterator<char>(in), {});
      if (bytes.size() == sizeof(rex::system::xam::XCONTENT_AGGREGATE_DATA)) {
        // (the profile field cleared, as the runtime writes it)
        auto* data = reinterpret_cast<rex::system::xam::XCONTENT_AGGREGATE_DATA*>(bytes.data());
        data->xuid = 0;
        std::ofstream(dest_header, std::ios::binary).write(bytes.data(), std::streamsize(bytes.size()));
      }
      fs::remove(header, ec2);
      REXLOG_INFO("Crackdown saves: the imported save {} is now the profile {}'s", name, own);
    }
    ec.clear();
    // (nothing of it left but empty folders: gone)
    bool empty = true;
    for (fs::recursive_directory_iterator it(saves / folder, ec), end; !ec && it != end; it.increment(ec))
      if (it->is_regular_file()) empty = false;
    if (!ec && empty) fs::remove_all(saves / folder, ec);
  }
}

Job& TheJob() {
  static Job job;
  return job;
}

}  // namespace cdps5::install

#endif  // REX_PLATFORM_PS5
