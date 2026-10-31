#pragma once
#include <cstdint>

// Optional native diagnostic. It observes the whole post-sprite target at the
// requesting TAS cursor, not the later cursor at asynchronous delivery. No CK
// objects, timing state, or snapshot-owned callbacks are retained by a request.
#if defined(__APPLE__) && defined(TAS_ENABLE_PIXEL_TRACE)
#include <CKRasterizer.h>
#include <CommonCrypto/CommonDigest.h>
#include <dlfcn.h>
#include <cstdio>
#include <cstdlib>
#include <filesystem>
#include <map>
#include <memory>
#include <string>
#include <utility>
#include <vector>

class PixelStateTrace {
    struct Output {
        std::string directory;
        FILE *log = nullptr;
        bool storePixels = true;
        ~Output() { if (log) std::fclose(log); }
    };
    struct Request {
        std::shared_ptr<Output> output;
        std::uint64_t sequence, tick, visit;
    };
    std::shared_ptr<Output> output;
    std::vector<std::pair<std::uint64_t, std::uint64_t>> ranges;
    std::map<std::uint64_t, std::uint64_t> visits;
    std::uint64_t sequence = 0;
    bool initialized = false;

    static void WriteResult(const Request &r, const VxImageDescEx *image,
                            const char *status, const char *digest = "-") {
        const auto *i = image;
        std::fprintf(r.output->log,
            "result,%llu,%llu,%llu,%s,%d,%d,%d,%d,%u,%u,%u,%u,%s\n",
            (unsigned long long)r.sequence, (unsigned long long)r.tick,
            (unsigned long long)r.visit, status,
            i ? i->Width : 0, i ? i->Height : 0, i ? i->BitsPerPixel : 0,
            i ? i->BytesPerLine : 0, i ? unsigned(i->RedMask) : 0,
            i ? unsigned(i->GreenMask) : 0, i ? unsigned(i->BlueMask) : 0,
            i ? unsigned(i->AlphaMask) : 0, digest);
        std::fflush(r.output->log);
    }

    static void Complete(void *user, const CKRECT *, VXBUFFER_TYPE buffer,
                         const VxImageDescEx *image, CKBOOL success) {
        std::unique_ptr<Request> r(static_cast<Request *>(user));
        if (!success || buffer != VXBUFFER_BACKBUFFER || !image || !image->Image) {
            WriteResult(*r, image, "readback_failed");
            return;
        }
        // Keep all four channels, including alpha, exactly as delivered. Row
        // padding is not image content; format masks are recorded separately.
        if (image->Flags || image->BitsPerPixel != 32 || image->Width <= 0 ||
            image->Height <= 0 || image->Width > 16384 || image->Height > 16384 ||
            image->BytesPerLine < image->Width * 4 || image->ColorMapEntries) {
            WriteResult(*r, image, "unsupported_format");
            return;
        }
        const std::string name = r->output->directory + "/" +
            std::to_string(r->sequence) + "-" + std::to_string(r->tick) + "-" +
            std::to_string(r->visit) + ".pixels";
        FILE *file = r->output->storePixels ? std::fopen(name.c_str(), "wbx") : nullptr;
        if (r->output->storePixels && !file) { WriteResult(*r, image, "open_failed"); return; }
        CC_SHA256_CTX hash{};
        CC_SHA256_Init(&hash);
        bool ok = true;
        for (int y = 0; y < image->Height; ++y) {
            const auto *row = image->Image + std::size_t(y) * image->BytesPerLine;
            const std::size_t bytes = std::size_t(image->Width) * 4;
            if (file && std::fwrite(row, 1, bytes, file) != bytes) { ok = false; break; }
            CC_SHA256_Update(&hash, row, static_cast<CC_LONG>(bytes));
        }
        if (file && std::fclose(file)) ok = false;
        unsigned char bytes[CC_SHA256_DIGEST_LENGTH];
        CC_SHA256_Final(bytes, &hash);
        char digest[CC_SHA256_DIGEST_LENGTH * 2 + 1];
        for (unsigned i = 0; i < sizeof(bytes); ++i)
            std::snprintf(digest + i * 2, 3, "%02x", unsigned(bytes[i]));
        WriteResult(*r, image, ok ? (r->output->storePixels ? "ok" : "ok_hash") : "write_failed", digest);
    }

