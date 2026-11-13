// Crackdown PS5 - Crackdown 3 on HOME (cd3_setup.h).
#include "cd3_setup.h"

#if REX_PLATFORM_PS5

#include <arpa/inet.h>
#include <netinet/in.h>
#include <sys/socket.h>
#include <sys/stat.h>
#include <unistd.h>

#include <cerrno>
#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <ctime>
#include <fstream>
#include <iterator>
#include <system_error>

#include <rex/logging.h>

#include "qrcodegen/qrcodegen.hpp"

namespace cdps5::cd3 {
namespace {

constexpr const char* kPayloadElf = "payloads/crackdown-ps5-cd3.elf";
constexpr const char* kJobRel = "UserData/crackdown/.cd3-job";
constexpr const char* kPrefix = "/data/prospero-win/prefixes/cd3";

bool Exists(const fs::path& p) {
  std::error_code ec;
  return fs::exists(p, ec);
}

bool ReadText(const fs::path& path, std::string& out) {
  std::ifstream in(path, std::ios::binary);
  if (!in) return false;
  out.assign(std::istreambuf_iterator<char>(in), std::istreambuf_iterator<char>());
  return true;
}

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

// The UWPDumper dump: AppxManifest.xml naming Crackdown 3's package, and its exe.
bool IsGame(const fs::path& dir) {
  std::string manifest;
  return Exists(dir / "Nimbus/Binaries/UWP64/Crackdown3.exe") && ReadText(dir / "AppxManifest.xml", manifest) &&
         manifest.find("Microsoft.48248D2C77101") != std::string::npos;
}

bool IsWindowsFiles(const fs::path& dir) { return Exists(dir / "cd3-windows.txt"); }

// A folder in install/ (or one level down) that `test` accepts.
bool InInstall(const fs::path& app, bool (*test)(const fs::path&)) {
  std::error_code ec;
  for (const auto& e : fs::directory_iterator(app / "install", ec)) {
    if (!e.is_directory(ec)) continue;
    if (test(e.path())) return true;
    std::error_code ec2;
    for (const auto& f : fs::directory_iterator(e.path(), ec2))
      if (f.is_directory(ec2) && test(f.path())) return true;
  }
  return false;
}

// The payload to the payload loader on 127.0.0.1:9021 (as cd_install.cpp's).
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
  const int one = 1;
  ::setsockopt(fd, SOL_SOCKET, SO_NOSIGPIPE, &one, sizeof(one));
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
    const ssize_t n = ::send(fd, elf.data() + at, elf.size() - at, 0);
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

std::string g_token;  // the job this app started

}  // namespace

Setup Setup::Find(const fs::path& app) {
  Setup s;
  for (const char* root : {"/data/homebrew/", "/mnt/ext0/homebrew/", "/mnt/ext1/homebrew/", "/mnt/usb0/homebrew/",
                           "/mnt/usb1/homebrew/"})
    if (Exists(fs::path(root) / "PPSA99995" / "eboot.bin")) s.prospero = true;
  const fs::path game = app / "cd3" / "game";
  // /data/prospero-win is root's (mode 700): this app can't see inside it, so it trusts the markers the payload
  // (which can) writes into the app's own folder when an install or a sign-in finishes.
  s.installed = Exists(app / "cd3" / "installed.txt") && IsGame(game);
  s.signed_in = Exists(app / "cd3" / "signed-in.txt");
  for (const char* user : {"root", "prospero"}) {
    const fs::path w = fs::path(kPrefix) / "drive_c/users" / user / "AppData/Local/wineuwp";
    if (Exists(w / "xboxauth.bin") || Exists(w / "xboxauth-import.txt")) s.signed_in = true;
  }
  s.dump = IsGame(game) || InInstall(app, IsGame);
  s.windows = IsWindowsFiles(app / "cd3" / "windows") || InInstall(app, IsWindowsFiles);
  s.data = Exists(app / "cd3data" / "ms-files.tsv") && Exists(app / "cd3data" / "template");
  return s;
}

bool Start(const fs::path& app, Job::Kind kind, Job& job, std::string& error, const std::string& extra) {
  std::string elf;
  if (!ReadText(app / kPayloadElf, elf) || elf.size() < 64 || elf.compare(0, 4, "\x7F" "ELF") != 0) {
    error = "The app's Crackdown 3 payload is missing (payloads/crackdown-ps5-cd3.elf). Reinstall the Crackdown app.";
    return false;
  }
  const fs::path dir = app / kJobRel;
  std::error_code ec;
  fs::create_directories(dir, ec);
  for (const char* name : {"job.txt", "job.taken", "cancel", "status.txt", "status.txt.tmp"}) fs::remove(dir / name, ec);
  char made[48];
  std::snprintf(made, sizeof(made), "%lld-%lld", (long long)std::time(nullptr),
                (long long)(std::chrono::steady_clock::now().time_since_epoch().count() % 1000000));
  g_token = made;
  const char* what = kind == Job::Kind::kInstall ? "install" : kind == Job::Kind::kLaunch ? "launch" : "signin";
  if (!WriteText(dir / "job.txt", "token=" + g_token + "\nkind=" + what + "\n" + extra)) {
    error = "The job couldn't be written in " + dir.string() + ".";
    return false;
  }
  int err = 0;
  if (!SendToLoader(elf, err)) {
    fs::remove(dir / "job.txt", ec);
    error = "No payload loader answered (etaHEN or elfldr, port 9021). Start one, then try again.";
    REXLOG_WARN("Crackdown 3: no payload loader on port 9021 (errno {})", err);
    return false;
  }
  REXLOG_INFO("Crackdown 3: {} payload sent ({} KB), job {}", what, elf.size() / 1024, g_token);
  job = Job{};
  job.kind = kind;
  job.phase = "working";
  job.message = kind == Job::Kind::kInstall ? "Starting the install" : "Starting the sign-in";
  job.started = -1;  // (Tick sets it)
  return true;
}

void Tick(const fs::path& app, Job& job, double now) {
  if (job.kind == Job::Kind::kNone) return;
  if (job.started < 0) job.started = now;
  std::string text;
  if (!ReadText(app / kJobRel / "status.txt", text) || Field(text, "token") != g_token) {
    if (job.Active() && now - job.started > 20) {
      job.phase = "failed";
      job.error = "The Crackdown 3 payload didn't start. Is a payload loader (etaHEN or elfldr) running?";
    }
    return;
  }
  const std::string was = job.phase;
  job.phase = Field(text, "phase");
  job.message = Field(text, "message");
  job.error = Field(text, "error");
  job.code = Field(text, "code");
  job.url = Field(text, "url");
  job.done = std::strtoull(Field(text, "done").c_str(), nullptr, 10);
  job.total = std::strtoull(Field(text, "total").c_str(), nullptr, 10);
  // strtoll, not atoll: atoll is a libc import the PS5 builds don't have yet (a new import can stop an eboot launching)
  const long long beat = std::strtoll(Field(text, "beat").c_str(), nullptr, 10);
  if (job.Active() && beat && std::time(nullptr) - beat > 90) {
    job.phase = "failed";
    job.error = "The Crackdown 3 payload stopped answering. Try again.";
  }
  if (job.phase != was) REXLOG_INFO("Crackdown 3: job {} {} ({})", g_token, job.phase, job.phase == "failed" ? job.error : job.message);
}

void Stop(const fs::path& app) {
  WriteText(app / kJobRel / "cancel", "stop\n");
}

std::vector<bool> QrModules(const std::string& text, int& size) {
  size = 0;
  std::vector<bool> out;
  try {
    const auto qr = qrcodegen::QrCode::encodeText(text.c_str(), qrcodegen::QrCode::Ecc::MEDIUM);
    size = qr.getSize();
    out.resize(size_t(size) * size_t(size));
    for (int y = 0; y < size; ++y)
      for (int x = 0; x < size; ++x) out[size_t(y) * size_t(size) + size_t(x)] = qr.getModule(x, y);
  } catch (const std::exception& e) {
    REXLOG_WARN("Crackdown 3: no QR code ({})", e.what());
    size = 0;
    out.clear();
  }
  return out;
}

}  // namespace cdps5::cd3

#endif  // REX_PLATFORM_PS5
