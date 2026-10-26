// eri_export: decode EntisGLS .eri images / ERISA .noa archives to PNG using the
// official EntisGLS4 (Sakura2) SDK decoder (SGLArchiveFile + SGLImage/SGLERImageDecoder).
//
//   eri_export list    <archive.noa>
//   eri_export extract <archive.noa> <outdir> [--only a.eri,b.inf] [--keep-eri] [--no-frames]
//   eri_export decode  <file.eri|png|bmp> <out.png>
//   eri_export info    <file.eri>
//
// extract writes <outdir>/<name>.png for every image entry (frame 0; extra frames as
// <name>@fNN.png), the raw decoded bytes for every other entry (e.g. system.inf), and
// <outdir>/manifest.json with size/format/frames/hotspot/description for each entry.
// ERISAN-compressed entries (encode type 0x80000010) are decoded by the SDK archive reader.

#include <sakuraglx/sakuraglx.h>
#include <sakuragl/sgl_erisa_lib.h>
#include <sakuragl/sgl2d_image.h>
#include <sakuragl/sgl_media.h>

#include <zlib.h>
#if defined(EXPORT_WITH_SDL_DECODER)
#include "platform/sdl/sdk_image_codec.h"
#endif
#include "platform/sdl/system.h"

#include <cstdio>
#include <cstring>
#include <fstream>
#include <memory>
#include <set>
#include <sstream>
#include <string>
#include <sys/stat.h>
#include <vector>

using namespace SakuraGL;