    void Initialize() {
        initialized = true;
        const char *directory = std::getenv("BALLANCE_PIXEL_TRACE_DIR");
        if (!directory || !*directory) return;
        const char *selected = std::getenv("BALLANCE_PIXEL_TRACE_RANGES");
        // Explicit inclusive ranges prevent an accidental full-run disk dump.
        const char *p = selected;
        while (p && *p) {
            if (*p < '0' || *p > '9') break;
            char *end = nullptr;
            const auto first = std::strtoull(p, &end, 10);
            if (!end || *end != ':') break;
            p = end + 1;
            if (*p < '0' || *p > '9') break;
            const auto last = std::strtoull(p, &end, 10);
            if (!end || first > last || (*end && *end != ',')) break;
            ranges.emplace_back(first, last);
            p = *end ? end + 1 : end;
        }
        if (!selected || !*selected || !p || *p || ranges.empty() ||
            selected[std::char_traits<char>::length(selected) - 1] == ',') {
            std::fprintf(stderr, "PIXEL_TRACE invalid or missing ranges\n");
            ranges.clear();
            return;
        }
        // Pending readbacks can outlive mod shutdown. Hold the diagnostic code
        // resident until process exit; callbacks own their file, not this mod.
        Dl_info info{};
        if (!dladdr(reinterpret_cast<void *>(&Complete), &info) || !info.dli_fname ||
            !dlopen(info.dli_fname, RTLD_LAZY | RTLD_NOLOAD | RTLD_NODELETE)) {
            std::fprintf(stderr, "PIXEL_TRACE cannot retain callback provider\n");
            return;
        }
        std::error_code error;
        if (!std::filesystem::create_directory(directory, error)) {
            std::fprintf(stderr, "PIXEL_TRACE requires a new output directory\n");
            return;
        }
        auto candidate = std::make_shared<Output>();
        candidate->directory = directory;
        if (const char *storage = std::getenv("BALLANCE_PIXEL_TRACE_STORAGE")) {
            const std::string mode(storage);
            if (mode != "pixels" && mode != "hash") {
                std::fprintf(stderr, "PIXEL_TRACE unsupported storage mode\n");
                return;
            }
            candidate->storePixels = mode == "pixels";
        }
        candidate->log = std::fopen((candidate->directory + "/index.csv").c_str(), "wx");
        if (!candidate->log) return;
        std::fprintf(candidate->log, "# whole post-sprite target; exact packed pixel rows; no masking\n");
        std::fprintf(candidate->log, "# storage=%s\n", candidate->storePixels ? "pixels" : "hash");
        std::fflush(candidate->log);
        output = std::move(candidate);
    }
public:
    void Sample(CKRenderContext *context, std::uint64_t tick) {
        if (!initialized) Initialize();
        if (!output) return;
        bool selected = false;
        for (const auto &range : ranges)
            if (tick >= range.first && tick <= range.second) { selected = true; break; }
        if (!selected) return;
        auto *rasterizer = context ? context->GetRasterizerContext() : nullptr;
        auto r = std::make_unique<Request>(Request{output, ++sequence, tick, ++visits[tick]});
        std::fprintf(output->log, "request,%llu,%llu,%llu\n",
            (unsigned long long)r->sequence, (unsigned long long)r->tick,
            (unsigned long long)r->visit);
        std::fflush(output->log);
        if (rasterizer && rasterizer->RequestReadback(nullptr, VXBUFFER_BACKBUFFER,
                                                     &Complete, r.get())) {
            r.release();
        } else {
            WriteResult(*r, nullptr, "request_rejected");
        }
    }
};
#else
class PixelStateTrace {
public:
    template<class Context> void Sample(Context *, std::uint64_t) {}
};
#endif
