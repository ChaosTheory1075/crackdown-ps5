/* Crackdown: ReActivated - Crackdown 3's payload (payloads/crackdown-ps5-cd3.elf): its install and its Xbox sign-in.
 *
 * Crackdown 3 is the player's own PC (UWP) copy, run by Prospero Win (PPSA99995) through its "cd3" profile. The app
 * leaves a job in <app>/UserData/crackdown/.cd3-job/job.txt and sends this to the payload loader (port 9021), as it
 * does the installer payload; this writes status.txt there (written beside and renamed over) and logs/cd3.log.
 *
 *   kind=install
 *     <app>/install/: the player's UWPDumper dump (a folder with AppxManifest.xml naming Microsoft.48248D2C77101 and
 *     Nimbus/Binaries/UWP64/Crackdown3.exe) and "Crackdown 3 Windows files" (the PC helper's folder: cd3-windows.txt).
 *     Either may already be in place: <app>/cd3/game, <app>/cd3/windows.
 *     1. both moved into <app>/cd3/ (a rename: the same drive);
 *     2. the prefix /data/prospero-win/prefixes/cd3 built from <app>/cd3data/template (Wine's files, ours) and the
 *        Windows files (cd3data/ms-files.tsv: each one's place), the game's path written into its registry; a prefix
 *        already there keeps the player's data (AppData/Local/wineuwp: sign-in and saves; the game's LocalState);
 *     3. the profile /data/prospero-win/profiles/cd3.profile (cd3data/cd3.profile.in) and its line in profiles.lst;
 *     4. Prospero Win's runtime overlay (cd3data/overlay: wine-uwp's PE build and pw-merge's signed PRX), every file
 *        it replaces kept first in <app>/cd3-backup/ (once: the first install's copy is the stock one).
 *   kind=signin
 *     Microsoft's device-code sign-in (login.live.com, the Xbox app's client): status phase=code with code= and url=
 *     for the app to show (text and QR code), then polls; the refresh token goes to xboxauth-import.txt in the
 *     prefix's AppData/Local/wineuwp (users root and prospero), which Wine's winexboxauth takes in on the game's start.
 *
 * Signed PRX are kept wrapped in cd3data ("CD3PRXW1" + the signed file): the console decrypts a signed file for any
 * reader, so copying one by reading it here would write the plain ELF the loader refuses.
 * SPDX-License-Identifier: GPL-3.0-or-later */

#include <curl/curl.h>

#include <algorithm>
#include <cerrno>
#include <cstdarg>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <ctime>
#include <dirent.h>
#include <fcntl.h>
#include <string>
#include <sys/file.h>
#include <sys/stat.h>
#include <unistd.h>
#include <vector>

#ifndef CD3_ROOT
#define CD3_ROOT "" /* (a test on a PC builds with a fake console tree's folder here) */
#endif