namespace {

std::wstring Widen(const std::string& utf8) {
    std::wstring out;
    size_t i = 0;
    while (i < utf8.size()) {
        unsigned char c = utf8[i];
        uint32_t cp; int n;
        if (c < 0x80) { cp = c; n = 1; }
        else if ((c >> 5) == 6) { cp = c & 0x1f; n = 2; }
        else if ((c >> 4) == 14) { cp = c & 0x0f; n = 3; }
        else { cp = c & 0x07; n = 4; }
        for (int k = 1; k < n && i + k < utf8.size(); ++k) cp = (cp << 6) | (utf8[i + k] & 0x3f);
        out.push_back(wchar_t(cp));
        i += n;
    }
    return out;
}

std::string Narrow(const SSystem::SString& s) {
    auto bytes = s.ToUTF8();
    std::string out(reinterpret_cast<const char*>(bytes.GetArray()), bytes.GetLength());
    while (!out.empty() && out.back() == 0) out.pop_back();
    return out;
}

std::string JsonEscape(const std::string& s) {
    std::string o;
    for (unsigned char c : s) {
        switch (c) {
        case '"': o += "\\\""; break;
        case '\\': o += "\\\\"; break;
        case '\n': o += "\\n"; break;
        case '\r': o += "\\r"; break;
        case '\t': o += "\\t"; break;
        default:
            if (c < 0x20) { char b[8]; snprintf(b, sizeof b, "\\u%04x", c); o += b; }
            else o += char(c);
        }
    }
    return o;
}

// Plain stdio-backed SDK file (read only).
class StdFile final : public SSystem::SFileInterface {
public:
    explicit StdFile(std::string path) : path_(std::move(path)) { fp_ = fopen(path_.c_str(), "rb"); }
    ~StdFile() override { if (fp_) fclose(fp_); }
    bool ok() const { return fp_ != nullptr; }
    SSystem::SFileInterface* Duplicate() const override {
        auto* f = new StdFile(path_);
        if (!f->ok()) { delete f; return nullptr; }
        fseeko(f->fp_, ftello(fp_), SEEK_SET);
        return f;
    }
    size_t Read(void* p, size_t n) override { return fread(p, 1, n, fp_); }
    size_t Write(const void*, size_t) override { return 0; }
    bool IsSeekable() const override { return true; }
    int64_t GetLength() const override {
        struct stat st{}; return fstat(fileno(fp_), &st) == 0 ? int64_t(st.st_size) : -1;
    }
    int64_t Seek(int64_t pos, SeekOrigin from) override {
        int w = from == FromBegin ? SEEK_SET : from == FromCurrent ? SEEK_CUR : SEEK_END;
        if (fseeko(fp_, pos, w) != 0) return -1;
        return ftello(fp_);
    }
    int64_t GetPosition() const override { return ftello(fp_); }
    SSystem::SError SetEndOfFile() override { return SSystem::errFailed; }
private:
    std::string path_;
    FILE* fp_ = nullptr;
};

bool ReadWholeFile(const std::string& path, std::vector<uint8_t>& out) {
    FILE* f = fopen(path.c_str(), "rb");
    if (!f) return false;
    fseeko(f, 0, SEEK_END); auto n = ftello(f); fseeko(f, 0, SEEK_SET);
    out.resize(size_t(n));
    bool ok = fread(out.data(), 1, out.size(), f) == out.size();
    fclose(f);
    return ok;
}

bool WriteWholeFile(const std::string& path, const void* data, size_t n) {
    FILE* f = fopen(path.c_str(), "wb");
    if (!f) return false;
    bool ok = fwrite(data, 1, n, f) == n;
    fclose(f);
    return ok;
}

void Put32(std::vector<uint8_t>& v, uint32_t x) {
    v.push_back(x >> 24); v.push_back(x >> 16); v.push_back(x >> 8); v.push_back(x);
}

void Chunk(std::vector<uint8_t>& png, const char* type, const std::vector<uint8_t>& data) {
    Put32(png, uint32_t(data.size()));
    size_t start = png.size();
    png.insert(png.end(), type, type + 4);
    png.insert(png.end(), data.begin(), data.end());
    uLong crc = crc32(0, png.data() + start, uInt(png.size() - start));
    Put32(png, uint32_t(crc));
}

bool WritePNG(const std::string& path, int w, int h, const std::vector<uint8_t>& rgba) {
    std::vector<uint8_t> raw;
    raw.reserve(size_t(h) * (size_t(w) * 4 + 1));
    for (int y = 0; y < h; ++y) {
        raw.push_back(0);
        raw.insert(raw.end(), rgba.begin() + size_t(y) * w * 4, rgba.begin() + size_t(y + 1) * w * 4);
    }
    uLongf zn = compressBound(uLong(raw.size()));
    std::vector<uint8_t> z(zn);
    int level = 6;
    if (const char* v = getenv("ERI_EXPORT_PNG_LEVEL")) level = std::max(0, std::min(9, atoi(v)));
    if (compress2(z.data(), &zn, raw.data(), uLong(raw.size()), level) != Z_OK) return false;
    z.resize(zn);
    std::vector<uint8_t> png = {0x89, 'P', 'N', 'G', 0x0d, 0x0a, 0x1a, 0x0a};
    std::vector<uint8_t> ihdr;
    Put32(ihdr, uint32_t(w)); Put32(ihdr, uint32_t(h));
    ihdr.push_back(8); ihdr.push_back(6); ihdr.push_back(0); ihdr.push_back(0); ihdr.push_back(0);
    Chunk(png, "IHDR", ihdr);
    Chunk(png, "IDAT", z);
    Chunk(png, "IEND", {});
    return WriteWholeFile(path, png.data(), png.size());
}

struct DecodedImage {
    int width = 0, height = 0;          // visible size
    uint32_t format = 0, depth = 0;     // SDK image format and depth
    int originX = 0, originY = 0;       // hotspot (ptOrigin)
    size_t frames = 0;
    std::string description;            // ERI description chunk (tag info)
    std::string error;
    std::vector<std::vector<uint8_t>> rgba;  // raw SDK RGB + alpha bytes, representation follows format flags
    uint32_t durationMs = 0;
    std::vector<std::pair<uint32_t, uint32_t>> sequence;
};

// Convert the selected SDK frame to RGBA8 channel order, retaining its alpha representation.
bool FrameToRGBA(SGLImage& image, std::vector<uint8_t>& out, int& w, int& h, std::string& err) {
    SGLImageInfo info;
    const uint8_t* src = image.LockBuffer(info, SGLImageObject::lockRead);
    if (!src) { err = "LockBuffer failed"; return false; }
    w = int(info.width); h = int(info.height);
    out.assign(size_t(w) * h * 4, 0);
    std::vector<SGLPalette> pal(256);
    const bool palette = (info.format & formatImageFlagPalette) != 0;
    if (palette) image.GetPaletteTable(pal.data(), 256);
    const uint32_t type = info.format & formatImageTypeMask;
    const bool alpha = (info.format & formatImageFlagAlpha) != 0;
    bool ok = true;
    for (int y = 0; y < h && ok; ++y) {
        const uint8_t* row = src + int64_t(y) * info.pitchLine;
        uint8_t* d = out.data() + size_t(y) * w * 4;
        for (int x = 0; x < w; ++x, d += 4) {
            const uint8_t* p = row + int64_t(x) * info.pitchPixel;
            if (palette && info.depth == 8) {
                const auto& c = pal[p[0]];
                const uint8_t* b = reinterpret_cast<const uint8_t*>(&c);
                d[0] = b[2]; d[1] = b[1]; d[2] = b[0]; d[3] = alpha ? b[3] : 255;
            } else if (type == formatImageGray && info.depth == 8) {
                d[0] = d[1] = d[2] = p[0]; d[3] = 255;
            } else if ((type == formatImageRGB) && (info.depth == 32 || info.depth == 24)) {
                d[0] = p[2]; d[1] = p[1]; d[2] = p[0];
                d[3] = (alpha && info.depth == 32) ? p[3] : 255;
            } else if ((type == formatImageBGR) && (info.depth == 32 || info.depth == 24)) {
                d[0] = p[0]; d[1] = p[1]; d[2] = p[2];
                d[3] = (alpha && info.depth == 32) ? p[3] : 255;
            } else { ok = false; err = "unsupported pixel format"; break; }
        }
    }
    image.UnlockBuffer(SGLImageObject::lockRead);
    return ok;
}

DecodedImage DecodeImageBytes(std::vector<uint8_t>& bytes, bool allFrames) {
    DecodedImage r;
    // Description / tag info (hotspot, reference file, ...), when the file is an ERI.
    if (bytes.size() > 8 && memcmp(bytes.data(), "Entis\x1a", 6) == 0) {
        SSystem::SMemoryReferenceFile mf;
        mf.AttachMemory(bytes.data(), bytes.size());
        ERISA::SGLMediaFile media;
        if (!media.OpenMediaFile(&mf, ERISA::SGLMediaFile::openStream)) {
            if (media.m_flagsRead & ERISA::SGLMediaFile::readDescription) r.description = Narrow(media.m_strDescription);
            r.durationMs = media.m_eriFileHeader.dwAllFrameTime;
            for (size_t i = 0; i < media.m_tableSequence.GetLength(); ++i) {
                const auto& s = media.m_tableSequence.At(i);
                r.sequence.emplace_back(s.nFrame, s.nDuration);
            }
        }
    }
    SSystem::SMemoryReferenceFile mf;
    mf.AttachMemory(bytes.data(), bytes.size());
    SGLImage image;
    if (image.ReadImage(&mf)) { r.error = "SDK ReadImage failed"; return r; }
    SGLImageInfo info;
    image.GetImageInfo(info);
    r.format = info.format; r.depth = info.depth;
    r.originX = info.ptOrigin.x; r.originY = info.ptOrigin.y;
    r.frames = image.GetFrameCount();
    if (r.frames == 0) r.frames = 1;
    const size_t n = allFrames ? r.frames : 1;
    for (size_t i = 0; i < n; ++i) {
        if (r.frames > 1) image.SelectFrame(i);
        std::vector<uint8_t> px; int w = 0, h = 0;
        if (!FrameToRGBA(image, px, w, h, r.error)) return r;
        if (i == 0) { r.width = w; r.height = h; }
        r.rgba.push_back(std::move(px));
    }
    return r;
}

void Init(const char* argv0) {
    // The SDL platform layer insists on explicit roots before SSystem starts.
    std::string root = "/tmp";
    if (const char* h = getenv("ERI_EXPORT_ROOT")) root = h;
    else {
        std::string exe = argv0; auto s = exe.rfind('/');
        if (s != std::string::npos && exe[0] == '/') root = exe.substr(0, s);
    }
    study::platform::sdl::SystemPaths paths;
    paths.storageRoot = root; paths.localRoot = root;
    study::platform::sdl::ConfigureSystemPaths(paths);
    SSystem::Initialize();
    ERISA::sclfInitializeMatrix();
    SGLImageDecoderManager::Initialzie();
#if defined(EXPORT_WITH_SDL_DECODER)
    SakuraGL::RegisterSDLImageDecoder();
#endif
}

#include <iconv.h>

std::string Cp932ToUtf8(const std::string& in) {
    iconv_t cd = iconv_open("UTF-8", "CP932");
    if (cd == (iconv_t)-1) return in;
    std::string out(in.size() * 4 + 4, '\0');
    char* src = const_cast<char*>(in.data()); size_t sl = in.size();
    char* dst = out.data(); size_t dl = out.size();
    if (iconv(cd, &src, &sl, &dst, &dl) == (size_t)-1) { iconv_close(cd); return in; }
    iconv_close(cd);
    out.resize(out.size() - dl);
    return out;
}

bool IsAscii(const std::string& s) { for (unsigned char c : s) if (c >= 0x80) return false; return true; }

struct ArcEntry {
    std::string raw;        // name bytes as stored (CP932 in these games)
    std::string name;       // UTF-8 display name
    uint64_t bytes = 0, offset = 0;  // offset is absolute
    uint32_t attr = 0, encode = 0;
};

// Root directory only (the UI archives have no sub directories).
void ListArchive(ERISA::SGLArchiveFile& arc, std::vector<ArcEntry>& files) {
    auto* dir = arc.GetCurrentDirectory();
    if (!dir) return;
    for (size_t i = 0; i < dir->GetLength(); ++i) {
        auto* fri = dir->GetAt(i);
        if (!fri || !fri->pfeEntry) continue;
        ArcEntry e;
        e.raw.assign(reinterpret_cast<const char*>(fri->pszFilename), fri->lenFilename);
        while (!e.raw.empty() && e.raw.back() == 0) e.raw.pop_back();
        e.name = (fri->pfeEntry->nAttribute & ERISA::SGLArchiveFile::attrFileNameUTF8) ? e.raw : Cp932ToUtf8(e.raw);
        e.bytes = fri->pfeEntry->nBytes;
        e.offset = dir->GetBaseFilePosition() + fri->pfeEntry->nOffsetPos;
        e.attr = fri->pfeEntry->nAttribute;
        e.encode = fri->pfeEntry->nEncodeType;
        files.push_back(e);
    }
}

// Fallback for entries whose CP932 name cannot round-trip through the SDK's
// UTF-8 lookup: read the record directly (8-byte tag + u64 length + payload).
bool ReadRawEntry(SSystem::SFileInterface& file, const ArcEntry& e, std::vector<uint8_t>& out) {
    if (file.Seek(int64_t(e.offset), SSystem::SFileInterface::FromBegin) < 0) return false;
    uint8_t hdr[16];
    if (file.Read(hdr, 16) != 16) return false;
    uint64_t len = 0; memcpy(&len, hdr + 8, 8);
    std::vector<uint8_t> payload(static_cast<size_t>(len));
    if (file.Read(payload.data(), payload.size()) != payload.size()) return false;
    if (e.encode == 0) { out = std::move(payload); out.resize(size_t(std::min<uint64_t>(e.bytes, out.size()))); return true; }
    if (e.encode != ERISA::SGLArchiveFile::encodeERISA) return false;
    SSystem::SMemoryReferenceFile mem;
    mem.AttachMemory(payload.data(), payload.size());
    ERISA::SGLDecodeBitStream bits(0x10000);
    bits.AttachInputStream(&mem);
    ERISA::SGLERISANDecodeContext dec(&bits);
    dec.PrepareToDecodeERISANCode();
    out.resize(size_t(e.bytes));
    size_t got = 0;
    while (got < out.size()) {
        size_t n = dec.Read(out.data() + got, out.size() - got);
        if (!n) break;
        got += n;
    }
    out.resize(got);
    return got == e.bytes;
}

bool ReadArchiveEntry(ERISA::SGLArchiveFile& arc, SSystem::SFileInterface& file, const ArcEntry& e,
                      std::vector<uint8_t>& out, bool& viaSdk) {
    viaSdk = false;
    if (IsAscii(e.raw) && !getenv("ERI_EXPORT_FORCE_RAW")) {
        std::wstring w = Widen(e.raw);
        if (!arc.DescendFile(w.c_str())) {
            out.clear();
            std::vector<uint8_t> buf(1 << 16);
            for (;;) {
                size_t got = arc.Read(buf.data(), buf.size());
                if (!got) break;
                out.insert(out.end(), buf.begin(), buf.begin() + got);
            }
            arc.AscendFile();
            viaSdk = true;
            return true;
        }
    }
    return ReadRawEntry(file, e, out);
}

std::string Lower(std::string s) { for (auto& c : s) c = char(tolower((unsigned char)c)); return s; }

std::string FormatName(uint32_t f, uint32_t depth) {
    std::ostringstream o;
    uint32_t t = f & formatImageTypeMask;
    o << (t == formatImageRGB ? "RGB" : t == formatImageGray ? "GRAY" : t == formatImageBGR ? "BGR" : "type?");
    if (f & formatImageFlagAlpha) o << "A";
    if (f & formatImageFlagPalette) o << "+PAL";
    o << depth;
    return o.str();
}

int CmdExtract(const std::string& noa, const std::string& outdir, const std::set<std::string>& only,
               bool keepEri, bool allFrames, int limit) {
    StdFile file(noa);
    if (!file.ok()) { fprintf(stderr, "cannot open %s\n", noa.c_str()); return 2; }
    ERISA::SGLArchiveFile arc;
    if (arc.OpenArchive(&file)) { fprintf(stderr, "OpenArchive failed: %s\n", noa.c_str()); return 2; }
    std::vector<ArcEntry> files;
    ListArchive(arc, files);
    StdFile rawFile(noa);
    mkdir(outdir.c_str(), 0755);
    std::ostringstream manifest;
    manifest << "{\n \"archive\": \"" << JsonEscape(noa) << "\",\n \"entries\": [\n";
    bool first = true;
    int done = 0, failed = 0;
    for (const auto& ent : files) {
        const std::string& name = ent.name;
        if (!only.empty() && !only.count(name) && !only.count(Lower(name))) continue;
        if (limit && done >= limit) break;
        std::vector<uint8_t> bytes;
        bool viaSdk = false;
        if (!ReadArchiveEntry(arc, rawFile, ent, bytes, viaSdk)) { fprintf(stderr, "read failed: %s\n", name.c_str()); ++failed; continue; }
        std::string stem = name, ext;
        auto dot = name.rfind('.');
        if (dot != std::string::npos) { stem = name.substr(0, dot); ext = Lower(name.substr(dot + 1)); }
        for (auto& c : stem) if (c == '/') c = '_';
        if (!first) manifest << ",\n";
        first = false;
        manifest << "  {\"name\": \"" << JsonEscape(name) << "\", \"bytes\": " << bytes.size()
                 << ", \"encode\": \"" << (ent.encode ? "ERISAN" : "raw") << "\"" << (viaSdk ? "" : ", \"read\": \"raw-fallback\"");
        const bool isImage = ext == "eri" || ext == "png" || ext == "bmp" || ext == "jpg" || ext == "tga" ||
                             (bytes.size() > 8 && memcmp(bytes.data(), "Entis\x1a", 6) == 0);
        if (isImage) {
            if (keepEri) WriteWholeFile(outdir + "/" + stem + "." + ext, bytes.data(), bytes.size());
            auto img = DecodeImageBytes(bytes, allFrames);
            if (!img.error.empty() || img.rgba.empty()) {
                fprintf(stderr, "decode failed: %s (%s)\n", name.c_str(), img.error.c_str());
                manifest << ", \"error\": \"" << JsonEscape(img.error) << "\"}";
                ++failed;
                continue;
            }
            std::string png = stem + ".png";
            WritePNG(outdir + "/" + png, img.width, img.height, img.rgba[0]);
            manifest << ", \"png\": \"" << JsonEscape(png) << "\", \"width\": " << img.width
                     << ", \"height\": " << img.height << ", \"format\": \"" << FormatName(img.format, img.depth)
                     << "\", \"frames\": " << img.frames << ", \"origin\": [" << img.originX << ", " << img.originY << "]";
            if (img.rgba.size() > 1) {
                manifest << ", \"frame_pngs\": [";
                for (size_t i = 1; i < img.rgba.size(); ++i) {
                    char suffix[16]; snprintf(suffix, sizeof suffix, "@f%02zu.png", i);
                    std::string fp = stem + suffix;
                    WritePNG(outdir + "/" + fp, img.width, img.height, img.rgba[i]);
                    manifest << (i > 1 ? ", " : "") << "\"" << JsonEscape(fp) << "\"";
                }
                manifest << "]";
            }
            if (!img.description.empty()) manifest << ", \"description\": \"" << JsonEscape(img.description) << "\"";
            manifest << "}";
        } else {
            std::string out = stem + (ext.empty() ? "" : "." + ext);
            WriteWholeFile(outdir + "/" + out, bytes.data(), bytes.size());
            manifest << ", \"file\": \"" << JsonEscape(out) << "\"}";
        }
        ++done;
    }
    manifest << "\n ]\n}\n";
    auto m = manifest.str();
    WriteWholeFile(outdir + "/manifest.json", m.data(), m.size());
    fprintf(stderr, "%s: %d extracted, %d failed\n", noa.c_str(), done, failed);
    return failed ? 1 : 0;
}

int CmdList(const std::string& noa) {
    StdFile file(noa);
    if (!file.ok()) return 2;
    ERISA::SGLArchiveFile arc;
    if (arc.OpenArchive(&file)) { fprintf(stderr, "OpenArchive failed\n"); return 2; }
    std::vector<ArcEntry> files;
    ListArchive(arc, files);
    for (auto& f : files)
        printf("%-36s %10llu attr=%08x enc=%08x off=%llx\n", f.name.c_str(), (unsigned long long)f.bytes, f.attr,
               f.encode, (unsigned long long)f.offset);
    return 0;
}

int CmdDecode(const std::string& in, const std::string& out, bool info) {
    std::vector<uint8_t> bytes;
    if (!ReadWholeFile(in, bytes)) { fprintf(stderr, "cannot read %s\n", in.c_str()); return 2; }
    auto img = DecodeImageBytes(bytes, !info);
    if (!img.error.empty()) { fprintf(stderr, "decode failed: %s\n", img.error.c_str()); return 1; }
    printf("{\"width\": %d, \"height\": %d, \"format\": \"%s\", \"frames\": %zu, \"origin\": [%d, %d], \"description\": \"%s\"}\n",
           img.width, img.height, FormatName(img.format, img.depth).c_str(), img.frames, img.originX, img.originY,
           JsonEscape(img.description).c_str());
    if (!info) {
        WritePNG(out, img.width, img.height, img.rgba[0]);
        for (size_t i = 1; i < img.rgba.size(); ++i) {
            std::string fp = out;
            auto dot = fp.rfind(".png");
            char suffix[16]; snprintf(suffix, sizeof suffix, "@f%02zu", i);
            fp.insert(dot == std::string::npos ? fp.size() : dot, suffix);
            WritePNG(fp, img.width, img.height, img.rgba[i]);
        }
    }
    return 0;
}

// A machine-generated plan contains seven tab-separated fields per line:
// encoded_sha256, original_archive, payload_offset, stored_bytes,
// decoded_bytes, archive_encoding, first_asset_id. Archive files are read-only.
// One process handles the whole plan, retaining SDK initialization overhead once.
int CmdBatch(const std::string& planPath, const std::string& outputRoot) {
    std::ifstream plan(planPath);
    if (!plan) { fprintf(stderr, "cannot open plan: %s\n", planPath.c_str()); return 2; }
    mkdir(outputRoot.c_str(), 0755);
    size_t done = 0, skipped = 0, failed = 0;
    std::string line;
    while (std::getline(plan, line)) {
        if (line.empty() || line[0] == '#') continue;
        std::vector<std::string> cols;
        std::stringstream input(line);
        std::string field;
        while (std::getline(input, field, '\t')) cols.push_back(field);
        if (cols.size() != 7 || cols[0].size() != 64 ||
            cols[0].find_first_not_of("0123456789abcdef") != std::string::npos) {
            fprintf(stderr, "bad batch row\n"); ++failed; continue;
        }
        const std::string& key = cols[0];
        const std::string outdir = outputRoot + "/" + key;
        const std::string metaPath = outdir + "/metadata.json";
        struct stat metaStat{};
        if (stat(metaPath.c_str(), &metaStat) == 0 && metaStat.st_size > 0) {
            ++skipped; continue;
        }
        const uint64_t offset = strtoull(cols[2].c_str(), nullptr, 10);
        const uint64_t stored = strtoull(cols[3].c_str(), nullptr, 10);
        const uint64_t decoded = strtoull(cols[4].c_str(), nullptr, 10);
        const uint32_t encoding = uint32_t(strtoul(cols[5].c_str(), nullptr, 10));
        StdFile source(cols[1]);
        std::vector<uint8_t> encoded(static_cast<size_t>(stored)), bytes;
        if (!source.ok() || stored > uint64_t(source.GetLength()) ||
            offset + stored > uint64_t(source.GetLength()) ||
            source.Seek(int64_t(offset), SSystem::SFileInterface::FromBegin) < 0 ||
            source.Read(encoded.data(), encoded.size()) != encoded.size()) {
            fprintf(stderr, "batch read failed: %s\n", key.c_str()); ++failed; continue;
        }
        if (encoding == 0) {
            bytes = std::move(encoded);
        } else if (encoding == ERISA::SGLArchiveFile::encodeERISA) {
            SSystem::SMemoryReferenceFile mem;
            mem.AttachMemory(encoded.data(), encoded.size());
            ERISA::SGLDecodeBitStream bits(0x10000);
            bits.AttachInputStream(&mem);
            ERISA::SGLERISANDecodeContext dec(&bits);
            dec.PrepareToDecodeERISANCode();
            bytes.resize(static_cast<size_t>(decoded));
            size_t got = 0;
            while (got < bytes.size()) {
                size_t n = dec.Read(bytes.data() + got, bytes.size() - got);
                if (!n) break;
                got += n;
            }
            bytes.resize(got);
        } else {
            fprintf(stderr, "batch unsupported encoding: %u\n", encoding); ++failed; continue;
        }
        if (bytes.size() != decoded) {
            fprintf(stderr, "batch decoded length mismatch: %s\n", key.c_str()); ++failed; continue;
        }
        auto img = DecodeImageBytes(bytes, true);
        if (!img.error.empty() || img.rgba.size() != img.frames) {
            fprintf(stderr, "batch decode failed: %s (%s)\n", key.c_str(), img.error.c_str()); ++failed; continue;
        }
        mkdir(outdir.c_str(), 0755);
        bool ok = true;
        for (size_t i = 0; i < img.rgba.size(); ++i) {
            char filename[32]; snprintf(filename, sizeof filename, "/frame%04zu.png", i);
            if (!WritePNG(outdir + filename, img.width, img.height, img.rgba[i])) { ok = false; break; }
        }
        if (!ok) { fprintf(stderr, "batch PNG write failed: %s\n", key.c_str()); ++failed; continue; }
        const bool alpha = (img.format & formatImageFlagAlpha) != 0;
        const char* alphaMode = alpha ? ((img.format & formatImageFlagNoProductOfAlpha) ? "straight" : "premultiplied") : "opaque";
        std::ostringstream meta;
        meta << "{\n \"encoded_sha256\": \"" << key << "\",\n \"first_asset_id\": \"" << JsonEscape(cols[6])
             << "\",\n \"width\": " << img.width << ",\n \"height\": " << img.height
             << ",\n \"frames\": " << img.frames << ",\n \"format\": " << img.format
             << ",\n \"depth\": " << img.depth << ",\n \"alpha_mode\": \"" << alphaMode
             << "\",\n \"pixel_policy\": \"raw SDK RGB and alpha bytes, no conversion or resampling\",\n \"origin\": ["
             << img.originX << ", " << img.originY << "],\n \"duration_ms\": " << img.durationMs << ",\n \"sequence\": [";
        for (size_t i = 0; i < img.sequence.size(); ++i) {
            meta << (i ? ", " : "") << "[" << img.sequence[i].first << ", " << img.sequence[i].second << "]";
        }
        meta << "]\n}\n";
        auto text = meta.str();
        const std::string temp = metaPath + ".partial";
        if (!WriteWholeFile(temp, text.data(), text.size()) || rename(temp.c_str(), metaPath.c_str()) != 0) {
            fprintf(stderr, "batch metadata write failed: %s\n", key.c_str()); ++failed; continue;
        }
        ++done;
        fprintf(stderr, "batch done=%zu skipped=%zu failed=%zu frames=%zu key=%s\n", done, skipped, failed, img.frames, key.c_str());
        fflush(stderr);
    }
    printf("{\"done\": %zu, \"skipped\": %zu, \"failed\": %zu}\n", done, skipped, failed);
    return failed ? 1 : 0;
}

}  // namespace

