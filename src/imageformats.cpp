#include "imageformats.h"
#include <algorithm>
#include <cmath>
#include <cstring>
#include <cwctype>

namespace {

// the most pixels decoded (a damaged header mustn't ask for gigabytes)
const long long kMaxPixels = 1LL << 28;

bool size_ok(long long w, long long h) { return w > 0 && h > 0 && w <= 65535 && h <= 65535 && w * h <= kMaxPixels; }

uint32_t pm(unsigned r, unsigned g, unsigned b, unsigned a) {
    if (a == 255) return 0xFF000000u | (r << 16) | (g << 8) | b;
    r = (r * a + 127) / 255;
    g = (g * a + 127) / 255;
    b = (b * a + 127) / 255;
    return (a << 24) | (r << 16) | (g << 8) | b;
}

unsigned le16(const uint8_t* p) { return p[0] | (p[1] << 8); }
unsigned be16(const uint8_t* p) { return (p[0] << 8) | p[1]; }
uint32_t be32(const uint8_t* p) { return ((uint32_t)p[0] << 24) | ((uint32_t)p[1] << 16) | ((uint32_t)p[2] << 8) | p[3]; }

bool alloc(RawImage& out, long long w, long long h) {
    if (!size_ok(w, h)) return false;
    out.w = (int)w;
    out.h = (int)h;
    out.px.assign((size_t)(w * h), 0);
    return true;
}

} // namespace

// ------------------------------------------------------------------
// TGA
// ------------------------------------------------------------------
bool tga_decode(const uint8_t* d, size_t n, RawImage& out) {
    if (n < 18) return false;
    unsigned idLen = d[0], cmapType = d[1], type = d[2];
    unsigned cmapFirst = le16(d + 3), cmapLen = le16(d + 5), cmapBits = d[7];
    unsigned w = le16(d + 12), h = le16(d + 14), bpp = d[16], desc = d[17];
    bool rle = type >= 9;
    unsigned base = rle ? type - 8 : type;              // 1 colour-mapped, 2 true colour, 3 grey
    if (base < 1 || base > 3 || cmapType > 1) return false;
    if (base == 1 && (cmapType != 1 || bpp != 8)) return false;
    if (base == 2 && bpp != 15 && bpp != 16 && bpp != 24 && bpp != 32) return false;
    if (base == 3 && bpp != 8 && bpp != 16) return false;
    unsigned alphaBits = desc & 15;
    size_t pos = 18 + idLen;
    std::vector<uint32_t> cmap;                         // straight BGRA (alpha in the top byte)
    // a pixel of `bits` at p, straight BGRA
    auto colour = [&](const uint8_t* p, unsigned bits, bool useAlpha) -> uint32_t {
        switch (bits) {
            case 8: return 0xFF000000u | (p[0] << 16) | (p[0] << 8) | p[0];
            case 15: case 16: {
                unsigned v = le16(p);
                unsigned r = (v >> 10) & 31, g = (v >> 5) & 31, b = v & 31;
                unsigned a = bits == 16 && useAlpha ? ((v & 0x8000) ? 255 : 0) : 255;
                return (a << 24) | (((r << 3) | (r >> 2)) << 16) | (((g << 3) | (g >> 2)) << 8) | ((b << 3) | (b >> 2));
            }
            case 24: return 0xFF000000u | (p[2] << 16) | (p[1] << 8) | p[0];
            case 32: return ((uint32_t)p[3] << 24) | (p[2] << 16) | (p[1] << 8) | p[0];
        }
        return 0xFF000000u;
    };
    if (cmapType == 1) {
        unsigned eb = (cmapBits + 7) / 8;
        if (cmapBits != 15 && cmapBits != 16 && cmapBits != 24 && cmapBits != 32) return false;
        if (pos + (size_t)cmapLen * eb > n) return false;
        for (unsigned i = 0; i < cmapLen; ++i) cmap.push_back(colour(d + pos + i * eb, cmapBits, cmapBits == 32 || alphaBits));
        pos += (size_t)cmapLen * eb;
    }
    if (!alloc(out, w, h)) return false;
    unsigned pb = (bpp + 7) / 8;
    bool alphaPx = base == 2 && (bpp == 32 || (bpp == 16 && alphaBits));
    auto read_px = [&](const uint8_t* p) -> uint32_t {
        if (base == 1) {
            unsigned i = p[0] - cmapFirst;
            return i < cmap.size() ? cmap[i] : 0xFF000000u;
        }
        if (base == 3) {
            unsigned a = bpp == 16 ? p[1] : 255;
            return (a << 24) | (p[0] << 16) | (p[0] << 8) | p[0];
        }
        return colour(p, bpp, alphaPx);
    };
    std::vector<uint32_t> raw((size_t)w * h);
    size_t total = raw.size(), i = 0;
    if (!rle) {
        if (pos + total * pb > n) return false;
        for (; i < total; ++i) raw[i] = read_px(d + pos + i * pb);
    } else {
        while (i < total) {
            if (pos >= n) return false;
            unsigned hdr = d[pos++], count = (hdr & 0x7F) + 1;
            if (hdr & 0x80) {
                if (pos + pb > n) return false;
                uint32_t v = read_px(d + pos);
                pos += pb;
                for (unsigned k = 0; k < count && i < total; ++k) raw[i++] = v;
            } else {
                if (pos + (size_t)count * pb > n) return false;
                for (unsigned k = 0; k < count && i < total; ++k, pos += pb) raw[i++] = read_px(d + pos);
            }
        }
    }
    // 32-bit files that say nothing of alpha and have none: opaque
    bool useAlpha = true;
    if (base == 2 && bpp == 32 && !alphaBits)
        useAlpha = std::any_of(raw.begin(), raw.end(), [](uint32_t v) { return (v >> 24) != 0; });
    bool rightToLeft = desc & 0x10, topDown = desc & 0x20;
    for (unsigned y = 0; y < h; ++y)
        for (unsigned x = 0; x < w; ++x) {
            uint32_t v = raw[(size_t)y * w + x];
            unsigned a = useAlpha ? v >> 24 : 255;
            unsigned ty = topDown ? y : h - 1 - y, tx = rightToLeft ? w - 1 - x : x;
            out.px[(size_t)ty * w + tx] = pm((v >> 16) & 255, (v >> 8) & 255, v & 255, a);
        }
    return true;
}