namespace {

const char *const kJobRel = "UserData/crackdown/.cd3-job";
const std::string kProfiles = CD3_ROOT "/data/prospero-win/profiles";
const std::string kPrefixes = CD3_ROOT "/data/prospero-win/prefixes";
const char *const kMsaClient = "000000004C12AE6F";  // the Xbox app's client (Wine's winexboxauth MSA_CLIENT_ID)
const char *const kMsaScope = "service::user.auth.xboxlive.com::MBI_SSL";

std::string g_app, g_dir, g_token, g_log, g_job;
std::string g_phase = "working", g_message, g_error, g_code, g_url;
std::uint64_t g_done = 0, g_total = 0;

void log_line(const char *fmt, ...)
{
    char text[2048];
    va_list ap;
    va_start(ap, fmt);
    std::vsnprintf(text, sizeof text, fmt, ap);
    va_end(ap);
    if (std::FILE *f = std::fopen(g_log.c_str(), "a")) {
        char when[32];
        const std::time_t t = std::time(nullptr);
        std::strftime(when, sizeof when, "%Y-%m-%d %H:%M:%S", std::localtime(&t));
        std::fprintf(f, "%s  %s\n", when, text);
        std::fclose(f);
    }
}

bool read_text(const std::string &path, std::string &out)
{
    std::FILE *f = std::fopen(path.c_str(), "rb");
    if (!f)
        return false;
    out.clear();
    char buf[65536];
    size_t n;
    while ((n = std::fread(buf, 1, sizeof buf, f)) > 0)
        out.append(buf, n);
    std::fclose(f);
    return true;
}

bool write_text(const std::string &path, const std::string &text)
{
    const std::string tmp = path + ".tmp";
    std::FILE *f = std::fopen(tmp.c_str(), "wb");
    if (!f)
        return false;
    const bool ok = std::fwrite(text.data(), 1, text.size(), f) == text.size();
    std::fclose(f);
    return ok && std::rename(tmp.c_str(), path.c_str()) == 0;
}

std::string field(const std::string &text, const char *key)
{
    const std::string k = std::string(key) + "=";
    size_t at = 0;
    while (at < text.size()) {
        size_t end = text.find('\n', at);
        if (end == std::string::npos)
            end = text.size();
        if (text.compare(at, k.size(), k) == 0)
            return text.substr(at + k.size(), end - at - k.size());
        at = end + 1;
    }
    return {};
}

void write_status()
{
    std::string s = "token=" + g_token + "\nphase=" + g_phase + "\nmessage=" + g_message + "\nerror=" + g_error +
                    "\ncode=" + g_code + "\nurl=" + g_url + "\ndone=" + std::to_string(g_done) +
                    "\ntotal=" + std::to_string(g_total) + "\nbeat=" + std::to_string(std::time(nullptr)) + "\n";
    write_text(g_dir + "/status.txt", s);
}

void step(const char *message)
{
    g_message = message;
    log_line("%s", message);
    write_status();
}

int fail(const std::string &why)
{
    g_phase = "failed";
    g_error = why;
    log_line("FAILED: %s", why.c_str());
    write_status();
    return 1;
}

bool cancelled()
{
    struct stat st{};
    return ::stat((g_dir + "/cancel").c_str(), &st) == 0;
}

bool exists(const std::string &p)
{
    struct stat st{};
    return ::stat(p.c_str(), &st) == 0;
}

bool is_dir(const std::string &p)
{
    struct stat st{};
    return ::stat(p.c_str(), &st) == 0 && S_ISDIR(st.st_mode);
}

bool mkdirs(const std::string &path)
{
    if (path.empty() || is_dir(path))
        return true;
    const size_t slash = path.find_last_of('/');
    if (slash != std::string::npos && slash > 0 && !mkdirs(path.substr(0, slash)))
        return false;
    return ::mkdir(path.c_str(), 0777) == 0 || errno == EEXIST;
}

std::vector<std::string> list(const std::string &dir)
{
    std::vector<std::string> out;
    if (DIR *d = ::opendir(dir.c_str())) {
        while (dirent *e = ::readdir(d))
            if (std::strcmp(e->d_name, ".") && std::strcmp(e->d_name, ".."))
                out.push_back(e->d_name);
        ::closedir(d);
    }
    std::sort(out.begin(), out.end());
    return out;
}

bool remove_tree(const std::string &path)
{
    struct stat st{};
    if (::lstat(path.c_str(), &st) != 0)
        return errno == ENOENT;
    if (S_ISDIR(st.st_mode)) {
        for (const std::string &n : list(path))
            if (!remove_tree(path + "/" + n))
                return false;
        return ::rmdir(path.c_str()) == 0;
    }
    return ::unlink(path.c_str()) == 0;
}

/* One file, whole; `skip` leading bytes of the source left out (a wrapped PRX's 8). */
bool copy_file(const std::string &from, const std::string &to, std::string &error, size_t skip = 0)
{
    const int in = ::open(from.c_str(), O_RDONLY);
    if (in < 0) {
        error = "can't read " + from + " (" + std::strerror(errno) + ")";
        return false;
    }
    const size_t slash = to.find_last_of('/');
    mkdirs(to.substr(0, slash));
    const std::string tmp = to + ".cd3tmp";
    const int out = ::open(tmp.c_str(), O_WRONLY | O_CREAT | O_TRUNC, 0777);
    if (out < 0) {
        error = "can't write " + to + " (" + std::strerror(errno) + ")";
        ::close(in);
        return false;
    }
    static std::vector<char> buf(4 << 20);
    bool ok = true;
    if (skip && ::lseek(in, off_t(skip), SEEK_SET) != off_t(skip))
        ok = false;
    for (ssize_t n; ok && (n = ::read(in, buf.data(), buf.size())) != 0;) {
        if (n < 0) {
            ok = errno == EINTR;
            continue;
        }
        for (ssize_t at = 0; at < n;) {
            const ssize_t w = ::write(out, buf.data() + at, size_t(n - at));
            if (w <= 0) {
                ok = false;
                break;
            }
            at += w;
        }
        g_done += std::uint64_t(n);
    }
    ::close(in);
    if (::close(out) != 0)
        ok = false;
    if (!ok || std::rename(tmp.c_str(), to.c_str()) != 0) {
        error = "writing " + to + " failed (" + std::strerror(errno) + ")";
        ::unlink(tmp.c_str());
        return false;
    }
    return true;
}

bool copy_tree(const std::string &from, const std::string &to, std::string &error)
{
    if (!mkdirs(to)) {
        error = "can't make " + to;
        return false;
    }
    for (const std::string &n : list(from)) {
        if (cancelled()) {
            error = "stopped";
            return false;
        }
        const std::string f = from + "/" + n, t = to + "/" + n;
        if (is_dir(f) ? !copy_tree(f, t, error) : !copy_file(f, t, error))
            return false;
    }
    return true;
}

std::uint64_t tree_bytes(const std::string &path)
{
    struct stat st{};
    if (::lstat(path.c_str(), &st) != 0)
        return 0;
    if (!S_ISDIR(st.st_mode))
        return std::uint64_t(st.st_size);
    std::uint64_t n = 0;
    for (const std::string &e : list(path))
        n += tree_bytes(path + "/" + e);
    return n;
}

/* the app's folder: the one with the newest job */
bool find_app()
{
    std::vector<std::string> places;
    for (const std::string root : {CD3_ROOT "/data/homebrew", CD3_ROOT "/mnt/ext0/homebrew", CD3_ROOT "/mnt/ext1/homebrew",
                                   CD3_ROOT "/mnt/usb0/homebrew", CD3_ROOT "/mnt/usb1/homebrew"})
        for (const std::string &n : list(root))
            places.push_back(root + "/" + n);
    std::time_t best = 0;
    for (const std::string &p : places) {
        struct stat st{};
        if (::stat((p + "/" + kJobRel + "/job.txt").c_str(), &st) == 0 && st.st_mtime > best) {
            best = st.st_mtime;
            g_app = p;
        }
    }
    return !g_app.empty() && std::time(nullptr) - best < 600;
}

std::string prospero_dir()
{
    for (const std::string root : {CD3_ROOT "/data/homebrew", CD3_ROOT "/mnt/ext0/homebrew", CD3_ROOT "/mnt/ext1/homebrew",
                                   CD3_ROOT "/mnt/usb0/homebrew", CD3_ROOT "/mnt/usb1/homebrew"})
        if (exists(root + "/PPSA99995/eboot.bin"))
            return root + "/PPSA99995";
    return {};
}

/* "/data/homebrew/PPSA53070/cd3/game" -> "Z:\data\homebrew\PPSA53070\cd3\game" (doubled: in a .reg value) */
std::string windows_path(const std::string &path, bool reg)
{
    const std::string root = CD3_ROOT;
    const std::string unix_path = path.compare(0, root.size(), root) == 0 ? path.substr(root.size()) : path;
    std::string w = "Z:";
    for (char c : unix_path) {
        if (c == '/')
            w += reg ? "\\\\" : "\\";
        else
            w += c;
    }
    return w;
}

void replace_all(std::string &s, const std::string &a, const std::string &b)
{
    for (size_t at = 0; (at = s.find(a, at)) != std::string::npos; at += b.size())
        s.replace(at, a.size(), b);
}

bool is_game(const std::string &dir)
{
    std::string manifest;
    return exists(dir + "/Nimbus/Binaries/UWP64/Crackdown3.exe") && read_text(dir + "/AppxManifest.xml", manifest) &&
           manifest.find("Microsoft.48248D2C77101") != std::string::npos;
}

/* a folder in install/ (or one level down) that `test` accepts */
std::string find_in_install(bool (*test)(const std::string &))
{
    const std::string inst = g_app + "/install";
    for (const std::string &n : list(inst)) {
        const std::string p = inst + "/" + n;
        if (!is_dir(p))
            continue;
        if (test(p))
            return p;
        for (const std::string &m : list(p))
            if (is_dir(p + "/" + m) && test(p + "/" + m))
                return p + "/" + m;
    }
    return {};
}

bool is_windows_files(const std::string &dir) { return exists(dir + "/cd3-windows.txt"); }

struct MsFile {
    std::string target, source;
    std::uint64_t size = 0;
};

int install()
{
    const std::string data = g_app + "/cd3data", cd3 = g_app + "/cd3";
    const std::string game = cd3 + "/game", windows = cd3 + "/windows", frameworks = cd3 + "/frameworks";
    const std::string prospero = prospero_dir();
    if (prospero.empty())
        return fail("Prospero Win isn't installed. Install it from the Recomp store, then install Crackdown 3 again.");
    if (!exists(data + "/ms-files.tsv") || !is_dir(data + "/template") || !is_dir(data + "/overlay"))
        return fail("The app's Crackdown 3 files (cd3data) are missing. Reinstall the Crackdown app.");
    mkdirs(cd3);

    /* 1. the game and the Windows files into <app>/cd3 */
    step("Finding your Crackdown 3 files");
    if (!is_game(game)) {
        const std::string dump = find_in_install(is_game);
        if (dump.empty())
            return fail("No Crackdown 3 dump found. Put the folder UWPDumper made (with AppxManifest.xml in it) in "
                        "the app's install/ folder.");
        if (exists(game) && !remove_tree(game))
            return fail("An old " + game + " folder couldn't be removed.");
        if (std::rename(dump.c_str(), game.c_str()) != 0)
            return fail("Moving " + dump + " to " + game + " failed (" + std::strerror(errno) + ").");
        log_line("game: %s -> %s", dump.c_str(), game.c_str());
    }
    if (!is_windows_files(windows)) {
        const std::string w = find_in_install(is_windows_files);
        if (w.empty())
            return fail("The Windows files are missing. Run the PC helper (cd3_windows_files.py) and put the folder "
                        "it makes, \"Crackdown 3 Windows files\", in the app's install/ folder.");
        if (exists(windows) && !remove_tree(windows))
            return fail("An old " + windows + " folder couldn't be removed.");
        if (std::rename(w.c_str(), windows.c_str()) != 0)
            return fail("Moving " + w + " to " + windows + " failed (" + std::strerror(errno) + ").");
        log_line("windows files: %s -> %s", w.c_str(), windows.c_str());
    }

    /* what goes where */
    std::string tsv;
    read_text(data + "/ms-files.tsv", tsv);
    std::vector<MsFile> ms;
    for (size_t at = 0; at < tsv.size();) {
        size_t end = tsv.find('\n', at);
        if (end == std::string::npos)
            end = tsv.size();
        const std::string line = tsv.substr(at, end - at);
        at = end + 1;
        const size_t t1 = line.find('\t'), t2 = line.find('\t', t1 + 1), t3 = line.find('\t', t2 + 1);
        if (t1 == std::string::npos || t2 == std::string::npos || t3 == std::string::npos)
            continue;
        MsFile f;
        f.target = line.substr(0, t1);
        f.source = line.substr(t1 + 1, t2 - t1 - 1);
        f.size = std::strtoull(line.substr(t2 + 1, t3 - t2 - 1).c_str(), nullptr, 10);
        ms.push_back(f);
    }
    for (const MsFile &f : ms) {
        struct stat st{};
        if (::stat((windows + "/" + f.source).c_str(), &st) != 0 || std::uint64_t(st.st_size) != f.size)
            return fail("A Windows file is missing or different: " + f.source + ". Run the PC helper again.");
    }

    /* 2. the prefix, built beside and moved in */
    const std::string prefix = kPrefixes + "/cd3", fresh = kPrefixes + "/.cd3-new";
    g_total = tree_bytes(data + "/template");
    for (const MsFile &f : ms)
        g_total += f.size;
    g_total += tree_bytes(data + "/overlay");
    g_done = 0;
    step("Building Crackdown 3's Wine prefix");
    remove_tree(fresh);
    std::string error;
    if (!copy_tree(data + "/template", fresh, error))
        return fail("Copying the prefix failed: " + error);
    {
        std::string dirs;
        if (read_text(data + "/template-empty-dirs.txt", dirs))
            for (size_t at = 0; at < dirs.size();) {
                size_t end = dirs.find('\n', at);
                if (end == std::string::npos)
                    end = dirs.size();
                if (end > at)
                    mkdirs(fresh + "/" + dirs.substr(at, end - at));
                at = end + 1;
            }
    }
    step("Adding the Windows files");
    for (const MsFile &f : ms) {
        if (cancelled())
            return fail("stopped");
        const std::string to = f.target.compare(0, 12, "@frameworks/") == 0 ? frameworks + "/" + f.target.substr(12)
                                                                             : fresh + "/" + f.target;
        if (!copy_file(windows + "/" + f.source, to, error))
            return fail("Adding the Windows files failed: " + error);
        // The VCLibs runtime also next to the game's exe: the exe imports vccorlib140_app / concrt140_app
        // directly and Wine's loader looked only in its own folder (c0000135 at start, 2026-11-11).
        const size_t slash = f.target.rfind('/');
        const std::string base = slash == std::string::npos ? f.target : f.target.substr(slash + 1);
        if (f.target.compare(0, 12, "@frameworks/") == 0 && base.size() > 8 &&
            base.compare(base.size() - 8, 8, "_app.dll") == 0 &&
            !copy_file(windows + "/" + f.source, game + "/Nimbus/Binaries/UWP64/" + base, error))
            return fail("Adding the VCLibs runtime failed: " + error);
    }
    {
        std::string reg;
        if (!read_text(fresh + "/user.reg", reg))
            return fail("The prefix's registry is missing from the app's files.");
        replace_all(reg, "@CD3_GAME@", windows_path(game, true));
        replace_all(reg, "@CD3_FRAMEWORKS@", windows_path(frameworks, true));
        if (!write_text(fresh + "/user.reg", reg))
            return fail("Writing the prefix's registry failed.");
    }
    if (is_dir(prefix)) {
        /* the player's sign-in, saves and settings go to the new prefix */
        for (const char *user : {"root", "prospero"})
            for (const char *keep : {"AppData/Local/wineuwp", "AppData/Local/Packages"}) {
                const std::string from = prefix + "/drive_c/users/" + user + "/" + keep;
                if (is_dir(from) && !copy_tree(from, fresh + "/drive_c/users/" + user + "/" + keep, error))
                    return fail("Keeping your Crackdown 3 saves failed: " + error);
            }
        /* only the latest earlier prefix is kept (each is about 2 GB) */
        for (const std::string &n : list(kPrefixes))
            if (n.compare(0, 11, "cd3.before-") == 0)
                remove_tree(kPrefixes + "/" + n);
        char old[512];
        std::snprintf(old, sizeof old, "%s/cd3.before-%ld", kPrefixes.c_str(), long(std::time(nullptr)));
        if (std::rename(prefix.c_str(), old) != 0)
            return fail("The old prefix couldn't be moved aside (" + std::string(std::strerror(errno)) + ").");
        log_line("old prefix kept as %s", old);
    }
    if (std::rename(fresh.c_str(), prefix.c_str()) != 0)
        return fail("Putting the prefix in place failed (" + std::string(std::strerror(errno)) + ").");

    /* 3. the profile and its line */
    step("Adding Crackdown 3 to Prospero Win");
    {
        std::string profile;
        if (!read_text(data + "/cd3.profile.in", profile))
            return fail("The profile is missing from the app's files.");
        replace_all(profile, "@CD3_GAME_WIN@", windows_path(game, false));
        mkdirs(kProfiles);
        if (!write_text(kProfiles + "/cd3.profile", profile))
            return fail("Writing Prospero Win's profile failed.");
        std::string lst;
        read_text(kProfiles + ".lst", lst);
        if (lst.find("cd3.profile") == std::string::npos) {
            if (!lst.empty() && lst.back() != '\n')
                lst += "\n";
            lst += "cd3.profile\n";
            if (!write_text(kProfiles + ".lst", lst))
                return fail("Adding Crackdown 3 to Prospero Win's list failed.");
        }
    }

    /* 4. the runtime overlay, Prospero Win's own files kept first */
    step("Updating Prospero Win's Wine for Crackdown 3");
    const std::string lib = prospero + "/win/wine/lib/wine", backup = g_app + "/cd3-backup";
    for (const char *sub : {"x86_64-windows", "x86_64-unix"}) {
        const std::string from = data + "/overlay/" + sub;
        for (const std::string &n : list(from)) {
            const bool wrapped = n.size() > 2 && n.compare(n.size() - 2, 2, ".w") == 0;
            const std::string name = wrapped ? n.substr(0, n.size() - 2) : n;
            const std::string target = lib + "/" + sub + "/" + name, keep = backup + "/" + sub + "/" + name;
            if (exists(target) && !exists(keep)) {
                /* (a signed PRX reads back decrypted: its copy is for the record, Prospero Win's reinstall restores it) */
                if (!copy_file(target, keep, error))
                    return fail("Keeping Prospero Win's own " + name + " failed: " + error);
            }
            if (wrapped) {
                char magic[8] = {};
                const int fd = ::open((from + "/" + n).c_str(), O_RDONLY);
                const bool ok = fd >= 0 && ::read(fd, magic, 8) == 8 && std::memcmp(magic, "CD3PRXW1", 8) == 0;
                if (fd >= 0)
                    ::close(fd);
                if (!ok)
                    return fail("The app's copy of " + name + " is damaged. Reinstall the Crackdown app.");
            }
            if (!copy_file(from + "/" + n, target, error, wrapped ? 8 : 0))
                return fail("Updating Prospero Win failed (is it running? close it and try again): " + error);
        }
    }
    {
        /* The game's settings the first time: Extreme, 200% resolution scale, 60 fps (the user's, 2026-11-11), not the
         * Low / 80% it picks for itself. A player's own settings file is never replaced. */
        const std::string cfg = kPrefixes + "/cd3/drive_c/users/root/AppData/Local/Packages/"
                                "Microsoft.48248D2C77101_8wekyb3d8bbwe/LocalState/Nimbus/Saved/Config/UWP";
        struct stat st;
        std::string text;
        if (::stat((cfg + "/GameUserSettings.ini").c_str(), &st) != 0 && read_text(data + "/GameUserSettings.ini", text)) {
            mkdirs(cfg);
            write_text(cfg + "/GameUserSettings.ini", text);
        }
    }
    write_text(cd3 + "/installed.txt", "Crackdown 3 installed " + std::to_string(std::time(nullptr)) +
                                           "\ngame=" + game + "\nprospero=" + prospero + "\n");
    // The app itself can't look inside /data/prospero-win (root only, mode 700): it learns what the payload saw
    // from files in its own folder. A sign-in already in the prefix (a reinstall) counts as signed in.
    for (const char *user : {"root", "prospero"}) {
        const std::string w = kPrefixes + "/cd3/drive_c/users/" + user + "/AppData/Local/wineuwp";
        struct stat st;
        if (::stat((w + "/xboxauth.bin").c_str(), &st) == 0 || ::stat((w + "/xboxauth-import.txt").c_str(), &st) == 0) {
            write_text(cd3 + "/signed-in.txt", "signed in (found in the prefix at install)\n");
            break;
        }
    }
    g_phase = "done";
    step("Crackdown 3 is installed");
    return 0;
}

/* --- the Xbox sign-in --- */

size_t keep_reply(char *data, size_t size, size_t n, void *out)
{
    static_cast<std::string *>(out)->append(data, size * n);
    return size * n;
}

std::string url_encode(const std::string &s)
{
    static const char *hex = "0123456789ABCDEF";
    std::string out;
    for (unsigned char c : s)
        if (std::isalnum(c) || c == '-' || c == '_' || c == '.' || c == '~')
            out += char(c);
        else {
            out += '%';
            out += hex[c >> 4];
            out += hex[c & 15];
        }
    return out;
}

/* a JSON string value, simply (Microsoft's replies are flat objects) */
std::string json_str(const std::string &j, const char *key)
{
    const std::string k = std::string("\"") + key + "\"";
    size_t at = j.find(k);
    if (at == std::string::npos)
        return {};
    at = j.find(':', at + k.size());
    if (at == std::string::npos)
        return {};
    ++at;
    while (at < j.size() && std::isspace(static_cast<unsigned char>(j[at])))
        ++at;
    if (at < j.size() && j[at] == '"') {
        std::string v;
        for (++at; at < j.size() && j[at] != '"'; ++at) {
            if (j[at] == '\\' && at + 1 < j.size())
                ++at;
            v += j[at];
        }
        return v;
    }
    size_t end = at;
    while (end < j.size() && j[end] != ',' && j[end] != '}')
        ++end;
    return j.substr(at, end - at);
}

long post(const char *url, const std::string &form, std::string &reply, std::string &error)
{
    CURL *curl = curl_easy_init();
    if (!curl) {
        error = "no connection";
        return 0;
    }
    reply.clear();
    curl_easy_setopt(curl, CURLOPT_URL, url);
    curl_easy_setopt(curl, CURLOPT_POSTFIELDS, form.c_str());
    curl_easy_setopt(curl, CURLOPT_USERAGENT, "CrackdownReActivated-PS5/1.0");
    curl_easy_setopt(curl, CURLOPT_WRITEFUNCTION, keep_reply);
    curl_easy_setopt(curl, CURLOPT_WRITEDATA, &reply);
    curl_easy_setopt(curl, CURLOPT_CONNECTTIMEOUT, 20L);
    curl_easy_setopt(curl, CURLOPT_TIMEOUT, 60L);
    curl_easy_setopt(curl, CURLOPT_NOSIGNAL, 1L);
    /* Microsoft's certificate checked against the bundle the app ships (cd3data/ca-certificates.crt) */
    const std::string ca = g_app + "/cd3data/ca-certificates.crt";
    curl_easy_setopt(curl, CURLOPT_CAINFO, ca.c_str());
    const CURLcode rc = curl_easy_perform(curl);
    long http = 0;
    curl_easy_getinfo(curl, CURLINFO_RESPONSE_CODE, &http);
    curl_easy_cleanup(curl);
    if (rc != CURLE_OK) {
        error = curl_easy_strerror(rc);
        return 0;
    }
    return http;
}

int signin()
{
    if (!is_dir(kPrefixes + "/cd3"))
        return fail("Install Crackdown 3 first.");
    curl_global_init(CURL_GLOBAL_DEFAULT);
    step("Asking Microsoft for a sign-in code");
    std::string reply, error;
    const std::string scope = url_encode(kMsaScope);
    long http = post("https://login.live.com/oauth20_connect.srf",
                     std::string("client_id=") + kMsaClient + "&scope=" + scope + "&response_type=device_code", reply,
                     error);
    if (http == 0)
        return fail("Microsoft couldn't be reached (" + error + "). Is the PS5 connected to the internet?");
    const std::string user_code = json_str(reply, "user_code"), device_code = json_str(reply, "device_code");
    if (http != 200 || user_code.empty() || device_code.empty())
        return fail("Microsoft didn't give a sign-in code (HTTP " + std::to_string(http) + ").");
    int interval = std::atoi(json_str(reply, "interval").c_str());
    if (interval < 1)
        interval = 5;
    int expires = std::atoi(json_str(reply, "expires_in").c_str());
    if (expires < 60)
        expires = 900;
    g_code = user_code;
    g_url = "https://www.microsoft.com/link?otc=" + user_code;
    g_phase = "code";
    step("Waiting for you to sign in");
    const std::time_t deadline = std::time(nullptr) + expires;
    const std::string form = std::string("grant_type=urn:ietf:params:oauth:grant-type:device_code&client_id=") +
                             kMsaClient + "&device_code=" + url_encode(device_code);
    while (std::time(nullptr) < deadline) {
        for (int i = 0; i < interval * 4; ++i) {
            if (cancelled()) {
                g_phase = "stopped";
                step("Sign-in stopped");
                return 1;
            }
            ::usleep(250000);
        }
        write_status();  // (the beat)
        http = post("https://login.live.com/oauth20_token.srf", form, reply, error);
        if (http == 0)
            continue;  // (a moment without the network: keep polling)
        const std::string refresh = json_str(reply, "refresh_token");
        if (http == 200 && !refresh.empty()) {
            const std::string text = "refresh_token=" + refresh + "\nuser_id=" + json_str(reply, "user_id") + "\n";
            int written = 0;
            for (const char *user : {"root", "prospero"}) {
                const std::string dir = kPrefixes + "/cd3/drive_c/users/" + user + "/AppData/Local/wineuwp";
                mkdirs(dir);
                if (write_text(dir + "/xboxauth-import.txt", text))
                    ++written;
            }
            if (!written)
                return fail("Signed in, but the sign-in couldn't be saved in Crackdown 3's prefix.");
            write_text(g_app + "/cd3/signed-in.txt", "signed in " + std::to_string(std::time(nullptr)) + "\n");
            g_phase = "done";
            g_code.clear();
            g_url.clear();
            step("Signed in: Crackdown 3 uses this account from its next start");
            return 0;
        }
        const std::string err = json_str(reply, "error");
        if (err == "authorization_pending")
            continue;
        if (err == "slow_down") {
            interval += 5;
            continue;
        }
        return fail(err == "expired_token" ? "The code expired. Start the sign-in again."
                                           : "Microsoft refused the sign-in (" + err + ").");
    }
    return fail("The code expired. Start the sign-in again.");
}

}  // namespace


