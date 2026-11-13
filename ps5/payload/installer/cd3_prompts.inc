/* Crackdown 3's PlayStation button prompts (included by cd3.cpp; GPL-3.0-or-later).
 *
 * The game draws its prompts from 53 textures in its pak (Nimbus/Content/UI/UIBlueprints/ButtonTextures, DXT5,
 * .uasset + .usbulk). UE4 mounts every *.pak in Content/Paks and a *_P.pak wins over the main one, so a small
 * Nimbus-UWP_P.pak holding those files with PlayStation pixels replaces them (seen on the PC, 2026-11-13: Cross on
 * OK/SELECT, Circle on BACK in the main menu).
 *
 * No game bytes ship with the app: cd3data/ps-prompts.bin ("CDPP" v1) holds, per file, its pak path, the
 * original's size and CRC-32, and (offset, bytes) runs of our own pixel data (drawn for this app, DXT5 at the
 * same sizes and positions). Here the player's own files are read from their pak, checked against size and CRC
 * (another game version is left alone), patched and written into the _P pak (pak format 3, entries stored).
 * BUTTON PROMPTS XBOX 360 removes the _P pak. */

#include <zlib.h>

namespace prompts {

struct Sha1 {
    std::uint32_t h[5] = {0x67452301, 0xEFCDAB89, 0x98BADCFE, 0x10325476, 0xC3D2E1F0};
    unsigned char block[64];
    std::uint64_t length = 0;
    size_t used = 0;
    static std::uint32_t rol(std::uint32_t v, int n) { return (v << n) | (v >> (32 - n)); }
    void compress()
    {
        std::uint32_t w[80];
        for (int i = 0; i < 16; i++)
            w[i] = std::uint32_t(block[4 * i]) << 24 | std::uint32_t(block[4 * i + 1]) << 16 |
                   std::uint32_t(block[4 * i + 2]) << 8 | block[4 * i + 3];
        for (int i = 16; i < 80; i++) w[i] = rol(w[i - 3] ^ w[i - 8] ^ w[i - 14] ^ w[i - 16], 1);
        std::uint32_t a = h[0], b = h[1], c = h[2], d = h[3], e = h[4];
        for (int i = 0; i < 80; i++) {
            std::uint32_t f, k;
            if (i < 20) f = (b & c) | (~b & d), k = 0x5A827999;
            else if (i < 40) f = b ^ c ^ d, k = 0x6ED9EBA1;
            else if (i < 60) f = (b & c) | (b & d) | (c & d), k = 0x8F1BBCDC;
            else f = b ^ c ^ d, k = 0xCA62C1D6;
            const std::uint32_t t = rol(a, 5) + f + e + k + w[i];
            e = d, d = c, c = rol(b, 30), b = a, a = t;
        }
        h[0] += a, h[1] += b, h[2] += c, h[3] += d, h[4] += e;
    }
    void add(const void *data, size_t n)
    {
        const unsigned char *p = static_cast<const unsigned char *>(data);
        length += n;
        while (n) {
            const size_t k = std::min(n, 64 - used);
            std::memcpy(block + used, p, k);
            used += k, p += k, n -= k;
            if (used == 64) compress(), used = 0;
        }
    }
    void finish(unsigned char out[20])
    {
        const std::uint64_t bits = length * 8;
        const unsigned char pad = 0x80, zero = 0;
        add(&pad, 1);
        while (used != 56) add(&zero, 1);
        for (int i = 7; i >= 0; i--) {
            const unsigned char b = (unsigned char)(bits >> (8 * i));
            add(&b, 1);
        }
        for (int i = 0; i < 5; i++)
            for (int j = 0; j < 4; j++) out[4 * i + j] = (unsigned char)(h[i] >> (24 - 8 * j));
    }
};

struct Reader {
    const std::string &s;
    size_t p = 0;
    bool ok = true;
    template <class T> T get()
    {
        T v{};
        if (p + sizeof(T) > s.size()) { ok = false; return v; }
        std::memcpy(&v, s.data() + p, sizeof(T));
        p += sizeof(T);
        return v;
    }
    std::string bytes(size_t n)
    {
        if (p + n > s.size()) { ok = false; return {}; }
        std::string r = s.substr(p, n);
        p += n;
        return r;
    }
    std::string fstring() /* a pak FString: int32 length incl. NUL, negative = UTF-16 */
    {
        const std::int32_t n = get<std::int32_t>();
        if (n < 0 || n > 4096) { ok = false; return {}; }
        std::string r = bytes(size_t(n));
        if (!r.empty() && r.back() == '\0') r.pop_back();
        return r;
    }
};

struct Entry {
    std::int64_t offset = 0, size = 0, usize = 0;
    std::int32_t method = 0;
    std::vector<std::pair<std::int64_t, std::int64_t>> blocks;
};

bool pread_all(int fd, std::int64_t at, size_t n, std::string &out)
{
    out.resize(n);
    size_t done = 0;
    while (done < n) {
        const ssize_t r = ::pread(fd, out.data() + done, n - done, off_t(at + std::int64_t(done)));
        if (r <= 0) return false;
        done += size_t(r);
    }
    return true;
}

/* the main pak's index entries for the names we want */
bool read_index(int fd, const std::vector<std::string> &want, std::vector<Entry> &found, std::string &why)
{
    struct stat st{};
    if (::fstat(fd, &st) != 0 || st.st_size < 45) return why = "the game's pak can't be read", false;
    std::string foot;
    if (!pread_all(fd, st.st_size - 44, 44, foot)) return why = "the game's pak can't be read", false;
    Reader f{foot};
    const std::uint32_t magic = f.get<std::uint32_t>(), version = f.get<std::uint32_t>();
    const std::int64_t index_at = f.get<std::int64_t>(), index_size = f.get<std::int64_t>();
    if (magic != 0x5A6F12E1 || version != 3 || index_size <= 0 || index_size > (64 << 20) ||
        index_at + index_size > st.st_size)
        return why = "the game's pak isn't the expected version (pak 3)", false;
    std::string index;
    if (!pread_all(fd, index_at, size_t(index_size), index)) return why = "the game's pak index can't be read", false;
    Reader r{index};
    r.fstring();
    const std::int32_t count = r.get<std::int32_t>();
    found.assign(want.size(), Entry{});
    std::vector<bool> got(want.size(), false);
    for (std::int32_t i = 0; i < count && r.ok; i++) {
        const std::string name = r.fstring();
        Entry e;
        e.offset = r.get<std::int64_t>(), e.size = r.get<std::int64_t>(), e.usize = r.get<std::int64_t>();
        e.method = r.get<std::int32_t>();
        r.bytes(20);
        if (e.method) {
            const std::int32_t n = r.get<std::int32_t>();
            if (n < 0 || n > 1 << 20) { r.ok = false; break; }
            for (std::int32_t b = 0; b < n; b++) {
                const std::int64_t a = r.get<std::int64_t>(), z = r.get<std::int64_t>();
                e.blocks.emplace_back(a, z);
            }
        }
        const std::uint8_t encrypted = r.get<std::uint8_t>();
        r.get<std::uint32_t>();
        for (size_t k = 0; k < want.size(); k++)
            if (!got[k] && name == want[k] && !encrypted) found[k] = e, got[k] = true;
    }
    if (!r.ok) return why = "the game's pak index is damaged", false;
    for (size_t k = 0; k < want.size(); k++)
        if (!got[k]) return why = "the game's pak has no " + want[k], false;
    return true;
}

bool read_entry(int fd, const Entry &e, std::string &out)
{
    if (!e.method) {
        /* stored: the data follows the entry's own header (53 bytes in pak 3 for an uncompressed entry) */
        return pread_all(fd, e.offset + 53, size_t(e.size), out);
    }
    if (e.method != 1 || e.usize <= 0 || e.usize > (64 << 20)) return false;
    out.clear();
    out.reserve(size_t(e.usize));
    std::string packed, chunk;
    for (const auto &[a, z] : e.blocks) {
        if (z <= a || !pread_all(fd, a, size_t(z - a), packed)) return false;
        chunk.resize(size_t(e.usize) - out.size());
        uLongf n = uLongf(chunk.size());
        if (uncompress(reinterpret_cast<Bytef *>(chunk.data()), &n, reinterpret_cast<const Bytef *>(packed.data()),
                       uLong(packed.size())) != Z_OK)
            return false;
        out.append(chunk.data(), n);
    }
    return std::int64_t(out.size()) == e.usize;
}

void put32(std::string &s, std::uint32_t v) { s.append(reinterpret_cast<const char *>(&v), 4); }
void put64(std::string &s, std::int64_t v) { s.append(reinterpret_cast<const char *>(&v), 8); }

std::string pak_entry(std::int64_t at, const std::string &data)
{
    std::string e;
    put64(e, at), put64(e, std::int64_t(data.size())), put64(e, std::int64_t(data.size()));
    put32(e, 0);
    unsigned char sha[20];
    Sha1 h;
    h.add(data.data(), data.size());
    h.finish(sha);
    e.append(reinterpret_cast<const char *>(sha), 20);
    e.push_back('\0');
    put32(e, 0);
    return e;
}

/* Makes <game>/Nimbus/Content/Paks/Nimbus-UWP_P.pak match the setting. Never fails the launch: a problem is
 * logged and the game starts with its own prompts. */
void apply(bool playstation)
{
    const std::string paks = g_app + "/cd3/game/Nimbus/Content/Paks";
    const std::string target = paks + "/Nimbus-UWP_P.pak", stamp = paks + "/Nimbus-UWP_P.stamp";
    const std::string blob_path = g_app + "/cd3data/ps-prompts.bin";
    if (!playstation) {
        if (exists(target)) {
            ::unlink(target.c_str());
            ::unlink(stamp.c_str());
            log_line("prompts: Xbox 360 buttons (the PlayStation pak removed)");
        }
        return;
    }
    std::string blob;
    if (!read_text(blob_path, blob) || blob.size() < 12 || blob.compare(0, 4, "CDPP") != 0) {
        log_line("prompts: %s missing or damaged: the game's own buttons", blob_path.c_str());
        return;
    }
    char want_stamp[64];
    std::snprintf(want_stamp, sizeof(want_stamp), "%zu %08lx\n", blob.size(),
                  crc32(0, reinterpret_cast<const Bytef *>(blob.data()), uInt(blob.size())));
    std::string have;
    if (exists(target) && read_text(stamp, have) && have == want_stamp) {
        log_line("prompts: PlayStation buttons (pak up to date)");
        return;
    }
    step("Putting in the PlayStation buttons");
    Reader b{blob};
    b.bytes(4);
    const std::uint32_t version = b.get<std::uint32_t>(), count = b.get<std::uint32_t>();
    if (version != 1 || count == 0 || count > 4096) {
        log_line("prompts: ps-prompts.bin version %u not understood", version);
        return;
    }
    struct Patch { std::string name; std::uint32_t size, crc; std::vector<std::pair<std::uint32_t, std::string>> runs; };
    std::vector<Patch> patches(count);
    std::vector<std::string> names;
    for (auto &p : patches) {
        const std::uint16_t n = b.get<std::uint16_t>();
        p.name = b.bytes(n);
        p.size = b.get<std::uint32_t>(), p.crc = b.get<std::uint32_t>();
        const std::uint16_t runs = b.get<std::uint16_t>();
        for (std::uint16_t r = 0; r < runs && b.ok; r++) {
            const std::uint32_t at = b.get<std::uint32_t>(), len = b.get<std::uint32_t>();
            p.runs.emplace_back(at, b.bytes(len));
        }
        names.push_back(p.name);
    }
    if (!b.ok) {
        log_line("prompts: ps-prompts.bin damaged");
        return;
    }
    const std::string main_pak = paks + "/Nimbus-UWP.pak";
    const int fd = ::open(main_pak.c_str(), O_RDONLY);
    if (fd < 0) {
        log_line("prompts: %s can't be opened (%s)", main_pak.c_str(), std::strerror(errno));
        return;
    }
    std::vector<Entry> entries;
    std::string why;
    if (!read_index(fd, names, entries, why)) {
        ::close(fd);
        log_line("prompts: %s: the game's own buttons", why.c_str());
        return;
    }
    std::string pak, index;
    index.reserve(1 << 16);
    {
        const std::string mount = "../../../";
        put32(index, std::uint32_t(mount.size() + 1));
        index.append(mount).push_back('\0');
        put32(index, count);
    }
    for (size_t k = 0; k < patches.size(); k++) {
        std::string data;
        if (!read_entry(fd, entries[k], data)) {
            ::close(fd);
            log_line("prompts: %s can't be read from the pak: the game's own buttons", patches[k].name.c_str());
            return;
        }
        const std::uint32_t crc = crc32(0, reinterpret_cast<const Bytef *>(data.data()), uInt(data.size()));
        if (data.size() != patches[k].size || crc != patches[k].crc) {
            ::close(fd);
            log_line("prompts: %s differs from the version the PlayStation buttons were made for (size %zu crc %08x, "
                     "expected %u %08x): the game's own buttons", patches[k].name.c_str(), data.size(), crc,
                     patches[k].size, patches[k].crc);
            return;
        }
        for (const auto &[at, run] : patches[k].runs) {
            if (size_t(at) + run.size() > data.size()) {
                ::close(fd);
                log_line("prompts: a patch runs past %s: the game's own buttons", patches[k].name.c_str());
                return;
            }
            std::memcpy(data.data() + at, run.data(), run.size());
        }
        const std::int64_t at = std::int64_t(pak.size());
        pak += pak_entry(0, data);
        pak += data;
        put32(index, std::uint32_t(patches[k].name.size() + 1));
        index.append(patches[k].name).push_back('\0');
        index += pak_entry(at, data);
    }
    ::close(fd);
    const std::int64_t index_at = std::int64_t(pak.size());
    pak += index;
    pak.push_back('\0');
    put32(pak, 0x5A6F12E1), put32(pak, 3);
    put64(pak, index_at), put64(pak, std::int64_t(index.size()));
    unsigned char sha[20];
    Sha1 h;
    h.add(index.data(), index.size());
    h.finish(sha);
    pak.append(reinterpret_cast<const char *>(sha), 20);
    if (!write_text(target, pak)) {  /* (written beside it, then renamed over it) */
        ::unlink((target + ".tmp").c_str());
        log_line("prompts: %s couldn't be written: the game's own buttons", target.c_str());
        return;
    }
    write_text(stamp, want_stamp);
    log_line("prompts: PlayStation buttons, %u files into %s (%zu bytes)", count, target.c_str(), pak.size());
}

}  // namespace prompts