// ------------------------------------------------------------------
// QOI (qoiformat.org)
// ------------------------------------------------------------------
bool qoi_decode(const uint8_t* d, size_t n, RawImage& out) {
    if (n < 14 + 8 || std::memcmp(d, "qoif", 4) != 0) return false;
    uint32_t w = be32(d + 4), h = be32(d + 8);
    unsigned channels = d[12];
    if ((channels != 3 && channels != 4) || !alloc(out, w, h)) return false;
    uint8_t index[64][4] = {};
    uint8_t px[4] = { 0, 0, 0, 255 };
    size_t pos = 14, total = (size_t)w * h, end = n - 8;
    unsigned run = 0;
    for (size_t i = 0; i < total; ++i) {
        if (run) {
            --run;
        } else {
            if (pos >= end) return false;
            unsigned b = d[pos++];
            if (b == 0xFE) {
                if (pos + 3 > end) return false;
                px[0] = d[pos]; px[1] = d[pos + 1]; px[2] = d[pos + 2];
                pos += 3;
            } else if (b == 0xFF) {
                if (pos + 4 > end) return false;
                std::memcpy(px, d + pos, 4);
                pos += 4;
            } else if ((b & 0xC0) == 0x00) {
                std::memcpy(px, index[b], 4);
            } else if ((b & 0xC0) == 0x40) {
                px[0] = (uint8_t)(px[0] + ((b >> 4) & 3) - 2);
                px[1] = (uint8_t)(px[1] + ((b >> 2) & 3) - 2);
                px[2] = (uint8_t)(px[2] + (b & 3) - 2);
            } else if ((b & 0xC0) == 0x80) {
                if (pos >= end) return false;
                unsigned b2 = d[pos++];
                int dg = (int)(b & 0x3F) - 32;
                px[0] = (uint8_t)(px[0] + dg - 8 + ((b2 >> 4) & 15));
                px[1] = (uint8_t)(px[1] + dg);
                px[2] = (uint8_t)(px[2] + dg - 8 + (b2 & 15));
            } else {
                run = b & 0x3F;                     // (this pixel and `run` more)
            }
            std::memcpy(index[(px[0] * 3 + px[1] * 5 + px[2] * 7 + px[3] * 11) % 64], px, 4);
        }
        out.px[i] = pm(px[0], px[1], px[2], channels == 4 ? px[3] : 255);
    }
    return true;
}