/* Starting another app needs system rights: the Crackdown app itself faulted inside sceLncUtilInitialize
 * (crackdown_067, 2026-11-11), so the payload (run by the payload loader, with them) starts Prospero Win. */
extern "C" {
struct Cd3LncAppParam {
    uint32_t sz;
    int32_t user_id;
    uint32_t app_opt;
    uint64_t crash_report;
    uint32_t check_flag;
};
int sceUserServiceInitialize(void *);
int sceUserServiceGetForegroundUser(int32_t *);
int sceUserServiceGetInitialUser(int32_t *);
int sceSystemServiceLaunchApp(const char *title_id, const char *argv[], Cd3LncAppParam *param);
int sceLncUtilInitialize(void);
int sceLncUtilLaunchApp(const char *title_id, const char *argv[], Cd3LncAppParam *param);
}

#include "cd3_prompts.inc"

static int launch()
{
    /* BUTTON PROMPTS (the hub's choice, "prompts=ps|xbox" in the job): PlayStation unless it says xbox */
    prompts::apply(field(g_job, "prompts") != "xbox");
    sceUserServiceInitialize(nullptr);
    int32_t user = -1;
    if (sceUserServiceGetForegroundUser(&user) != 0 || user < 0)
        sceUserServiceGetInitialUser(&user);
    Cd3LncAppParam param{};
    param.sz = sizeof(param);
    param.user_id = user;
    const char *args[] = {"profile=cd3", nullptr};
    int rc = sceSystemServiceLaunchApp("PPSA99995", args, &param);
    log_line("launch: sceSystemServiceLaunchApp(PPSA99995, profile=cd3, user %#x) = %#x", (unsigned)user, (unsigned)rc);
    if (rc < 0) {
        const int init = sceLncUtilInitialize();
        rc = sceLncUtilLaunchApp("PPSA99995", args, &param);
        log_line("launch: sceLncUtilInitialize = %#x, sceLncUtilLaunchApp = %#x", (unsigned)init, (unsigned)rc);
    }
    if (rc < 0) {
        char text[160];
        std::snprintf(text, sizeof(text), "Prospero Win didn't start (error %08X). Start Prospero Win from the home "
                      "screen and choose Crackdown 3 there.", (unsigned)rc);
        return fail(text);
    }
    g_phase = "done";
    step("Starting Crackdown 3");
    return 0;
}

int main()
{
    umask(0);
    if (!find_app())
        return 1;
    g_dir = g_app + "/" + kJobRel;
    mkdirs(g_app + "/logs");
    g_log = g_app + "/logs/cd3.log";
    const int lock = ::open((g_dir + "/running").c_str(), O_RDWR | O_CREAT, 0666);
    if (lock < 0 || ::flock(lock, LOCK_EX | LOCK_NB) != 0)
        return 0; /* (another one has it) */
    std::string job;
    if (std::rename((g_dir + "/job.txt").c_str(), (g_dir + "/job.taken").c_str()) != 0 ||
        !read_text(g_dir + "/job.taken", job))
        return 0;
    g_token = field(job, "token");
    g_job = job;
    const std::string kind = field(job, "kind");
    log_line("job %s: %s (app %s)", g_token.c_str(), kind.c_str(), g_app.c_str());
    write_status();
    return kind == "signin" ? signin() : kind == "install" ? install() : kind == "launch" ? launch()
                                                                          : fail("unknown job " + kind);
}