int main(int argc, char** argv) {
    if (argc < 3) {
        fprintf(stderr,
                "usage:\n  %s list <archive.noa>\n  %s extract <archive.noa> <outdir> [--only a,b] [--keep-eri] "
                "[--first-frame] [--limit N]\n  %s decode <in.eri> <out.png>\n  %s info <in.eri>\n",
                argv[0], argv[0], argv[0], argv[0]);
        return 2;
    }
    Init(argv[0]);
    std::string cmd = argv[1];
    if (cmd == "list") return CmdList(argv[2]);
    if (cmd == "info") return CmdDecode(argv[2], "", true);
    if (cmd == "decode" && argc >= 4) return CmdDecode(argv[2], argv[3], false);
    if (cmd == "batch" && argc >= 4) return CmdBatch(argv[2], argv[3]);
    if (cmd == "extract" && argc >= 4) {
        std::set<std::string> only;
        bool keep = false, all = true; int limit = 0;
        for (int i = 4; i < argc; ++i) {
            std::string a = argv[i];
            if (a == "--keep-eri") keep = true;
            else if (a == "--first-frame") all = false;
            else if (a == "--limit" && i + 1 < argc) limit = atoi(argv[++i]);
            else if (a == "--only" && i + 1 < argc) {
                std::string s = argv[++i]; size_t p = 0;
                while (p <= s.size()) {
                    size_t q = s.find(',', p); if (q == std::string::npos) q = s.size();
                    if (q > p) only.insert(s.substr(p, q - p));
                    p = q + 1;
                }
            }
        }
        return CmdExtract(argv[2], argv[3], only, keep, all, limit);
    }
    fprintf(stderr, "bad arguments\n");
    return 2;
}