// ------------------------------------------------------------------
// Radiance HDR (RGBE), tone mapped for the screen
// ------------------------------------------------------------------
bool hdr_decode(const uint8_t* d, size_t n, RawImage& out) {
    if (n < 11 || (std::memcmp(d, "#?RADIANCE", 10) != 0 && std::memcmp(d, "#?RGBE", 6) != 0)) return false;
    // the header's lines up to an empty one, then the resolution line
    size_t pos = 0;
    auto line = [&](std::string& s) {
        s.clear();
        while (pos < n && d[pos] != '\n') s += (char)d[pos++];
        if (pos >= n) return false;
        ++pos;
        return true;
    };
    std::string s;
    bool xyz = false;
    for (;;) {
        if (!line(s)) return false;
        if (s.empty()) break;
        if (s.rfind("FORMAT=", 0) == 0) xyz = s.find("xyze") != std::string::npos;
    }
    if (xyz || !line(s)) return false;
    char ys = 0, xs = 0;
    long hh = 0, ww = 0;
    char ya[2] = {}, xa[2] = {};
    if (sscanf(s.c_str(), "%c%c %ld %c%c %ld", &ys, &ya[0], &hh, &xs, &xa[0], &ww) != 6 || ya[0] != 'Y' || xa[0] != 'X')
        return false;
    if (!size_ok(ww, hh)) return false;
    bool bottomUp = ys == '+', rightToLeft = xs == '-';
    std::vector<float> rgb((size_t)ww * hh * 3);
    std::vector<uint8_t> sc((size_t)ww * 4);
    for (long y = 0; y < hh; ++y) {
        if (pos + 4 > n) return false;
        if (ww >= 8 && ww < 32768 && d[pos] == 2 && d[pos + 1] == 2 && !(d[pos + 2] & 0x80)) {
            if ((long)((d[pos + 2] << 8) | d[pos + 3]) != ww) return false;
            pos += 4;
            for (int c = 0; c < 4; ++c) {                  // each component run-length coded
                long x = 0;
                while (x < ww) {
                    if (pos >= n) return false;
                    unsigned cnt = d[pos++];
                    if (cnt > 128) {
                        cnt -= 128;
                        if (pos >= n || x + (long)cnt > ww) return false;
                        uint8_t v = d[pos++];
                        for (unsigned k = 0; k < cnt; ++k) sc[(size_t)(x++) * 4 + c] = v;
                    } else {
                        if (!cnt || pos + cnt > n || x + (long)cnt > ww) return false;
                        for (unsigned k = 0; k < cnt; ++k) sc[(size_t)(x++) * 4 + c] = d[pos++];
                    }
                }
            }
        } else {                                           // flat (old run-length: 1,1,1,count repeats)
            long x = 0;
            int shift = 0;
            while (x < ww) {
                if (pos + 4 > n) return false;
                const uint8_t* p = d + pos;
                pos += 4;
                if (p[0] == 1 && p[1] == 1 && p[2] == 1 && x > 0) {
                    long cnt = (long)p[3] << shift;
                    for (long k = 0; k < cnt && x < ww; ++k, ++x) std::memcpy(&sc[(size_t)x * 4], &sc[(size_t)(x - 1) * 4], 4);
                    shift += 8;
                } else {
                    std::memcpy(&sc[(size_t)(x++) * 4], p, 4);
                    shift = 0;
                }
            }
        }
        long ty = bottomUp ? hh - 1 - y : y;
        for (long x = 0; x < ww; ++x) {
            const uint8_t* p = &sc[(size_t)x * 4];
            float f = p[3] ? std::ldexp(1.0f, (int)p[3] - 136) : 0.0f;
            float* o = &rgb[((size_t)ty * ww + (rightToLeft ? ww - 1 - x : x)) * 3];
            o[0] = p[0] * f; o[1] = p[1] * f; o[2] = p[2] * f;
        }
    }
    // a picture within the screen's range as it is; a brighter one by Reinhard's operator
    // (extended: its brightest the screen's white), its exposure from its average (log)
    // luminance
    auto lum = [](const float* c) { return 0.2126 * c[0] + 0.7152 * c[1] + 0.0722 * c[2]; };
    double logSum = 0, top = 0;
    size_t count = (size_t)ww * hh;
    for (size_t i = 0; i < count; ++i) {
        double l = lum(&rgb[i * 3]);
        logSum += std::log(1e-4 + l);
        top = std::max(top, l);
    }
    bool map = top > 1.0;
    double key = map ? 0.18 / std::exp(logSum / (double)count) : 1.0, white2 = top * key * top * key;
    auto srgb = [](double v) {
        v = v <= 0.0031308 ? 12.92 * v : 1.055 * std::pow(v, 1 / 2.4) - 0.055;
        return (unsigned)std::clamp(v * 255.0 + 0.5, 0.0, 255.0);
    };
    if (!alloc(out, ww, hh)) return false;
    for (size_t i = 0; i < count; ++i) {
        const float* c = &rgb[i * 3];
        double r = c[0] * key, g = c[1] * key, b = c[2] * key;
        double l = 0.2126 * r + 0.7152 * g + 0.0722 * b, k = map && l > 0 ? (1.0 + l / white2) / (1.0 + l) : 1.0;
        out.px[i] = pm(srgb(r * k), srgb(g * k), srgb(b * k), 255);
    }
    return true;
}

// ------------------------------------------------------------------
// PSD / PSB: the composite ("merged") image saved after the layers
// ------------------------------------------------------------------
bool psd_decode(const uint8_t* d, size_t n, RawImage& out) {
    if (n < 26 || std::memcmp(d, "8BPS", 4) != 0) return false;
    unsigned version = be16(d + 4);
    if (version != 1 && version != 2) return false;
    bool psb = version == 2;
    unsigned channels = be16(d + 12);
    uint32_t h = be32(d + 14), w = be32(d + 18);
    unsigned depth = be16(d + 22), mode = be16(d + 24);
    if (!channels || channels > 56 || (depth != 1 && depth != 8 && depth != 16 && depth != 32)) return false;
    unsigned need;                                   // channels the colour takes
    switch (mode) {
        case 0: need = 1; if (depth != 1) return false; break;     // bitmap
        case 1: case 8: need = 1; break;              // greyscale, duotone (its grey)
        case 2: need = 1; if (depth != 8) return false; break;     // indexed
        case 3: need = 3; break;                      // RGB
        case 4: need = 4; break;                      // CMYK
        default: return false;                        // (Lab, multichannel: not shown)
    }
    if (channels < need) return false;
    size_t pos = 26;
    auto skip_block = [&](bool wide, std::vector<uint8_t>* keep) {
        unsigned lb = wide ? 8 : 4;
        if (pos + lb > n) return false;
        uint64_t len = 0;
        for (unsigned i = 0; i < lb; ++i) len = (len << 8) | d[pos + i];
        pos += lb;
        if (len > n - pos) return false;
        if (keep) keep->assign(d + pos, d + pos + len);
        pos += (size_t)len;
        return true;
    };
    std::vector<uint8_t> palette;
    if (!skip_block(false, &palette) || !skip_block(false, nullptr) || !skip_block(psb, nullptr)) return false;
    if (pos + 2 > n) return false;
    unsigned compression = be16(d + pos);
    pos += 2;
    if (!alloc(out, w, h)) return false;
    size_t rowBytes = depth == 1 ? (w + 7) / 8 : (size_t)w * (depth / 8);
    // the colour channels, planar, as 8-bit values
    std::vector<std::vector<uint8_t>> planes(need, std::vector<uint8_t>((size_t)w * h));
    std::vector<uint8_t> row(rowBytes);
    auto take_row = [&](unsigned c, uint32_t y) {
        if (c >= need) return;
        uint8_t* o = &planes[c][(size_t)y * w];
        for (uint32_t x = 0; x < w; ++x) {
            switch (depth) {
                case 1: o[x] = (row[x / 8] & (0x80 >> (x % 8))) ? 0 : 255; break;      // (1: black)
                case 8: o[x] = row[x]; break;
                case 16: o[x] = row[x * 2]; break;
                case 32: {
                    uint32_t bits = be32(&row[x * 4]);
                    float f;
                    std::memcpy(&f, &bits, 4);
                    double v = std::clamp((double)f, 0.0, 1.0);
                    v = v <= 0.0031308 ? 12.92 * v : 1.055 * std::pow(v, 1 / 2.4) - 0.055;
                    o[x] = (uint8_t)(v * 255 + 0.5);
                    break;
                }
            }
        }
    };
    if (compression == 0) {
        for (unsigned c = 0; c < channels && c < need; ++c)
            for (uint32_t y = 0; y < h; ++y) {
                if (pos + rowBytes > n) return false;
                std::memcpy(row.data(), d + pos, rowBytes);
                pos += rowBytes;
                take_row(c, y);
            }
    } else if (compression == 1) {                    // PackBits, each row's length first
        unsigned cb = psb ? 4 : 2;
        size_t counts = (size_t)channels * h;
        if (pos + counts * cb > n) return false;
        const uint8_t* lens = d + pos;
        pos += counts * cb;
        for (unsigned c = 0; c < channels && c < need; ++c)
            for (uint32_t y = 0; y < h; ++y) {
                size_t k = (size_t)c * h + y;
                size_t len = psb ? be32(lens + k * 4) : be16(lens + k * 2);
                if (pos + len > n) return false;
                size_t i = pos, e = pos + len, o = 0;
                while (i < e && o < rowBytes) {
                    int8_t hdr = (int8_t)d[i++];
                    if (hdr >= 0) {
                        size_t cnt = (size_t)hdr + 1;
                        if (i + cnt > e) return false;
                        cnt = std::min(cnt, rowBytes - o);
                        std::memcpy(&row[o], d + i, cnt);
                        i += (size_t)hdr + 1;
                        o += cnt;
                    } else if (hdr != -128) {
                        size_t cnt = std::min((size_t)(1 - hdr), rowBytes - o);
                        if (i >= e) return false;
                        std::memset(&row[o], d[i++], cnt);
                        o += cnt;
                    }
                }
                if (o < rowBytes) std::memset(&row[o], 0, rowBytes - o);
                pos = e;
                take_row(c, y);
            }
    } else {
        return false;                                 // (ZIP: only in layers)
    }
    for (size_t i = 0, total = (size_t)w * h; i < total; ++i) {
        unsigned r, g, b;
        if (mode == 3) { r = planes[0][i]; g = planes[1][i]; b = planes[2][i]; }
        else if (mode == 4) {                          // (stored inverted: 255 is no ink)
            unsigned k = planes[3][i];
            r = planes[0][i] * k / 255; g = planes[1][i] * k / 255; b = planes[2][i] * k / 255;
        } else if (mode == 2) {
            unsigned v = planes[0][i];
            if (palette.size() >= 768) { r = palette[v]; g = palette[256 + v]; b = palette[512 + v]; }
            else r = g = b = v;
        } else { r = g = b = planes[0][i]; }
        out.px[i] = pm(r, g, b, 255);
    }
    return true;
}

// ------------------------------------------------------------------
// PCX
// ------------------------------------------------------------------
bool pcx_decode(const uint8_t* d, size_t n, RawImage& out) {
    if (n < 128 || d[0] != 0x0A || d[2] > 1) return false;
    unsigned bits = d[3], planes = d[65], bpl = le16(d + 66);
    int w = (int)le16(d + 8) - (int)le16(d + 4) + 1, h = (int)le16(d + 10) - (int)le16(d + 6) + 1;
    bool ok = (bits == 8 && (planes == 1 || planes == 3 || planes == 4)) || (bits == 1 && (planes == 1 || planes == 4)) ||
              (bits == 4 && planes == 1) || (bits == 2 && planes == 1);
    if (!ok || !bpl || (size_t)bpl * 8 < (size_t)w * bits || !alloc(out, w, h)) return false;
    size_t lineBytes = (size_t)bpl * planes;
    std::vector<uint8_t> line(lineBytes);
    size_t pos = 128;
    bool rle = d[2] == 1;
    // 256 colours: the palette after the image (0x0C, then 768 bytes)
    const uint8_t* pal256 = bits == 8 && planes == 1 && n >= 128 + 769 && d[n - 769] == 0x0C ? d + n - 768 : nullptr;
    for (int y = 0; y < h; ++y) {
        size_t o = 0;
        while (o < lineBytes) {
            if (pos >= n) return false;
            unsigned c = d[pos++];
            if (rle && (c & 0xC0) == 0xC0) {
                unsigned cnt = c & 0x3F;
                if (pos >= n) return false;
                unsigned v = d[pos++];
                for (unsigned k = 0; k < cnt && o < lineBytes; ++k) line[o++] = (uint8_t)v;
            } else {
                line[o++] = (uint8_t)c;
            }
        }
        uint32_t* row = &out.px[(size_t)y * w];
        for (int x = 0; x < w; ++x) {
            unsigned r, g, b, a = 255;
            if (bits == 8 && planes >= 3) {
                r = line[x]; g = line[bpl + x]; b = line[2 * bpl + x];
                if (planes == 4) a = line[3 * bpl + x];
            } else {
                unsigned idx;
                if (bits == 8) idx = line[x];
                else if (planes == 4) {                     // 16 colours as four 1-bit planes
                    idx = 0;
                    for (unsigned pl = 0; pl < 4; ++pl) idx |= ((line[pl * bpl + x / 8] >> (7 - x % 8)) & 1) << pl;
                } else {
                    unsigned perByte = 8 / bits, shift = (perByte - 1 - x % perByte) * bits;
                    idx = (line[x / perByte] >> shift) & ((1u << bits) - 1);
                }
                if (bits == 8) {
                    if (pal256) { r = pal256[idx * 3]; g = pal256[idx * 3 + 1]; b = pal256[idx * 3 + 2]; }
                    else r = g = b = idx;
                } else if (bits == 1 && planes == 1) {
                    r = g = b = idx ? 255 : 0;
                } else {                                     // the header's 16 colours
                    const uint8_t* e = d + 16 + (idx & 15) * 3;
                    r = e[0]; g = e[1]; b = e[2];
                }
            }
            row[x] = pm(r, g, b, a);
        }
    }
    return true;
}

// ------------------------------------------------------------------
// by extension
// ------------------------------------------------------------------
bool own_image_ext(const std::wstring& ext) {
    std::wstring e = ext;
    for (auto& c : e) c = (wchar_t)towlower(c);
    for (const wchar_t* k : { L".tga", L".qoi", L".hdr", L".pic", L".psd", L".psb", L".pcx" })
        if (e == k) return true;
    return false;
}

bool own_image_decode(const std::wstring& ext, const uint8_t* data, size_t size, RawImage& out) {
    std::wstring e = ext;
    for (auto& c : e) c = (wchar_t)towlower(c);
    if (e == L".tga") return tga_decode(data, size, out);
    if (e == L".qoi") return qoi_decode(data, size, out);
    if (e == L".hdr" || e == L".pic") return hdr_decode(data, size, out);
    if (e == L".psd" || e == L".psb") return psd_decode(data, size, out);
    if (e == L".pcx") return pcx_decode(data, size, out);
    return false;
}

// ------------------------------------------------------------------
// animations
// ------------------------------------------------------------------
AnimCanvas::AnimCanvas(int w, int h) : m_w(std::max(1, w)), m_h(std::max(1, h)), m_px((size_t)m_w * m_h, 0) {}

const std::vector<uint32_t>& AnimCanvas::add(const AnimFrame& f) {
    // the last frame's disposal first
    if (m_any) {
        if (m_last.dispose == AnimFrame::Clear) {
            int x0 = std::max(0, m_last.x), y0 = std::max(0, m_last.y);
            int x1 = std::min(m_w, m_last.x + m_last.w), y1 = std::min(m_h, m_last.y + m_last.h);
            for (int y = y0; y < y1; ++y) std::fill(&m_px[(size_t)y * m_w + x0], &m_px[(size_t)y * m_w + x0] + std::max(0, x1 - x0), 0u);
        } else if (m_last.dispose == AnimFrame::Restore && m_saved.size() == m_px.size()) {
            m_px = m_saved;
        }
    }
    if (f.dispose == AnimFrame::Restore) m_saved = m_px;
    if (f.px)
        for (int y = 0; y < f.h; ++y) {
            int ty = f.y + y;
            if (ty < 0 || ty >= m_h) continue;
            for (int x = 0; x < f.w; ++x) {
                int tx = f.x + x;
                if (tx < 0 || tx >= m_w) continue;
                uint32_t s = f.px[(size_t)y * f.w + x];
                uint32_t& t = m_px[(size_t)ty * m_w + tx];
                unsigned sa = s >> 24;
                if (!f.over || sa == 255) { t = s; continue; }
                if (!sa) continue;
                unsigned k = 255 - sa;                       // premultiplied "over"
                uint32_t r = 0;
                for (int sh = 0; sh < 32; sh += 8)
                    r |= (uint32_t)std::min(255u, ((s >> sh) & 255) + (((t >> sh) & 255) * k + 127) / 255) << sh;
                t = r;
            }
        }
    m_last = f;
    m_last.px = nullptr;
    m_any = true;
    return m_px;
}

namespace {

uint32_t s_crcTable[256];
bool s_crcReady = false;

uint32_t crc32(const uint8_t* p, size_t n, uint32_t c = 0) {
    if (!s_crcReady) {
        for (uint32_t i = 0; i < 256; ++i) {
            uint32_t v = i;
            for (int k = 0; k < 8; ++k) v = v & 1 ? 0xEDB88320u ^ (v >> 1) : v >> 1;
            s_crcTable[i] = v;
        }
        s_crcReady = true;
    }
    c ^= 0xFFFFFFFFu;
    for (size_t i = 0; i < n; ++i) c = s_crcTable[(c ^ p[i]) & 255] ^ (c >> 8);
    return c ^ 0xFFFFFFFFu;
}

void put32(std::vector<uint8_t>& v, uint32_t x) {
    for (int sh = 24; sh >= 0; sh -= 8) v.push_back((uint8_t)(x >> sh));
}

void put_chunk(std::vector<uint8_t>& v, const char* type, const uint8_t* data, size_t n) {
    put32(v, (uint32_t)n);
    size_t at = v.size();
    v.insert(v.end(), type, type + 4);
    if (n) v.insert(v.end(), data, data + n);
    put32(v, crc32(&v[at], n + 4));
}

} // namespace

int anim_delay(int ms) { return ms <= 10 ? 100 : ms; }

bool apng_split(const uint8_t* d, size_t n, int& canvasW, int& canvasH, std::vector<ApngFrame>& frames) {
    static const uint8_t sig[8] = { 0x89, 'P', 'N', 'G', '\r', '\n', 0x1A, '\n' };
    frames.clear();
    if (n < 8 + 25 || std::memcmp(d, sig, 8) != 0) return false;
    struct Chunk { const uint8_t* type; const uint8_t* data; uint32_t len; };
    std::vector<Chunk> chunks;
    for (size_t pos = 8; pos + 12 <= n;) {
        uint32_t len = be32(d + pos);
        if (len > n - pos - 12) return false;
        chunks.push_back({ d + pos + 4, d + pos + 8, len });
        pos += 12 + (size_t)len;
        if (!std::memcmp(d + pos - 12 - len + 4, "IEND", 4)) break;
    }
    auto is = [](const Chunk& c, const char* t) { return !std::memcmp(c.type, t, 4); };
    if (chunks.empty() || !is(chunks[0], "IHDR") || chunks[0].len != 13) return false;
    bool animated = std::any_of(chunks.begin(), chunks.end(), [&](const Chunk& c) { return is(c, "acTL"); });
    if (!animated) return false;
    canvasW = (int)be32(chunks[0].data);
    canvasH = (int)be32(chunks[0].data + 4);
    if (!size_ok(canvasW, canvasH)) return false;
    // the chunks every frame's PNG carries (palette, transparency, colour space ...)
    std::vector<uint8_t> shared;
    bool seenData = false;
    for (size_t i = 1; i < chunks.size(); ++i) {
        const Chunk& c = chunks[i];
        if (is(c, "IDAT") || is(c, "fdAT")) { seenData = true; continue; }
        if (seenData || is(c, "acTL") || is(c, "fcTL") || is(c, "IEND")) continue;
        put_chunk(shared, std::string((const char*)c.type, 4).c_str(), c.data, c.len);
    }
    ApngFrame* cur = nullptr;
    bool idatIsFrame = false;
    std::vector<uint8_t> body;
    auto finish = [&]() {
        if (!cur) return;
        std::vector<uint8_t> png(sig, sig + 8);
        uint8_t ihdr[13];
        std::memcpy(ihdr, chunks[0].data, 13);
        for (int k = 0; k < 4; ++k) {
            ihdr[k] = (uint8_t)(cur->w >> (24 - 8 * k));
            ihdr[4 + k] = (uint8_t)(cur->h >> (24 - 8 * k));
        }
        put_chunk(png, "IHDR", ihdr, 13);
        png.insert(png.end(), shared.begin(), shared.end());
        put_chunk(png, "IDAT", body.data(), body.size());
        put_chunk(png, "IEND", nullptr, 0);
        cur->png = std::move(png);
        body.clear();
        cur = nullptr;
    };
    for (size_t i = 1; i < chunks.size(); ++i) {
        const Chunk& c = chunks[i];
        if (is(c, "fcTL")) {
            if (c.len < 26) return false;
            finish();
            ApngFrame f;
            f.w = (int)be32(c.data + 4);
            f.h = (int)be32(c.data + 8);
            f.x = (int)be32(c.data + 12);
            f.y = (int)be32(c.data + 16);
            unsigned num = be16(c.data + 20), den = be16(c.data + 22);
            f.delayMs = (int)((num * 1000ull) / (den ? den : 100));
            unsigned disp = c.data[24], blend = c.data[25];
            f.dispose = disp == 1 ? AnimFrame::Clear : disp == 2 ? AnimFrame::Restore : AnimFrame::Keep;
            f.over = blend == 1;
            if (!size_ok(f.w, f.h) || f.x < 0 || f.y < 0 || f.x + f.w > canvasW || f.y + f.h > canvasH) return false;
            if (frames.empty() && f.dispose == AnimFrame::Restore) f.dispose = AnimFrame::Clear;
            frames.push_back(f);
            cur = &frames.back();
            if (frames.size() == 1) idatIsFrame = true;
        } else if (is(c, "IDAT")) {
            if (cur && idatIsFrame && frames.size() == 1) body.insert(body.end(), c.data, c.data + c.len);
        } else if (is(c, "fdAT")) {
            if (cur && c.len > 4) body.insert(body.end(), c.data + 4, c.data + c.len);
        }
    }
    finish();
    frames.erase(std::remove_if(frames.begin(), frames.end(), [](const ApngFrame& f) { return f.png.empty(); }), frames.end());
    return !frames.empty();
}

// ------------------------------------------------------------------
// SVG
// ------------------------------------------------------------------
namespace {

// a length in CSS pixels; false: none, or a percentage
bool css_length(const std::string& s, double& out) {
    char* end = nullptr;
    double v = strtod(s.c_str(), &end);
    if (end == s.c_str() || v <= 0) return false;
    std::string unit(end);
    unit.erase(std::remove_if(unit.begin(), unit.end(), [](char c) { return c == ' '; }), unit.end());
    for (auto& c : unit) c = (char)tolower((unsigned char)c);
    double k;
    if (unit.empty() || unit == "px") k = 1;
    else if (unit == "pt") k = 96.0 / 72;
    else if (unit == "pc") k = 16;
    else if (unit == "in") k = 96;
    else if (unit == "cm") k = 96 / 2.54;
    else if (unit == "mm") k = 96 / 25.4;
    else if (unit == "em") k = 16;
    else if (unit == "ex") k = 8;
    else return false;                                  // (%: the viewBox decides)
    out = v * k;
    return true;
}

bool attr(const std::string& tag, const char* name, std::string& out) {
    size_t nl = strlen(name);
    for (size_t i = 0; (i = tag.find(name, i)) != std::string::npos; i += nl) {
        if (i > 0 && !isspace((unsigned char)tag[i - 1])) continue;
        size_t j = i + nl;
        while (j < tag.size() && isspace((unsigned char)tag[j])) ++j;
        if (j >= tag.size() || tag[j] != '=') continue;
        ++j;
        while (j < tag.size() && isspace((unsigned char)tag[j])) ++j;
        if (j >= tag.size() || (tag[j] != '"' && tag[j] != '\'')) continue;
        size_t e = tag.find(tag[j], j + 1);
        if (e == std::string::npos) return false;
        out = tag.substr(j + 1, e - j - 1);
        return true;
    }
    return false;
}

} // namespace

bool svg_size(const std::string& text, double& w, double& h) {
    size_t at = 0;
    for (;;) {                                          // the first <svg ...> (not in a comment)
        at = text.find("<svg", at);
        if (at == std::string::npos) return false;
        size_t c = text.rfind("<!--", at), ce = c == std::string::npos ? 0 : text.find("-->", c);
        if (c != std::string::npos && (ce == std::string::npos || ce > at)) { at = ce == std::string::npos ? text.size() : ce; continue; }
        char after = at + 4 < text.size() ? text[at + 4] : 0;
        if (isspace((unsigned char)after) || after == '>' || after == '/') break;
        at += 4;
    }
    size_t end = text.find('>', at);
    std::string tag = text.substr(at + 4, end == std::string::npos ? std::string::npos : end - at - 4);
    std::string sw, sh, vb;
    double aw = 0, ah = 0, vw = 0, vh = 0;
    bool hw = attr(tag, "width", sw) && css_length(sw, aw), hh = attr(tag, "height", sh) && css_length(sh, ah);
    if (attr(tag, "viewBox", vb)) {
        for (auto& c : vb) if (c == ',') c = ' ';
        double x, y;
        if (sscanf(vb.c_str(), "%lf %lf %lf %lf", &x, &y, &vw, &vh) != 4 || vw <= 0 || vh <= 0) vw = vh = 0;
    }
    if (hw && hh) { w = aw; h = ah; }
    else if (hw && vw > 0) { w = aw; h = aw * vh / vw; }
    else if (hh && vw > 0) { h = ah; w = ah * vw / vh; }
    else if (vw > 0) { w = vw; h = vh; }
    else { w = hw ? aw : 300; h = hh ? ah : 150; }
    return true;
}
