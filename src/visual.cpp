// The player's visualizations: a child window (it lets the mouse through to the video area
// under it) drawn by a thread of its own. The thread renders with Direct2D on a Direct3D 11
// device into a flip-model swap chain whose waitable object paces it to the display (a
// frame per refresh, no more frames queued than one): smooth at 60 Hz and above, little
// latency. A frame: the style draws into a scene bitmap (on transparent, the previous
// frame faded in under it when the style leaves trails); a quarter-size copy of the scene,
// blurred, gives it its glow; then background, glow and scene go to the back buffer, the
// overlays (the style's name, the full-screen caption) on top. While the sound is paused
// the motion settles and the thread waits; it waits too while the window is hidden.
// The beat (the bass drum, found in the newest samples) moves and grows things, eased in
// and out; it changes their brightness only a little: nothing flashes (photosensitivity).
#include "visual.h"
#include "mediadata.h"
#include "perf.h"
#include <d2d1_1.h>
#include <d2d1_1helper.h>
#include <d2d1effects.h>
#include <d3d11.h>
#include <dxgi1_3.h>
#include <dwrite_1.h>
#include <algorithm>
#include <array>
#include <atomic>
#include <cmath>
#include <condition_variable>
#include <cstring>
#include <cwctype>
#include <deque>
#include <mutex>
#include <random>
#include <thread>

namespace {

const float kPi = 3.14159265f;
const int B = SpectrumFeed::kBands;
const UINT kSwapFlags = DXGI_SWAP_CHAIN_FLAG_FRAME_LATENCY_WAITABLE_OBJECT;
// (d2d1effects.h declares these; dxguid isn't linked)
const CLSID kClsidBlur = { 0x1feb6d69, 0x2fe6, 0x4ac9, { 0x8c, 0x58, 0x1d, 0x7f, 0x93, 0xe7, 0xa6, 0xa5 } };
const CLSID kClsidColorMatrix = { 0x921f03d6, 0x641c, 0x47df, { 0x85, 0x2d, 0xb4, 0xbb, 0x61, 0x53, 0xae, 0x11 } };

template <class T> void release(T*& p) {
    if (p) { p->Release(); p = nullptr; }
}

// --- colours ---
D2D1_COLOR_F hex(uint32_t c, float a = 1) {
    return { ((c >> 16) & 255) / 255.f, ((c >> 8) & 255) / 255.f, (c & 255) / 255.f, a };
}
D2D1_COLOR_F cref(COLORREF c, float a = 1) { return { GetRValue(c) / 255.f, GetGValue(c) / 255.f, GetBValue(c) / 255.f, a }; }
D2D1_COLOR_F mix(D2D1_COLOR_F a, D2D1_COLOR_F b, float t) {
    return { a.r + (b.r - a.r) * t, a.g + (b.g - a.g) * t, a.b + (b.b - a.b) * t, a.a + (b.a - a.a) * t };
}
D2D1_COLOR_F alpha(D2D1_COLOR_F c, float a) { c.a = a; return c; }
D2D1_COLOR_F hsv(float h, float s, float v, float a = 1) {
    h = h - std::floor(h);
    float r = std::clamp(std::fabs(h * 6 - 3) - 1, 0.f, 1.f), g = std::clamp(2 - std::fabs(h * 6 - 2), 0.f, 1.f),
          b = std::clamp(2 - std::fabs(h * 6 - 4), 0.f, 1.f);
    return { v * (1 - s + s * r), v * (1 - s + s * g), v * (1 - s + s * b), a };
}
float smooth_to(float v, float target, float dt, float tau) { return v + (target - v) * (1 - std::exp(-dt / tau)); }

// --- what the frame shows of the sound ---
struct Sound {
    float level[B]{};                   // per band, 0..1, smoothed (quick up, slower down)
    float bass = 0, mid = 0, treble = 0, energy = 0;
    float beat = 0;                     // 1 at a beat (the bass drum), then fading
    float kick = 0;                     // the pulse to move by: a beat's (quickly fading), or the bass pushing
    float snap = 0;                     // the same for the highs (snare, hi-hats)
    int beats = 0;                      // beats so far (a style's cue for something new)
    int snaps = 0;                      // the same for the highs
    std::vector<float> wave;            // the newest samples, about -1..1 (scaled to the recent peak)
    unsigned rate = 44100;
    bool quiet = true;                  // (nothing left to settle)
};

class Analyzer {
public:
    void update(const SpectrumTap& tap, LONGLONG pos, bool live, float dt, Sound& s) {
        update_bands(tap, pos, live, dt, s);
        update_wave(tap, pos, live, dt, s);
        float most = 0;
        for (float v : s.wave) most = std::max(most, std::fabs(v));
        s.quiet = !live && s.energy < 0.004f && s.beat < 0.01f && s.kick < 0.01f && s.snap < 0.01f && most < 0.01f;
    }
private:
    void update_bands(const SpectrumTap& tap, LONGLONG pos, bool live, float dt, Sound& s) {
        float raw[B];
        bool have = live && tap && tap.bands(pos, raw);
        // (a quiet track is drawn bigger: the levels scaled to the loudest band of late)
        float peak = 0, target[B];
        for (int b = 0; b < B; ++b) {
            target[b] = have ? vis_level(raw[b]) : 0.f;
            if (b >= 4 && b < 150) peak = std::max(peak, target[b]);
        }
        m_peak = std::max(peak, m_peak - dt * 0.08f);
        float gain = std::min(1.5f, 0.92f / std::max(m_peak, 0.45f));
        float sum = 0, lo = 0, mi = 0, hi = 0;
        for (int b = 0; b < B; ++b) {
            float t = std::min(1.f, target[b] * gain);
            float& l = s.level[b];
            l = smooth_to(l, t, dt, t > l ? 0.03f : 0.20f);
            sum += l;
            (b < 36 ? lo : b < 110 ? mi : hi) += l;     // (below ~160 Hz, to ~2.6 kHz, above)
        }
        s.bass = lo / 36;
        s.mid = mi / 74;
        s.treble = hi / 50;
        s.energy = sum / B;
    }
    void update_wave(const SpectrumTap& tap, LONGLONG pos, bool live, float dt, Sound& s) {
        unsigned rate = tap ? tap.rate() : 0;
        if (rate) s.rate = rate;
        s.wave.resize(4096);
        float pk = 0, kdb = -120, sdb = -120;
        if (live && tap && tap.samples(pos, s.wave.data(), s.wave.size())) {
            for (float v : s.wave) pk = std::max(pk, std::fabs(v));
            // the beats, from the newest 25 ms (the spectrum's columns are too blurred for
            // them): the bass drum below 150 Hz, the snare and hi-hats above 3 kHz
            size_t tail = s.rate / 40;
            kdb = band_db(s.wave.data(), s.wave.size(), tail, s.rate, 150, false);
            sdb = band_db(s.wave.data(), s.wave.size(), tail, s.rate, 3000, true);
        } else {
            for (float& v : s.wave) v *= std::exp(-dt / 0.15f);    // (paused: the line flattens)
        }
        if (m_kick.update(kdb, dt)) {
            s.beat = 1;
            m_kickHit = 1;
            ++s.beats;
        } else {
            s.beat *= std::exp(-dt / 0.25f);
            m_kickHit *= std::exp(-dt / 0.16f);
        }
        if (m_snap.update(sdb, dt)) {
            m_snapHit = 1;
            ++s.snaps;
        } else {
            m_snapHit *= std::exp(-dt / 0.1f);
        }
        // (eased in and out: it moves things, never flashes)
        float kick = std::max(m_kickHit, 0.8f * m_kick.punch()), snap = std::max(m_snapHit, 0.6f * m_snap.punch());
        s.kick = smooth_to(s.kick, kick, dt, kick > s.kick ? 0.04f : 0.25f);
        s.snap = smooth_to(s.snap, snap, dt, snap > s.snap ? 0.04f : 0.2f);
        m_wavePeak = std::max({ pk, m_wavePeak * std::exp(-dt / 1.5f), 0.05f });
        if (pk > 0)
            for (float& v : s.wave) v /= m_wavePeak;
    }
    float m_peak = 0.5f, m_wavePeak = 0.2f, m_kickHit = 0, m_snapHit = 0;
    BeatDetector m_kick, m_snap;
};

// --- a frame's drawing context ---
struct Frame {
    ID2D1DeviceContext* dc;
    ID2D1Factory1* factory;
    ID2D1SolidColorBrush* brush;        // (any colour: SetColor)
    ID2D1StrokeStyle* round;            // round caps and joins
    float w, h, s;                      // size (px), the DPI's scale
    float zoom;                         // lines' widths on a big area (full screen): up to 2.5 times
    float dt;                           // seconds since the last frame
    double t;                           // seconds of motion (since the thread started)
    bool dark;                          // dark background (bright, glowing colours) or light
    bool full;                          // full screen (on black) or in the window (on its background)
    D2D1_COLOR_F bg;
    const Sound& snd;
    void color(D2D1_COLOR_F c) const { brush->SetColor(c); }
};

// a path through the points (straight lines), open or closed, filled or not
ID2D1PathGeometry* make_path(ID2D1Factory1* f, const std::vector<D2D1_POINT_2F>& p, bool closed, bool filled = false) {
    ID2D1PathGeometry* g = nullptr;
    ID2D1GeometrySink* sink = nullptr;
    if (p.size() < 2 || FAILED(f->CreatePathGeometry(&g))) return nullptr;
    if (FAILED(g->Open(&sink))) { g->Release(); return nullptr; }
    sink->BeginFigure(p[0], filled ? D2D1_FIGURE_BEGIN_FILLED : D2D1_FIGURE_BEGIN_HOLLOW);
    sink->AddLines(p.data() + 1, (UINT32)(p.size() - 1));
    sink->EndFigure(closed ? D2D1_FIGURE_END_CLOSED : D2D1_FIGURE_END_OPEN);
    sink->Close();
    sink->Release();
    return g;
}

// a line through the points, its joins rounded (one path: no overlaps at the joins)
void stroke(Frame& f, const std::vector<D2D1_POINT_2F>& p, float width) {
    if (ID2D1PathGeometry* g = make_path(f.factory, p, false)) {
        f.dc->DrawGeometry(g, f.brush, width, f.round);
        g->Release();
    }
}

// a smooth curve (Catmull-Rom) through the points, as points about `step` apart (closed:
// back to the first)
void smooth_curve(const std::vector<D2D1_POINT_2F>& p, bool closed, float step, std::vector<D2D1_POINT_2F>& out) {
    out.clear();
    size_t n = p.size();
    if (n < 2) { out = p; return; }
    auto at = [&](long k) {
        if (closed) return p[(size_t)((k % (long)n + (long)n) % (long)n)];
        return p[(size_t)std::clamp(k, 0L, (long)n - 1)];
    };
    out.push_back(p[0]);
    size_t segs = closed ? n : n - 1;
    for (size_t i = 0; i < segs; ++i) {
        D2D1_POINT_2F p0 = at((long)i - 1), p1 = at((long)i), p2 = at((long)i + 1), p3 = at((long)i + 2);
        int steps = std::clamp((int)(std::hypot(p2.x - p1.x, p2.y - p1.y) / step), 1, 12);
        for (int k = 1; k <= steps; ++k) {
            float t = (float)k / steps, t2 = t * t, t3 = t2 * t;
            auto cr = [&](float a, float b, float c, float d) {
                return 0.5f * (2 * b + (c - a) * t + (2 * a - 5 * b + 4 * c - d) * t2 + (3 * b - a - 3 * c + d) * t3);
            };
            out.push_back({ cr(p0.x, p1.x, p2.x, p3.x), cr(p0.y, p1.y, p2.y, p3.y) });
        }
    }
}

// points joined by straight lines, each drawn on its own: no path to widen and tessellate
// (the costly part of a long curve on a big screen)
void draw_lines(Frame& f, const std::vector<D2D1_POINT_2F>& p, float width) {
    for (size_t i = 1; i < p.size(); ++i) f.dc->DrawLine(p[i - 1], p[i], f.brush, width);
}

// a smooth curve through the points, as short straight lines
void stroke_lines(Frame& f, const std::vector<D2D1_POINT_2F>& p, bool closed, float width) {
    static thread_local std::vector<D2D1_POINT_2F> curve;
    smooth_curve(p, closed, 10 * f.s, curve);
    draw_lines(f, curve, width);
}

ID2D1GradientStopCollection* stops(ID2D1DeviceContext* dc, std::initializer_list<D2D1_GRADIENT_STOP> s) {
    ID2D1GradientStopCollection* c = nullptr;
    static_cast<ID2D1RenderTarget*>(dc)->CreateGradientStopCollection(s.begin(), (UINT)s.size(), D2D1_GAMMA_2_2,
                                                                        D2D1_EXTEND_MODE_CLAMP, &c);
    return c;
}

ID2D1LinearGradientBrush* linear(ID2D1DeviceContext* dc, std::initializer_list<D2D1_GRADIENT_STOP> s) {
    ID2D1GradientStopCollection* c = stops(dc, s);
    ID2D1LinearGradientBrush* b = nullptr;
    if (c) dc->CreateLinearGradientBrush(D2D1::LinearGradientBrushProperties({ 0, 0 }, { 1, 0 }), c, &b);
    release(c);
    return b;
}

ID2D1RadialGradientBrush* radial(ID2D1DeviceContext* dc, std::initializer_list<D2D1_GRADIENT_STOP> s) {
    ID2D1GradientStopCollection* c = stops(dc, s);
    ID2D1RadialGradientBrush* b = nullptr;
    if (c) dc->CreateRadialGradientBrush(D2D1::RadialGradientBrushProperties({ 0, 0 }, { 0, 0 }, 1, 1), c, &b);
    release(c);
    return b;
}

// ------------------------------------------------------------------
// the styles
// ------------------------------------------------------------------
struct Style {
    virtual ~Style() = default;
    virtual void lost() {}                              // the device's resources go
    virtual void backdrop(Frame&) {}                    // on the back buffer, under the glow
    virtual void draw(Frame&) = 0;                      // into the scene
    virtual float trail() const { return 0; }           // how much of the previous frame stays
    virtual D2D1_MATRIX_3X2_F trail_move(Frame&) { return D2D1::IdentityMatrix(); }
    virtual float glow() const { return 0.8f; }         // its strength (1: as bright as the scene)
    virtual float glow_radius() const { return 9; }     // (at 96 dpi)
    virtual float pump() const { return 0.2f; }         // the glow's more at a beat: by this much (a little)
};

// --- bars from the middle, up and down; red to green from the bass up; their peaks fall ---
struct Bars : Style {
    std::vector<float> v, peak, hold, fall;
    ID2D1LinearGradientBrush* grad = nullptr;
    bool gradDark = false;
    void lost() override { release(grad); }
    float glow() const override { return 0.5f; }
    float pump() const override { return 0.3f; }
    void draw(Frame& f) override {
        const Sound& snd = f.snd;
        float pitch = 11 * f.s;
        int n = std::clamp((int)(f.w / pitch), 12, 160);
        pitch = f.w / n;
        v.resize(n);
        peak.resize(n);
        hold.resize(n);
        fall.resize(n);
        vis_bars(snd.level, 6, 150, v.data(), n);       // (~50 Hz to ~13 kHz)
        if (!grad || gradDark != f.dark) {
            release(grad);
            gradDark = f.dark;
            float k = f.dark ? 1.f : 0.8f;
            auto c = [&](uint32_t x) { D2D1_COLOR_F col = hex(x); return D2D1_COLOR_F{ col.r * k, col.g * k, col.b * k, 1 }; };
            grad = linear(f.dc, { { 0, c(0xFF5A5A) }, { 0.35f, c(0xFFA040) }, { 0.55f, c(0xFFE84A) }, { 1, c(0x3CF03C) } });
            if (!grad) return;
        }
        grad->SetStartPoint({ 0, 0 });
        grad->SetEndPoint({ f.w, 0 });
        float bw = std::max(2.f, pitch * 0.55f), mid = f.h / 2, gap = 3 * f.s, most = mid - bw / 2 - gap - bw * 0.7f;
        float lift = 0.88f + 0.22f * snd.kick;          // (the bass drum: all of them taller a moment)
        for (int k = 0; k < n; ++k) {
            v[k] = std::min(1.f, v[k] * lift);
            float x = (k + 0.5f) * pitch, hh = std::max(v[k] * most, 0.75f * f.s);
            f.dc->DrawLine({ x, mid - hh }, { x, mid + hh }, grad, bw, f.round);
            // its peak: held a moment, then falling faster and faster
            if (v[k] >= peak[k]) {
                peak[k] = v[k];
                hold[k] = 0.3f;
                fall[k] = 0;
            } else if ((hold[k] -= f.dt) < 0) {
                fall[k] += f.dt * 2.2f;
                peak[k] = std::max(v[k], peak[k] - fall[k] * f.dt);
            }
        }
        f.color(f.dark ? hex(0xFFFFFF, 0.85f) : hex(0x303030, 0.7f));
        for (int k = 0; k < n; ++k) {
            if (peak[k] < 0.03f) continue;
            float x = (k + 0.5f) * pitch, y = peak[k] * most + bw / 2 + gap + bw * 0.3f;
            f.dc->DrawLine({ x - bw * 0.2f, mid - y }, { x + bw * 0.2f, mid - y }, f.brush, bw * 0.5f, f.round);
            f.dc->DrawLine({ x - bw * 0.2f, mid + y }, { x + bw * 0.2f, mid + y }, f.brush, bw * 0.5f, f.round);
        }
    }
};

// --- the waveform, glowing, its echoes spreading out behind it ---
struct Waveform : Style {
    std::vector<D2D1_POINT_2F> pts;
    float trail() const override { return 0.78f; }
    D2D1_MATRIX_3X2_F trail_move(Frame& f) override {
        return D2D1::Matrix3x2F::Scale(1, 1.012f + 0.06f * f.snd.kick, { f.w / 2, f.h / 2 });
    }
    float glow() const override { return 1.1f; }
    float pump() const override { return 0.25f; }
    void draw(Frame& f) override {
        const Sound& snd = f.snd;
        const std::vector<float>& w = snd.wave;
        int N = (int)w.size();
        int show = std::clamp((int)(snd.rate * 0.035), 256, N / 2);
        // (it starts at a rising zero crossing, so a steady tone stands still)
        int start = N - show, best = -1;
        float steepest = 0;
        for (int i = std::max(1, N - show - N / 3); i < N - show; ++i)
            if (w[i - 1] < 0 && w[i] >= 0 && w[i] - w[i - 1] > steepest) { steepest = w[i] - w[i - 1]; best = i; }
        if (best > 0) start = best;
        int P = std::clamp((int)(f.w / (3 * f.s)), 48, 480);
        float mid = f.h / 2, amp = f.h * 0.36f * (1 + 0.15f * snd.kick);
        pts.resize(P);
        for (int k = 0; k < P; ++k) {
            int a = start + show * k / P, b = std::max(a + 1, start + show * (k + 1) / P);
            float v = 0;                                // (the bucket's biggest swing, its sign kept)
            for (int i = a; i < b && i < N; ++i) if (std::fabs(w[i]) > std::fabs(v)) v = w[i];
            pts[k] = { f.w * k / (P - 1), mid - std::clamp(v, -1.25f, 1.25f) * amp };
        }
        f.color(f.dark ? hex(0xFFFFFF, 0.07f) : hex(0x000000, 0.06f));
        f.dc->DrawLine({ 0, mid }, { f.w, mid }, f.brush, 1 * f.s);
        // at a beat: thicker, a little warmer
        f.color(f.dark ? mix(hex(0xFF7A38), hex(0xFFB060), 0.4f * snd.kick) : mix(hex(0xE0561A), hex(0xC03A10), 0.5f * snd.kick));
        stroke(f, pts, 2.2f * f.s * f.zoom * (1 + 0.4f * snd.kick));
    }
};

// --- bars around a glowing core, in the colours of the rainbow; the core beats, waves go out ---
struct Circle : Style {
    std::vector<float> v;
    float spin = 0, swirl = 0;
    int seen = 0;
    struct Wave { float r, a, hue; };
    std::vector<Wave> waves;
    ID2D1RadialGradientBrush* core = nullptr;
    void lost() override { release(core); }
    float trail() const override { return 0.5f; }
    D2D1_MATRIX_3X2_F trail_move(Frame& f) override {   // (its echoes go out)
        float k = 1.02f + 0.035f * f.snd.kick;
        return D2D1::Matrix3x2F::Scale(k, k, { f.w / 2, f.h / 2 });
    }
    float glow() const override { return 0.7f; }
    float pump() const override { return 0.25f; }
    void draw(Frame& f) override {
        const Sound& snd = f.snd;
        const int n = 128;
        v.resize(n);
        vis_bars(snd.level, 4, 144, v.data(), n);
        float cx = f.w / 2, cy = f.h / 2, m = std::min(f.w, f.h);
        float r0 = m * 0.17f * (1 + 0.24f * snd.kick + 0.06f * snd.bass), len = m * 0.25f * (1 + 0.2f * snd.kick);
        float hue0 = 0.07f + (float)f.t * 0.015f;
        if (snd.beats != seen) {                        // a beat: a wave goes out, the bars swirl
            seen = snd.beats;
            swirl += 0.9f;
            waves.push_back({ r0, 0.6f, hue0 + 0.5f * (seen % 2) });
        }
        swirl *= std::exp(-f.dt / 0.45f);
        spin += f.dt * (0.04f + 0.3f * snd.energy + swirl);
        for (Wave& w : waves) {
            w.r += f.dt * m * (0.35f + 0.8f * w.a);
            w.a *= std::exp(-f.dt / 0.45f);
        }
        waves.erase(std::remove_if(waves.begin(), waves.end(), [&](const Wave& w) { return w.a < 0.03f || w.r > m; }), waves.end());
        for (const Wave& w : waves) {
            f.color(hsv(w.hue, f.dark ? 0.45f : 0.8f, f.dark ? 1.f : 0.75f, w.a * 0.8f));
            f.dc->DrawEllipse(D2D1::Ellipse({ cx, cy }, w.r, w.r), f.brush, (1 + 4 * w.a) * f.s * std::sqrt(f.zoom));
        }
        if (!core) core = radial(f.dc, { { 0, hex(0xFF5A28) }, { 0.45f, hex(0xB0605A, 0.75f) }, { 0.8f, hex(0x503050, 0.35f) },
                                         { 1, hex(0x503050, 0) } });
        if (core) {
            core->SetCenter({ cx, cy });
            core->SetRadiusX(r0 * 0.95f);
            core->SetRadiusY(r0 * 0.95f);
            core->SetOpacity(0.55f + 0.3f * std::min(1.f, std::max(snd.kick, snd.bass * 0.8f)));
            f.dc->FillEllipse(D2D1::Ellipse({ cx, cy }, r0 * 0.95f, r0 * 0.95f), core);
        }
        float bw = std::max(1.5f * f.s, 2 * kPi * r0 / n * 0.5f);
        for (int k = 0; k < n; ++k) {
            float a = -kPi / 2 + spin + 2 * kPi * k / n;
            float L = r0 * 0.04f + v[k] * len;
            float c = std::cos(a), s = std::sin(a), r = r0 + bw;
            f.color(hsv(hue0 + (float)k / n, f.dark ? 0.72f : 0.85f, f.dark ? 1.f : 0.85f));
            f.dc->DrawLine({ cx + c * r, cy + s * r }, { cx + c * (r + L), cy + s * (r + L) }, f.brush, bw, f.round);
        }
    }
};

// --- a cloud of glowing points, each a band's, linked to its near neighbours; it turns ---
struct Particles : Style {
    struct P { float x, y, z; int band, color; float size, phase; };
    struct Star { float x, y, b, phase; };
    std::vector<P> ps;
    std::vector<Star> stars;
    struct Shown { float x, y, r, a, lvl; };
    std::vector<Shown> shown;
    ID2D1Bitmap1* dot[4]{};
    float yaw = 0, swirl = 0;
    int seen = 0;
    void lost() override { for (auto*& d : dot) release(d); }
    float glow() const override { return 0.5f; }
    float glow_radius() const override { return 7; }
    float pump() const override { return 0.25f; }
    const uint32_t colors[2][4] = { { 0x1898B0, 0xC89010, 0x50607A, 0x23955A },     // (on light)
                                    { 0x7FE8F0, 0xF3EB8F, 0xFFFFFF, 0x8FE0B0 } };  // (on dark)
    bool dotsDark = false;
    void init() {
        std::mt19937 rng(7);
        std::normal_distribution<float> g(0, 1);
        std::uniform_real_distribution<float> u(0, 1);
        const float centres[3][3] = { { -0.72f, 0.2f, 0.0f }, { 0.68f, -0.25f, 0.1f }, { 0.0f, 0.02f, -0.1f } };
        for (int i = 0; i < 280; ++i) {
            const float* c = centres[i % 3];
            float spread = i % 3 == 2 ? 0.2f : 0.3f;
            P p{ c[0] + g(rng) * spread * 1.1f, c[1] + g(rng) * spread * 0.75f, c[2] + g(rng) * spread, 0, 0, 0, 0 };
            p.band = std::clamp((int)(std::pow(u(rng), 1.3f) * 140) + 4, 4, 150);
            p.color = (int)(u(rng) * 4) & 3;
            p.size = 0.5f + u(rng);
            p.phase = u(rng) * 6.28f;
            ps.push_back(p);
        }
        for (int i = 0; i < 140; ++i) stars.push_back({ u(rng), u(rng), u(rng), u(rng) * 6.28f });
    }
    void make_dots(Frame& f) {
        ID2D1Image* was = nullptr;
        f.dc->GetTarget(&was);
        const int S = 64;
        for (int i = 0; i < 4; ++i) {
            D2D1_BITMAP_PROPERTIES1 bp = D2D1::BitmapProperties1(D2D1_BITMAP_OPTIONS_TARGET,
                                                                D2D1::PixelFormat(DXGI_FORMAT_B8G8R8A8_UNORM, D2D1_ALPHA_MODE_PREMULTIPLIED));
            if (FAILED(f.dc->CreateBitmap(D2D1::SizeU(S, S), nullptr, 0, &bp, &dot[i]))) break;
            D2D1_COLOR_F c = hex(colors[f.dark][i]);
            ID2D1RadialGradientBrush* rb = radial(f.dc, { { 0, alpha(c, 1) }, { 0.25f, alpha(c, 0.85f) }, { 0.55f, alpha(c, 0.3f) }, { 1, alpha(c, 0) } });
            f.dc->SetTarget(dot[i]);
            f.dc->Clear({ 0, 0, 0, 0 });
            if (rb) {
                rb->SetCenter({ S / 2.f, S / 2.f });
                rb->SetRadiusX(S / 2.f);
                rb->SetRadiusY(S / 2.f);
                f.dc->FillRectangle(D2D1::RectF(0, 0, S, S), rb);
                rb->Release();
            }
        }
        f.dc->SetTarget(was);
        release(was);
    }
    void draw(Frame& f) override {
        if (ps.empty()) init();
        if (!dot[0] || dotsDark != f.dark) {
            lost();
            dotsDark = f.dark;
            make_dots(f);
        }
        const Sound& snd = f.snd;
        if (f.dark) {                                   // (far stars, twinkling with the highs)
            for (const Star& st : stars) {
                float a = 0.15f + 0.25f * st.b + (0.3f * snd.treble + 0.1f * snd.snap) * (0.5f + 0.5f * std::sin((float)f.t * 2.5f + st.phase));
                f.color(hex(0x59E07A, std::min(a, 0.85f)));
                float r = 1.1f * f.s * std::sqrt(f.zoom);
                f.dc->FillEllipse(D2D1::Ellipse({ st.x * f.w, st.y * f.h }, r, r), f.brush);
            }
        }
        if (snd.beats != seen) { seen = snd.beats; swirl += 0.5f; }
        swirl *= std::exp(-f.dt / 0.5f);
        yaw += f.dt * (0.05f + 0.35f * snd.energy + swirl);
        float tilt = 0.25f * std::sin((float)f.t * 0.07f);
        float cy_ = std::cos(yaw), sy_ = std::sin(yaw), ct = std::cos(tilt), st = std::sin(tilt);
        // (a beat: the cloud bursts out a little, the points flare)
        float cx = f.w / 2, cy = f.h / 2, scale = std::min(f.w * 0.5f, f.h * 0.85f) * (1 + 0.16f * snd.kick);
        shown.resize(ps.size());
        for (size_t i = 0; i < ps.size(); ++i) {
            const P& p = ps[i];
            float x = p.x * cy_ + p.z * sy_, z = -p.x * sy_ + p.z * cy_;
            float y = p.y * ct - z * st;
            z = p.y * st + z * ct;
            float k = 2.2f / (2.2f + z);                // (nearer: bigger)
            float lvl = snd.level[p.band];
            float r = (0.8f + 1.0f * p.size + 4.5f * p.size * lvl * lvl) * (1 + 0.3f * snd.kick) * f.s * f.zoom * k;
            float a = std::clamp((0.25f + 0.65f * lvl + 0.1f * snd.kick) * (0.5f + 0.5f * k), 0.f, 1.f);
            shown[i] = { cx + x * k * scale * 0.8f, cy + y * k * scale * 0.55f, r, a, lvl };
        }
        // links between near, lively points
        f.color(f.dark ? hex(0x9FE0A0) : hex(0x2E8A5A));
        int links = 0;
        float reach = 0.08f * (1 + 0.6f * snd.kick);
        for (size_t i = 0; i < ps.size() && links < 900; ++i) {
            if (shown[i].lvl < 0.25f) continue;
            for (size_t j = i + 1; j < ps.size(); ++j) {
                float dx = ps[i].x - ps[j].x, dy = ps[i].y - ps[j].y, dz = ps[i].z - ps[j].z;
                float d2 = dx * dx + dy * dy + dz * dz;
                if (d2 > reach || shown[j].lvl < 0.25f) continue;
                float a = (1 - std::sqrt(d2 / reach)) * std::min(shown[i].lvl, shown[j].lvl) * (0.55f + 0.15f * snd.kick);
                f.brush->SetOpacity(a);
                f.dc->DrawLine({ shown[i].x, shown[i].y }, { shown[j].x, shown[j].y }, f.brush, 0.7f * f.s * std::sqrt(f.zoom));
                ++links;
            }
        }
        f.brush->SetOpacity(1);
        for (size_t i = 0; i < ps.size(); ++i) {
            const Shown& s = shown[i];
            ID2D1Bitmap1* d = dot[ps[i].color];
            if (!d) continue;
            D2D1_RECT_F r = D2D1::RectF(s.x - s.r * 2, s.y - s.r * 2, s.x + s.r * 2, s.y + s.r * 2);
            f.dc->DrawBitmap(d, &r, s.a, D2D1_INTERPOLATION_MODE_LINEAR, nullptr, nullptr);
        }
    }
};

// --- strands of light crossing in the middle, swaying with the sound ---
struct Ribbons : Style {
    float sm[3][56]{};
    float phase = 0;
    std::vector<D2D1_POINT_2F> pts;
    float trail() const override { return 0.5f; }
    float glow() const override { return 1.3f; }
    float glow_radius() const override { return 11; }
    float pump() const override { return 0.15f; }
    void draw(Frame& f) override {
        const Sound& snd = f.snd;
        const int M = 56;
        const std::vector<float>& w = snd.wave;
        int N = (int)w.size();
        phase += f.dt * (0.5f + 1.6f * snd.energy + 3.0f * snd.kick);
        // each ribbon: the waveform smoothed to M points (from a stretch of its own)
        for (int r = 0; r < 3; ++r) {
            int span = N / 2, from = N - span - r * N / 8, per = std::max(1, span / M);
            for (int i = 0; i < M; ++i) {
                float acc = 0;
                for (int k = 0; k < per; ++k) acc += w[std::clamp(from + i * per + k, 0, N - 1)];
                sm[r][i] = smooth_to(sm[r][i], std::clamp(acc / per * 2.5f, -1.f, 1.f), f.dt, 0.07f);
            }
        }
        const float angles[3] = { 0.5f, -0.5f, 0.0f };
        const uint32_t cols[3] = { 0xF7EA48, 0x3FF0F0, 0xFF4FA8 };
        const float drive[3] = { snd.bass + 0.6f * snd.kick, snd.treble * 1.6f + 0.5f * snd.snap, snd.mid * 1.2f };
        float cx = f.w / 2, cy = f.h / 2, L = std::hypot(f.w, f.h) * 1.05f, A = f.h * 0.2f * (1 + 0.3f * snd.kick);
        f.dc->SetPrimitiveBlend(D2D1_PRIMITIVE_BLEND_ADD);
        pts.resize(M);
        for (int r = 0; r < 3; ++r) {
            float ca = std::cos(angles[r]), sa = std::sin(angles[r]);
            D2D1_COLOR_F c = hex(cols[r]);
            if (!f.dark) c = { c.r * 0.75f, c.g * 0.75f, c.b * 0.75f, 1 };
            for (int j = 0; j < 5; ++j) {
                for (int i = 0; i < M; ++i) {
                    float u = (float)i / (M - 1) * 2 - 1;
                    float spread = 0.12f + 0.88f * u * u;           // (tight in the middle, wide at the ends)
                    float d = A * spread * (sm[r][i] * (0.5f + drive[r]) * (1 + 0.15f * j) +
                                            0.4f * std::sin(u * 3.2f + phase * (1 + 0.17f * j) + j * 1.3f + r * 2.1f)) +
                              (j - 2) * 7 * f.s * spread * (1 + snd.kick);
                    float along = u * L / 2;
                    pts[i] = { cx + ca * along - sa * d, cy + sa * along + ca * d };
                }
                f.color(alpha(c, std::min(1.f, 0.55f + 0.1f * (j == 2) + 0.1f * snd.kick)));
                stroke_lines(f, pts, false, (j == 2 ? 2.0f : 1.4f) * f.s * f.zoom * (1 + 0.4f * snd.kick));
            }
        }
        f.dc->SetPrimitiveBlend(D2D1_PRIMITIVE_BLEND_SOURCE_OVER);
    }
};

// --- a synthwave sunset: a striped sun, mountains of the spectrum, a grid floor racing in ---
struct Horizon : Style {
    std::vector<float> v, vs;
    std::vector<D2D1_POINT_2F> pts;
    struct Star { float x, y, b, phase; };
    std::vector<Star> stars;
    float travel = 0;
    ID2D1LinearGradientBrush *sky = nullptr, *floor_ = nullptr, *sun = nullptr, *grid = nullptr, *hill = nullptr;
    ID2D1RadialGradientBrush* halo = nullptr;
    D2D1_COLOR_F madeFor{ -1, 0, 0, 0 };                // (the background they were made for)
    void lost() override { release(sky); release(floor_); release(sun); release(grid); release(hill); release(halo); }
    float glow() const override { return 0.55f; }
    float pump() const override { return 0.2f; }
    float horizon(const Frame& f) const { return f.h * 0.6f; }
    void make(Frame& f) {
        lost();
        madeFor = f.bg;
        // full screen: night; in the window: from its background (it blends in) to a haze
        D2D1_COLOR_F top = f.full ? hex(0x05040C) : f.bg;
        D2D1_COLOR_F haze = f.full ? hex(0x3D3612) : mix(f.bg, hex(0xFFC060), f.dark ? 0.2f : 0.35f);
        sky = linear(f.dc, { { 0, top }, { 0.7f, mix(top, haze, 0.5f) }, { 1, haze } });
        D2D1_COLOR_F far_ = f.full ? hex(0x0E1020) : mix(f.bg, hex(0x6A8CFF), f.dark ? 0.15f : 0.2f);
        floor_ = linear(f.dc, { { 0, far_ }, { 1, f.full ? hex(0x02030A) : f.bg } });
        sun = linear(f.dc, { { 0, hex(0xFF3C9A) }, { 0.5f, hex(0xFF7AB0) }, { 1, hex(0xFFD0A0) } });
        D2D1_COLOR_F g = f.dark ? hex(0x5FB4FF) : hex(0x2F6FD0);
        grid = linear(f.dc, { { 0, alpha(g, 0.1f) }, { 1, alpha(g, 1) } });
        hill = linear(f.dc, { { 0, f.full ? hex(0x1A1024) : mix(f.bg, hex(0x40205A), 0.3f) },
                              { 1, f.full ? hex(0x06040C) : mix(f.bg, hex(0x40205A), 0.1f) } });
        halo = radial(f.dc, { { 0, hex(0xFF5AA0, 0.7f) }, { 0.55f, hex(0xFF5AA0, 0.25f) }, { 1, hex(0xFF5AA0, 0) } });
        if (stars.empty()) {
            std::mt19937 rng(11);
            std::uniform_real_distribution<float> u(0, 1);
            for (int i = 0; i < 90; ++i) stars.push_back({ u(rng), u(rng) * u(rng), u(rng), u(rng) * 6.28f });
        }
    }
    void backdrop(Frame& f) override {
        if (!sky || std::memcmp(&madeFor, &f.bg, sizeof(f.bg)) != 0) make(f);
        if (!sky || !floor_) return;
        float yh = horizon(f);
        sky->SetStartPoint({ 0, 0 });
        sky->SetEndPoint({ 0, yh });
        f.dc->FillRectangle(D2D1::RectF(0, 0, f.w, yh), sky);
        floor_->SetStartPoint({ 0, yh });
        floor_->SetEndPoint({ 0, f.h });
        f.dc->FillRectangle(D2D1::RectF(0, yh, f.w, f.h), floor_);
    }
    void draw(Frame& f) override {
        if (!sun || !grid || !hill) return;
        const Sound& snd = f.snd;
        float yh = horizon(f), cx = f.w / 2;
        if (f.dark)
            for (const Star& st : stars) {
                float a = 0.25f + 0.45f * st.b * (0.6f + 0.4f * std::sin((float)f.t * 1.7f + st.phase)) + 0.25f * snd.treble +
                          0.1f * snd.snap * st.b;
                f.color(hex(0xFFFFFF, std::min(a, 0.95f)));
                f.dc->FillEllipse(D2D1::Ellipse({ st.x * f.w, st.y * yh * 0.85f }, 1.2f * f.s, 1.2f * f.s), f.brush);
            }
        // the sun, beating, its lower half cut by stripes drifting down; a halo around it
        float R = std::min(f.w * 0.17f, yh * 0.7f) * (1 + 0.12f * snd.kick + 0.04f * snd.bass);
        float sy = yh - R * 0.3f;
        f.dc->PushAxisAlignedClip(D2D1::RectF(0, 0, f.w, yh), D2D1_ANTIALIAS_MODE_ALIASED);
        if (halo) {
            float hr = R * (1.25f + 0.3f * snd.kick);
            halo->SetCenter({ cx, sy });
            halo->SetRadiusX(hr);
            halo->SetRadiusY(hr);
            halo->SetOpacity((f.dark ? 0.4f : 0.3f) + 0.15f * snd.kick);
            f.dc->FillEllipse(D2D1::Ellipse({ cx, sy }, hr, hr), halo);
        }
        sun->SetStartPoint({ 0, sy - R });
        sun->SetEndPoint({ 0, sy + R });
        sun->SetOpacity(0.9f);
        f.dc->FillEllipse(D2D1::Ellipse({ cx, sy }, R, R), sun);
        f.dc->SetPrimitiveBlend(D2D1_PRIMITIVE_BLEND_COPY);
        f.color({ 0, 0, 0, 0 });
        float drift = std::fmod((float)f.t * 0.12f, 1.f);
        for (int k = 0; k < 7; ++k) {
            float pos = (k + drift) / 7;                // 0..1: from above the middle down to the horizon
            float top = sy - R * 0.25f + (yh - sy + R * 0.25f) * pos, th = R * (0.012f + 0.06f * pos);
            f.dc->FillRectangle(D2D1::RectF(cx - R - 2, top, cx + R + 2, top + th), f.brush);
        }
        f.dc->SetPrimitiveBlend(D2D1_PRIMITIVE_BLEND_SOURCE_OVER);
        f.dc->PopAxisAlignedClip();
        // mountains: the spectrum, the bass at the edges, low in the middle (the sun shows)
        int n = std::clamp((int)(f.w / (4 * f.s)), 60, 480), half = n / 2;
        v.resize(half);
        vis_bars(snd.level, 4, 150, v.data(), half);
        vs.resize(n + 1);
        for (int i = 0; i <= n; ++i) {
            float u = (float)i / n * 2 - 1;             // -1 left edge, 1 right edge
            int k = std::clamp((int)((1 - std::fabs(u)) * (half - 1)), 0, half - 1);
            float taper = 0.25f + 0.75f * std::pow(std::fabs(u), 0.8f);
            vs[i] = vs[i] * 0.4f + v[k] * taper * 0.6f;
        }
        pts.clear();
        pts.push_back({ -2, yh });
        for (int i = 0; i <= n; ++i) {
            float sm = (vs[std::max(0, i - 1)] + 2 * vs[i] + vs[std::min(n, i + 1)]) / 4;
            pts.push_back({ f.w * i / n, yh - sm * f.h * 0.3f * (1 + 0.12f * snd.kick) });
        }
        pts.push_back({ f.w + 2, yh });
        hill->SetStartPoint({ 0, yh - f.h * 0.3f });
        hill->SetEndPoint({ 0, yh });
        if (ID2D1PathGeometry* g = make_path(f.factory, pts, true, true)) {
            f.dc->FillGeometry(g, hill);
            g->Release();
        }
        std::vector<D2D1_POINT_2F> ridge(pts.begin() + 1, pts.end() - 1);
        for (auto [dx, col] : { std::pair{ -1.5f, 0xFF3C78u }, std::pair{ 1.5f, 0x3CE0FFu } }) {    // (a little colour fringe)
            std::vector<D2D1_POINT_2F> o = ridge;
            for (auto& p : o) p.x += dx * f.s * (1 + 1.5f * snd.kick);
            f.color(hex(col, f.dark ? 0.55f : 0.35f));
            stroke(f, o, 1.2f * f.s * std::sqrt(f.zoom));
        }
        f.color(f.dark ? hex(0xFFC24A) : hex(0xE08A10));
        stroke(f, ridge, 1.8f * f.s * std::sqrt(f.zoom) * (1 + 0.4f * snd.kick));
        // the floor's grid, racing in at a beat, lit up
        travel += f.dt * (0.6f + 2.0f * snd.bass + 4.0f * snd.kick);
        float depth = f.h - yh, fr = travel - std::floor(travel);
        grid->SetStartPoint({ 0, yh });
        grid->SetEndPoint({ 0, f.h });
        grid->SetOpacity(0.8f + 0.1f * snd.kick);
        float gw = 1 + 0.4f * snd.kick;
        for (int k = 0; k < 26; ++k) {
            float z = k + 1 - fr;                       // 0..26 (1: the bottom edge)
            if (z <= 0.05f) continue;
            float y = yh + depth / z;
            if (y > f.h + 2) continue;
            f.dc->DrawLine({ 0, y }, { f.w, y }, grid, std::max(1.f, 1.6f * f.s * std::sqrt(f.zoom) * gw / std::sqrt(z)));
        }
        float spacing = f.w * 0.09f;
        for (int i = -16; i <= 16; ++i)
            f.dc->DrawLine({ cx + i * spacing / 40, yh + depth / 40 }, { cx + i * spacing * 1.6f, f.h + depth * 0.6f }, grid,
                           1.3f * f.s * std::sqrt(f.zoom) * gw);
        grid->SetOpacity(1);
        f.color(f.dark ? hex(0xFFD27A, 0.7f) : hex(0xE0A040, 0.6f));
        f.dc->DrawLine({ 0, yh }, { f.w, yh }, f.brush, 1.2f * f.s);
    }
};

// --- a tunnel of rings, each the spectrum of its moment, flying past; a light at its end ---
struct Tunnel : Style {
    struct Ring { float z, hue, rot; bool hit; std::array<float, 40> r; };
    std::deque<Ring> rings;
    float spawn = 0, hueKick = 0;
    int seen = 0;
    std::vector<float> v;
    std::vector<D2D1_POINT_2F> pts;
    ID2D1RadialGradientBrush* light = nullptr;
    bool lightDark = false;
    float lightHue = -1;
    void lost() override { release(light); }
    float glow() const override { return 0.75f; }
    float pump() const override { return 0.2f; }
    // where a ring at depth z is: the far end wanders, the tunnel bends
    D2D1_POINT_2F centre(const Frame& f, float z) const {
        float k = std::pow(std::clamp(z / 3.2f, 0.f, 1.f), 0.8f);
        float vx = f.w * 0.1f * std::sin((float)f.t * 0.23f), vy = f.h * 0.09f * std::sin((float)f.t * 0.31f + 1);
        return { f.w / 2 + vx * k, f.h / 2 + vy * k };
    }
    void draw(Frame& f) override {
        const Sound& snd = f.snd;
        float speed = 0.45f + 1.1f * snd.energy + 2.4f * snd.kick;
        for (Ring& r : rings) r.z -= f.dt * speed;
        while (!rings.empty() && rings.front().z < 0.1f) rings.pop_front();
        bool hit = snd.beats != seen;
        if (hit) { seen = snd.beats; hueKick += 0.07f; }
        spawn += f.dt;
        if (spawn >= 0.08f || rings.empty() || hit) {   // (a beat: a ring of its own, bright)
            spawn = 0;
            Ring r{ 3.2f, (float)std::fmod(f.t * 0.1 + hueKick, 1.0), (float)f.t * 0.25f, hit, {} };
            v.resize(20);
            vis_bars(snd.level, 4, 140, v.data(), 20);
            for (int k = 0; k < 40; ++k) r.r[k] = v[k < 20 ? k : 39 - k];     // (mirrored: the same left and right)
            rings.push_back(r);
        }
        float R = std::min(f.w, f.h) * 0.16f * (1 + 0.15f * snd.kick);
        // the light at the end, in the far rings' colour, flaring at a beat
        float hue = rings.back().hue;
        if (!light || lightDark != f.dark || std::fabs(hue - lightHue) > 0.02f) {
            release(light);
            lightDark = f.dark;
            lightHue = hue;
            D2D1_COLOR_F c = hsv(hue, 0.6f, f.dark ? 1.f : 0.85f), core = f.dark ? hex(0xFFFFFF) : c;
            light = radial(f.dc, { { 0, core }, { 0.12f, alpha(mix(core, c, 0.5f), 0.85f) }, { 0.4f, alpha(c, 0.3f) }, { 1, alpha(c, 0) } });
        }
        if (light) {
            D2D1_POINT_2F end = centre(f, 3.2f);
            float lr = R * (0.7f + 0.5f * snd.kick + 0.4f * snd.energy);
            light->SetCenter(end);
            light->SetRadiusX(lr);
            light->SetRadiusY(lr);
            light->SetOpacity(0.45f + 0.15f * snd.kick);
            f.dc->FillEllipse(D2D1::Ellipse(end, lr, lr), light);
        }
        pts.resize(40);
        for (auto it = rings.rbegin(); it != rings.rend(); ++it) { // (the far ones first)
            const Ring& r = *it;
            float rad = R / r.z;
            float a = std::clamp((3.2f - r.z) / 0.8f, 0.f, 1.f) * std::clamp((r.z - 0.1f) / 0.35f, 0.f, 1.f);
            if (a <= 0.01f) continue;
            D2D1_POINT_2F c = centre(f, r.z);
            for (int k = 0; k < 40; ++k) {
                float ang = r.rot + 2 * kPi * k / 40;
                float rr = rad * (1 + 0.55f * r.r[k]);
                pts[k] = { c.x + std::cos(ang) * rr, c.y + std::sin(ang) * rr * 0.92f };
            }
            D2D1_COLOR_F col = hsv(r.hue, f.dark ? 0.65f : 0.8f, f.dark ? 1.f : 0.8f, a * 0.85f);
            if (r.hit) col = alpha(col, a);
            float wd = std::clamp(1.4f * f.s * f.zoom / std::sqrt(r.z), 0.8f, 5.f * f.s * f.zoom) * (r.hit ? 2.f : 1.f);
            f.color(col);
            stroke_lines(f, pts, true, wd);
        }
    }
};

// --- flying through stars: faster with the music, a leap at each beat ---
struct Warp : Style {
    struct Star { float x, y, z, hue; int band; };
    std::vector<Star> stars;
    std::mt19937 rng{ 3 };
    float speed = 0.2f, hue = 0.58f;
    int seen = 0;
    D2D1_POINT_2F vp{};                                 // (the vanishing point)
    ID2D1RadialGradientBrush* core = nullptr;
    void lost() override { release(core); }
    float trail() const override { return 0.45f; }
    D2D1_MATRIX_3X2_F trail_move(Frame& f) override {   // (the trails stream out from the middle)
        float k = 1.01f + 0.03f * std::min(speed, 2.f);
        return D2D1::Matrix3x2F::Scale(k, k, vp);
    }
    float glow() const override { return 0.9f; }
    float pump() const override { return 0.15f; }
    void place(Star& s, bool back) {
        std::uniform_real_distribution<float> u(-1, 1), u01(0, 1);
        do { s.x = u(rng); s.y = u(rng); } while (s.x * s.x + s.y * s.y < 0.01f);
        s.z = back ? 1.f + 0.2f * u01(rng) : 0.05f + 1.15f * u01(rng);
        s.hue = u01(rng);
        s.band = std::clamp((int)(u01(rng) * 140) + 4, 4, 150);
    }
    void draw(Frame& f) override {
        const Sound& snd = f.snd;
        if (stars.empty()) {
            stars.resize(700);
            for (Star& s : stars) place(s, false);
        }
        if (snd.beats != seen) { seen = snd.beats; hue += 0.04f; }
        speed = smooth_to(speed, 0.12f + 0.9f * snd.energy + 1.8f * snd.kick, f.dt, 0.12f);
        vp = { f.w / 2 + f.w * 0.05f * std::sin((float)f.t * 0.17f), f.h / 2 + f.h * 0.05f * std::sin((float)f.t * 0.13f + 2) };
        float F = std::max(f.w, f.h) * 0.22f;
        // a glow where it all comes from
        if (!core) core = radial(f.dc, { { 0, hex(0xFFFFFF, 0.9f) }, { 0.3f, hex(0xB0C8FF, 0.35f) }, { 1, hex(0x6080FF, 0) } });
        if (core && f.dark) {
            float cr = std::min(f.w, f.h) * (0.08f + 0.04f * snd.kick);
            core->SetCenter(vp);
            core->SetRadiusX(cr);
            core->SetRadiusY(cr);
            core->SetOpacity((f.dark ? 0.35f : 0.2f) + 0.1f * snd.kick);
            f.dc->FillEllipse(D2D1::Ellipse(vp, cr, cr), core);
        }
        float tail = 0.02f + 0.16f * speed;             // (the streak: how far back, as a share of the depth)
        float wk = std::sqrt(f.zoom) * f.s;
        for (Star& s : stars) {
            s.z -= f.dt * speed * 0.5f;
            if (s.z <= 0.02f) { place(s, true); continue; }
            float z1 = s.z * (1 + tail);
            D2D1_POINT_2F head{ vp.x + s.x / s.z * F, vp.y + s.y / s.z * F }, back{ vp.x + s.x / z1 * F, vp.y + s.y / z1 * F };
            if (head.x < -50 || head.x > f.w + 50 || head.y < -50 || head.y > f.h + 50) { place(s, true); continue; }
            float close = std::clamp(1 - s.z, 0.f, 1.f), lvl = snd.level[s.band];
            float a = std::clamp((1.2f - s.z) * 2.f, 0.f, 1.f) * (0.35f + 0.65f * lvl);
            D2D1_COLOR_F c = f.dark ? hsv(hue + (s.hue - 0.5f) * 0.3f, 0.15f + 0.55f * lvl, 1, a)
                                    : hsv(hue + (s.hue - 0.5f) * 0.3f, 0.75f, 0.55f, a);
            f.color(c);
            f.dc->DrawLine(back, head, f.brush, (0.6f + 2.6f * close * close) * wk, f.round);
        }
    }
};

// --- lines of the spectrum one behind another, the newest in front, rolling away ---
struct Ridges : Style {
    static const int P = 72;
    std::deque<std::array<float, P>> rows;
    float age = 0;                                      // (since the newest row came)
    std::mt19937 rng{ 5 };
    std::vector<float> v;
    std::vector<D2D1_POINT_2F> pts, curve;
    float glow() const override { return 0.45f; }
    float pump() const override { return 0.2f; }
    void draw(Frame& f) override {
        const Sound& snd = f.snd;
        const float step = 1 / 24.f;
        int N = std::clamp((int)(f.h / (11 * f.s * std::sqrt(f.zoom))), 12, 40);
        age += f.dt;
        if (rows.empty() || age >= step) {             // a new row: the spectrum, the bass in the middle
            age = rows.empty() ? 0 : std::fmod(age, step);
            v.resize(P / 2);
            vis_bars(snd.level, 4, 140, v.data(), P / 2);
            std::array<float, P> row;
            std::uniform_real_distribution<float> jit(-1, 1);
            for (int p = 0; p < P; ++p) {
                float u = (float)p / (P - 1) * 2 - 1;
                int k = std::clamp((int)(std::fabs(u) * (P / 2)), 0, P / 2 - 1);
                float e = std::max(0.f, 1 - u * u * 1.3f), env = 0.06f + 0.94f * e * e;
                row[p] = std::max(0.f, env * v[k] * (0.85f + 0.3f * snd.kick) + 0.015f * (0.3f + env) * jit(rng));
            }
            rows.push_front(row);
        }
        while ((int)rows.size() > N + 1) rows.pop_back();
        float W = f.full ? std::min(f.w * 0.62f, f.h * 1.3f) : f.w * 0.8f;
        float bottom = f.h * 0.95f, sp = f.h * 0.86f / N, A = sp * 7;
        float shift = age / step;                       // (the rows move a step back in that time)
        D2D1_COLOR_F hide = f.dark ? D2D1::ColorF(0, 0, 0, 1) : f.bg;
        D2D1_COLOR_F ink = f.dark ? hex(0xFFFFFF) : hex(0x202020);
        float hueNow = (float)std::fmod(f.t * 0.03, 1.0);
        for (int i = (int)rows.size() - 1; i >= 0; --i) {   // (the farthest first: the nearer hide them)
            float d = (i + shift) / N;                  // 0 front .. 1 back
            if (d > 1) continue;
            float base = bottom - (i + shift) * sp, wi = W * (1 - 0.3f * d), ai = A * (1 - 0.35f * d);
            pts.resize(P);
            for (int p = 0; p < P; ++p) pts[p] = { f.w / 2 + ((float)p / (P - 1) - 0.5f) * wi, base - rows[i][p] * ai };
            smooth_curve(pts, false, 12 * f.s, curve);
            curve.push_back({ curve.back().x, base + sp });
            curve.push_back({ curve.front().x, base + sp });
            f.color(hide);
            if (ID2D1PathGeometry* g = make_path(f.factory, curve, true, true)) {
                f.dc->FillGeometry(g, f.brush);
                g->Release();
            }
            curve.resize(curve.size() - 2);
            float a = std::min({ 1.f, (1 - d) * 1.6f, i == 0 ? shift * 3 : 1.f });
            // (the nearest rows take a colour at a beat)
            D2D1_COLOR_F c = mix(ink, hsv(hueNow, 0.6f, 1), f.dark ? snd.kick * std::max(0.f, 1 - d * 4) * 0.5f : 0);
            f.color(alpha(c, a * (0.55f + 0.45f * (1 - d))));
            draw_lines(f, curve, 1.3f * f.s * std::sqrt(f.zoom) * (1 - 0.3f * d));
        }
    }
};

// --- fireworks: a burst at each beat, crackles with the snare, the sparks falling, fading ---
struct Fireworks : Style {
    struct Spark { float x, y, vx, vy, life, max, w; D2D1_COLOR_F c; };
    std::vector<Spark> sparks;
    std::mt19937 rng{ 9 };
    int seen = 0, seenSnap = 0, kind = 0;
    float idle = 0, sinceSnap = 0;
    float trail() const override { return 0.8f; }
    D2D1_MATRIX_3X2_F trail_move(Frame& f) override { return D2D1::Matrix3x2F::Translation(0, 0.6f * f.s); }  // (the smoke sinks)
    float glow() const override { return 1.0f; }
    float glow_radius() const override { return 10; }
    float pump() const override { return 0.05f; }
    void burst(Frame& f, float strength, bool small) {
        std::uniform_real_distribution<float> u(0, 1);
        const Sound& snd = f.snd;
        float m = std::min(f.w, f.h);
        float x = f.w * (0.15f + 0.7f * u(rng)), y = f.h * (small ? 0.2f + 0.6f * u(rng) : 0.18f + 0.4f * u(rng));
        // its colour: where the spectrum is loudest (the bass red, the highs violet); a second one
        int loud = 4;
        for (int b = 4; b < 150; ++b) if (snd.level[b] > snd.level[loud]) loud = b;
        float h1 = (loud - 4) / 146.f * 0.8f + 0.12f * u(rng), h2 = h1 + 0.3f + 0.25f * u(rng);
        int kindNow = small ? -1 : kind++ % 4;          // sphere, ring, willow, two rings
        int count = small ? 18 : (int)(70 + 120 * strength);
        float speed = m * (small ? 0.18f : 0.4f + 0.45f * strength);
        float wd = (small ? 1.2f : 1.8f) * f.s * std::pow(f.zoom, 0.7f);
        for (int i = 0; i < count; ++i) {
            float a = 2 * kPi * u(rng), c = u(rng) * 2 - 1;
            float sp = kindNow == 1 ? 1.f : kindNow == 3 ? (i % 2 ? 1.f : 0.55f) : std::sqrt(1 - c * c);    // (a ball, seen from the side)
            float life = small ? 0.35f + 0.3f * u(rng) : kindNow == 2 ? 2.0f + u(rng) : 1.1f + 0.7f * u(rng);
            D2D1_COLOR_F col = small ? hsv(0.13f, 0.25f, 1) : kindNow == 2 ? hsv(0.1f, 0.75f, 1)
                             : hsv(i % 3 == 0 ? h2 : h1, 0.72f, 1);
            if (!f.dark) col = hsv(small ? 0.1f : i % 3 == 0 ? h2 : h1, 0.85f, small ? 0.55f : 0.75f);
            float v = speed * sp * (0.92f + 0.16f * u(rng));
            sparks.push_back({ x, y, std::cos(a) * v, std::sin(a) * v, life, life, wd, col });
        }
        if (sparks.size() > 5000) sparks.erase(sparks.begin(), sparks.begin() + (long)(sparks.size() - 5000));
    }
    void draw(Frame& f) override {
        const Sound& snd = f.snd;
        idle += f.dt;
        if (snd.beats != seen) {
            seen = snd.beats;
            burst(f, std::min(1.f, 0.4f + 0.8f * snd.bass), false);
            idle = 0;
        } else if (idle > 1.4f && snd.energy > 0.1f) {  // (no beat for a while: one now and then anyway)
            burst(f, snd.energy, false);
            idle = 0;
        }
        sinceSnap += f.dt;
        if (snd.snaps != seenSnap) {
            seenSnap = snd.snaps;
            if (sinceSnap > 0.3f) { burst(f, 0, true); sinceSnap = 0; }
        }
        float g = std::min(f.w, f.h) * 0.35f, drag = std::exp(-f.dt * 1.4f);
        if (f.dark) f.dc->SetPrimitiveBlend(D2D1_PRIMITIVE_BLEND_ADD);     // (light on light: laid over)
        size_t live = 0;
        for (size_t i = 0; i < sparks.size(); ++i) {
            Spark s = sparks[i];
            s.life -= f.dt;
            if (s.life <= 0) continue;
            s.vx *= drag;
            s.vy = s.vy * drag + g * f.dt * (s.max > 1.9f ? 1.6f : 1.f);     // (a willow's droop more)
            s.x += s.vx * f.dt;
            s.y += s.vy * f.dt;
            sparks[live++] = s;
            float k = s.life / s.max, a = std::pow(k, 1.3f);
            if (k < 0.4f) a *= 0.55f + 0.45f * std::sin((float)f.t * 37 + (float)i);      // (twinkling as they die)
            f.color(alpha(s.c, std::clamp(a, 0.f, 1.f)));
            f.dc->DrawLine({ s.x - s.vx * 0.035f, s.y - s.vy * 0.035f }, { s.x, s.y }, f.brush, s.w);
        }
        sparks.resize(live);
        f.dc->SetPrimitiveBlend(D2D1_PRIMITIVE_BLEND_SOURCE_OVER);
    }
};

std::unique_ptr<Style> make_style(int s) {
    switch (s) {
        case VIS_WAVEFORM: return std::make_unique<Waveform>();
        case VIS_CIRCLE: return std::make_unique<Circle>();
        case VIS_PARTICLES: return std::make_unique<Particles>();
        case VIS_RIBBONS: return std::make_unique<Ribbons>();
        case VIS_HORIZON: return std::make_unique<Horizon>();
        case VIS_TUNNEL: return std::make_unique<Tunnel>();
        case VIS_WARP: return std::make_unique<Warp>();
        case VIS_RIDGES: return std::make_unique<Ridges>();
        case VIS_FIREWORKS: return std::make_unique<Fireworks>();
        default: return std::make_unique<Bars>();
    }
}

// the system's graphics DLLs, loaded once (the first visualization shown), never freed
struct Dlls {
    decltype(&D3D11CreateDevice) d3d = nullptr;
    HRESULT(WINAPI* d2d)(D2D1_FACTORY_TYPE, REFIID, const D2D1_FACTORY_OPTIONS*, void**) = nullptr;
    HRESULT(WINAPI* dwrite)(DWRITE_FACTORY_TYPE, REFIID, IUnknown**) = nullptr;
};
const Dlls& dlls() {
    static Dlls d = [] {
        Dlls r;
        auto load = [](const wchar_t* n) { return LoadLibraryExW(n, nullptr, LOAD_LIBRARY_SEARCH_SYSTEM32); };
        if (HMODULE m = load(L"d3d11.dll")) r.d3d = (decltype(r.d3d))(void*)GetProcAddress(m, "D3D11CreateDevice");
        if (HMODULE m = load(L"d2d1.dll")) r.d2d = (decltype(r.d2d))(void*)GetProcAddress(m, "D2D1CreateFactory");
        if (HMODULE m = load(L"dwrite.dll")) r.dwrite = (decltype(r.dwrite))(void*)GetProcAddress(m, "DWriteCreateFactory");
        return r;
    }();
    return d;
}

} // namespace

const wchar_t* vis_name(int style) {
    static const wchar_t* names[VIS_COUNT] = { L"Spectrogram", L"Mirrored bars", L"Waveform", L"Circle",
                                               L"Particles", L"Ribbons", L"Horizon", L"Tunnel", L"Warp", L"Ridges",
                                               L"Fireworks" };
    return style >= 0 && style < VIS_COUNT ? names[style] : L"";
}

// ------------------------------------------------------------------
// the window and its thread
// ------------------------------------------------------------------
struct Visualizer::Impl {
    HWND wnd = nullptr, notify = nullptr;
    UINT failMsg = 0;
    COLORREF paintBg = 0;                   // (UI thread: the window until its first frame)
    bool placed = false;
    RECT at{};
    std::thread th;
    std::atomic<bool> failed{ false };
    std::atomic<bool> busy{ false };        // (the thread: something still moves)
    // the UI thread's word to the thread
    std::mutex mx;
    std::condition_variable cv;
    bool stop = false, shown = false, dirty = true;
    VisLook look;
    SpectrumTap tap;
    LONGLONG clockPos = 0, clockQpc = 0;
    bool running = false;
    VisCaption caption;
    unsigned captionGen = 0, announceGen = 0;
    bool captionHeld = false;

    void wake() { dirty = true; cv.notify_one(); }
    void run();
};

namespace {

struct Snapshot {
    VisLook look;
    SpectrumTap tap;
    LONGLONG clockPos = 0, clockQpc = 0;
    bool running = false;
    VisCaption caption;
    unsigned captionGen = 0, announceGen = 0;
    bool captionHeld = false;
};

class Renderer {
public:
    explicit Renderer(HWND w) : m_wnd(w) {
        LARGE_INTEGER f;
        QueryPerformanceFrequency(&f);
        m_qpf = (double)f.QuadPart;
    }
    ~Renderer() { teardown(true); }
    bool ready() const { return m_dc != nullptr; }
    bool create();
    HANDLE waitable() const { return m_wait; }
    // a frame; false: the device was lost (made again next time)
    bool frame(const Snapshot& s, bool& busy);
private:
    void teardown(bool all);
    bool make_targets(UINT w, UINT h);
    void drop_targets();
    void overlays(Frame& f, const Snapshot& s, double now);
    void caption(Frame& f, const Snapshot& s, float a, float ts);

    HWND m_wnd;
    double m_qpf = 1;
    ID3D11Device* m_d3d = nullptr;
    IDXGISwapChain1* m_swap = nullptr;
    HANDLE m_wait = nullptr;
    ID2D1Factory1* m_factory = nullptr;
    ID2D1Device* m_device = nullptr;
    ID2D1DeviceContext* m_dc = nullptr;
    ID2D1Bitmap1* m_back = nullptr;
    ID2D1Bitmap1* m_scene[2]{};
    ID2D1Bitmap1* m_small = nullptr;        // (the scene at a quarter: the glow's)
    ID2D1Effect* m_blur = nullptr;
    ID2D1Effect* m_strength = nullptr;      // (the glow's: a colour matrix after the blur)
    ID2D1SolidColorBrush* m_brush = nullptr;
    ID2D1StrokeStyle* m_round = nullptr;
    IDWriteFactory* m_dw = nullptr;
    IDWriteTextFormat *m_bigFmt = nullptr, *m_titleFmt = nullptr, *m_detailFmt = nullptr;
    float m_fmtScale = 0;                   // (the text's: the DPI's, bigger on a big area)
    UINT m_w = 0, m_h = 0;
    int m_cur = 0;
    bool m_sceneFresh = true;               // (no previous frame to trail)
    // the style and the sound
    int m_styleId = -1;
    std::unique_ptr<Style> m_style;
    Analyzer m_an;
    Sound m_snd;
    double m_motion = 0;                    // seconds of motion
    LONGLONG m_lastQpc = 0;
    // overlays
    unsigned m_announceSeen = 0, m_captionSeen = 0;
    double m_announceAt = -10;
    double m_captionUntil = 0;
    float m_captionA = 0;
    ID2D1Bitmap1* m_cover = nullptr;
    IDWriteTextLayout *m_titleLay = nullptr, *m_detailLay = nullptr;
    float m_layW = 0;
    // perf: frame intervals
    double m_statFrom = 0, m_statMax = 0, m_statCpu = 0;
    long long m_statN = 0;
};

bool Renderer::create() {
    const Dlls& d = dlls();
    if (!d.d3d || !d.d2d || !d.dwrite) return false;
    RECT rc{};
    GetClientRect(m_wnd, &rc);
    UINT w = (UINT)std::max(8L, rc.right), h = (UINT)std::max(8L, rc.bottom);
    const D3D_FEATURE_LEVEL levels[] = { D3D_FEATURE_LEVEL_11_1, D3D_FEATURE_LEVEL_11_0, D3D_FEATURE_LEVEL_10_1,
                                         D3D_FEATURE_LEVEL_10_0, D3D_FEATURE_LEVEL_9_3, D3D_FEATURE_LEVEL_9_1 };
    HRESULT hr = E_FAIL;
    for (D3D_DRIVER_TYPE type : { D3D_DRIVER_TYPE_HARDWARE, D3D_DRIVER_TYPE_WARP }) {
        hr = d.d3d(nullptr, type, nullptr, D3D11_CREATE_DEVICE_BGRA_SUPPORT, levels, ARRAYSIZE(levels), D3D11_SDK_VERSION,
                   &m_d3d, nullptr, nullptr);
        if (FAILED(hr))         // (11_1 unknown to an older runtime)
            hr = d.d3d(nullptr, type, nullptr, D3D11_CREATE_DEVICE_BGRA_SUPPORT, levels + 1, ARRAYSIZE(levels) - 1,
                       D3D11_SDK_VERSION, &m_d3d, nullptr, nullptr);
        if (SUCCEEDED(hr)) break;
    }
    if (FAILED(hr)) return false;
    IDXGIDevice1* dxdev = nullptr;
    IDXGIAdapter* adapter = nullptr;
    IDXGIFactory2* dxf = nullptr;
    bool ok = SUCCEEDED(m_d3d->QueryInterface(__uuidof(IDXGIDevice1), (void**)&dxdev)) &&
              SUCCEEDED(dxdev->GetAdapter(&adapter)) && SUCCEEDED(adapter->GetParent(__uuidof(IDXGIFactory2), (void**)&dxf));
    if (ok) {
        DXGI_SWAP_CHAIN_DESC1 sd{};
        sd.Width = w;
        sd.Height = h;
        sd.Format = DXGI_FORMAT_B8G8R8A8_UNORM;
        sd.SampleDesc.Count = 1;
        sd.BufferUsage = DXGI_USAGE_RENDER_TARGET_OUTPUT;
        sd.BufferCount = 2;
        sd.Scaling = DXGI_SCALING_STRETCH;
        sd.SwapEffect = DXGI_SWAP_EFFECT_FLIP_SEQUENTIAL;
        sd.AlphaMode = DXGI_ALPHA_MODE_IGNORE;
        sd.Flags = kSwapFlags;
        ok = SUCCEEDED(dxf->CreateSwapChainForHwnd(m_d3d, m_wnd, &sd, nullptr, nullptr, &m_swap));
        if (ok) dxf->MakeWindowAssociation(m_wnd, DXGI_MWA_NO_ALT_ENTER | DXGI_MWA_NO_WINDOW_CHANGES);
    }
    if (ok) {
        IDXGISwapChain2* s2 = nullptr;
        if (SUCCEEDED(m_swap->QueryInterface(__uuidof(IDXGISwapChain2), (void**)&s2))) {
            s2->SetMaximumFrameLatency(1);
            m_wait = s2->GetFrameLatencyWaitableObject();
            s2->Release();
        }
        D2D1_FACTORY_OPTIONS fo{ D2D1_DEBUG_LEVEL_NONE };
        if (!m_factory) ok = SUCCEEDED(d.d2d(D2D1_FACTORY_TYPE_SINGLE_THREADED, __uuidof(ID2D1Factory1), &fo, (void**)&m_factory));
        ok = ok && SUCCEEDED(m_factory->CreateDevice(dxdev, &m_device)) &&
             SUCCEEDED(m_device->CreateDeviceContext(D2D1_DEVICE_CONTEXT_OPTIONS_NONE, &m_dc));
    }
    release(dxf);
    release(adapter);
    release(dxdev);
    if (ok && !m_round)
        m_factory->CreateStrokeStyle(D2D1::StrokeStyleProperties(D2D1_CAP_STYLE_ROUND, D2D1_CAP_STYLE_ROUND, D2D1_CAP_STYLE_ROUND,
                                                                 D2D1_LINE_JOIN_ROUND), nullptr, 0, &m_round);
    if (ok && !m_dw) d.dwrite(DWRITE_FACTORY_TYPE_SHARED, __uuidof(IDWriteFactory), (IUnknown**)&m_dw);
    ok = ok && m_round && SUCCEEDED(m_dc->CreateSolidColorBrush(D2D1::ColorF(0, 0, 0), &m_brush)) &&
         SUCCEEDED(m_dc->CreateEffect(kClsidBlur, &m_blur)) && SUCCEEDED(m_dc->CreateEffect(kClsidColorMatrix, &m_strength));
    if (ok) {
        m_dc->SetUnitMode(D2D1_UNIT_MODE_PIXELS);
        m_dc->SetAntialiasMode(D2D1_ANTIALIAS_MODE_PER_PRIMITIVE);
        m_blur->SetValue(D2D1_GAUSSIANBLUR_PROP_OPTIMIZATION, (UINT32)0);      // (speed)
        m_blur->SetValue(D2D1_GAUSSIANBLUR_PROP_BORDER_MODE, D2D1_BORDER_MODE_SOFT);
        m_strength->SetInputEffect(0, m_blur);
        m_strength->SetValue(D2D1_COLORMATRIX_PROP_ALPHA_MODE, D2D1_COLORMATRIX_ALPHA_MODE_STRAIGHT);
        m_strength->SetValue(D2D1_COLORMATRIX_PROP_CLAMP_OUTPUT, TRUE);
        ok = make_targets(w, h);
    }
    if (!ok) teardown(false);
    return ok;
}

bool Renderer::make_targets(UINT w, UINT h) {
    IDXGISurface* surf = nullptr;
    if (FAILED(m_swap->GetBuffer(0, __uuidof(IDXGISurface), (void**)&surf))) return false;
    D2D1_BITMAP_PROPERTIES1 bp = D2D1::BitmapProperties1(D2D1_BITMAP_OPTIONS_TARGET | D2D1_BITMAP_OPTIONS_CANNOT_DRAW,
                                                        D2D1::PixelFormat(DXGI_FORMAT_B8G8R8A8_UNORM, D2D1_ALPHA_MODE_IGNORE));
    HRESULT hr = m_dc->CreateBitmapFromDxgiSurface(surf, &bp, &m_back);
    surf->Release();
    if (FAILED(hr)) return false;
    D2D1_BITMAP_PROPERTIES1 tp = D2D1::BitmapProperties1(D2D1_BITMAP_OPTIONS_TARGET,
                                                        D2D1::PixelFormat(DXGI_FORMAT_B8G8R8A8_UNORM, D2D1_ALPHA_MODE_PREMULTIPLIED));
    for (auto*& s : m_scene)
        if (FAILED(m_dc->CreateBitmap(D2D1::SizeU(w, h), nullptr, 0, &tp, &s))) return false;
    if (FAILED(m_dc->CreateBitmap(D2D1::SizeU(w / 4 + 1, h / 4 + 1), nullptr, 0, &tp, &m_small))) return false;
    m_blur->SetInput(0, m_small);
    m_w = w;
    m_h = h;
    m_sceneFresh = true;
    return true;
}

void Renderer::drop_targets() {
    if (m_dc) m_dc->SetTarget(nullptr);
    if (m_blur) m_blur->SetInput(0, nullptr);
    release(m_back);
    for (auto*& s : m_scene) release(s);
    release(m_small);
}

void Renderer::teardown(bool all) {
    drop_targets();
    if (m_style) m_style->lost();
    release(m_cover);
    m_captionSeen = 0;                      // (its cover is made again)
    release(m_strength);
    release(m_blur);
    release(m_brush);
    release(m_dc);
    release(m_device);
    if (m_wait) { CloseHandle(m_wait); m_wait = nullptr; }
    release(m_swap);
    release(m_d3d);
    if (all) {
        release(m_titleLay);
        release(m_detailLay);
        release(m_bigFmt);
        release(m_titleFmt);
        release(m_detailFmt);
        release(m_dw);
        release(m_round);
        release(m_factory);
    }
}

bool Renderer::frame(const Snapshot& s, bool& busy) {
    LARGE_INTEGER q;
    QueryPerformanceCounter(&q);
    double now = q.QuadPart / m_qpf;
    float dt = m_lastQpc ? (float)std::clamp((q.QuadPart - m_lastQpc) / m_qpf, 0.0, 0.1) : 1 / 60.f;
    if (perf_on()) {                        // (the frame rate, every 5 s)
        if (m_lastQpc) {
            m_statMax = std::max(m_statMax, (q.QuadPart - m_lastQpc) / m_qpf * 1000);
            ++m_statN;
        }
        if (!m_statFrom) m_statFrom = now;
        if (now - m_statFrom >= 5 && m_statN) {
            wchar_t d[96];
            swprintf_s(d, L"max=%.1fms cpu=%.1fms style=%d %ux%u", m_statMax, m_statCpu / std::max(1LL, m_statN), s.look.style, m_w, m_h);
            perf_event("vis.frames", (now - m_statFrom) * 1000 / m_statN, m_statN, d);
            m_statFrom = now;
            m_statN = 0;
            m_statMax = 0;
            m_statCpu = 0;
        }
    }
    m_lastQpc = q.QuadPart;
    // the size: the window's
    RECT rc{};
    GetClientRect(m_wnd, &rc);
    UINT w = (UINT)rc.right, h = (UINT)rc.bottom;
    if (w < 2 || h < 2) { busy = false; return true; }
    if (w != m_w || h != m_h) {
        drop_targets();
        HRESULT hr = m_swap->ResizeBuffers(0, w, h, DXGI_FORMAT_UNKNOWN, kSwapFlags);
        if (FAILED(hr) || !make_targets(w, h)) { teardown(false); return false; }
    }
    if (s.look.style != m_styleId) {
        if (m_style) m_style->lost();
        m_style = make_style(s.look.style);
        m_styleId = s.look.style;
        m_sceneFresh = true;
    }
    // the sound now: the playback position, moved on since the window said
    LONGLONG pos = s.clockPos;
    if (s.running && s.clockQpc) pos += (LONGLONG)(std::min(5.0, (q.QuadPart - s.clockQpc) / m_qpf) * 10000000);
    m_an.update(s.tap, pos, s.running, dt, m_snd);
    m_motion += dt;
    float scale = s.look.dpi / 96.f;
    bool dark = s.look.full || s.look.dark;
    D2D1_COLOR_F bg = s.look.full ? D2D1::ColorF(0, 0, 0) : cref(s.look.bg);
    float zoom = std::clamp(std::min((float)w, (float)h) / (600 * scale), 1.f, 2.5f);
    Frame f{ m_dc, m_factory, m_brush, m_round, (float)w, (float)h, scale, zoom, dt, m_motion, dark, s.look.full, bg, m_snd };
    m_dc->BeginDraw();
    // the scene: the style's drawing, over what's left of the last frame
    int prev = m_cur;
    m_cur ^= 1;
    m_dc->SetTarget(m_scene[m_cur]);
    m_dc->Clear(D2D1::ColorF(0, 0, 0, 0));
    float trail = m_style->trail();
    if (trail > 0 && !m_sceneFresh) {
        D2D1_RECT_F all = D2D1::RectF(0, 0, (float)w, (float)h);
        m_dc->SetTransform(m_style->trail_move(f));
        m_dc->DrawBitmap(m_scene[prev], &all, trail, D2D1_INTERPOLATION_MODE_LINEAR, nullptr, nullptr);
        m_dc->SetTransform(D2D1::IdentityMatrix());
    }
    m_sceneFresh = false;
    m_style->draw(f);
    // its glow: a quarter of it, blurred
    float glow = m_style->glow();
    if (glow > 0) {
        m_dc->SetTarget(m_small);
        m_dc->Clear(D2D1::ColorF(0, 0, 0, 0));
        D2D1_RECT_F q4 = D2D1::RectF(0, 0, w / 4.f, h / 4.f);
        m_dc->DrawBitmap(m_scene[m_cur], &q4, 1, D2D1_INTERPOLATION_MODE_LINEAR, nullptr, nullptr);
        m_blur->SetValue(D2D1_GAUSSIANBLUR_PROP_STANDARD_DEVIATION, m_style->glow_radius() * scale / 4);
        float g = (dark ? glow : glow * 0.6f) * (1 + m_style->pump() * m_snd.kick);     // (it pumps with the beat)
        D2D1_MATRIX_5X4_F m{};
        m.m[0][0] = m.m[1][1] = m.m[2][2] = m.m[3][3] = g;
        m_strength->SetValue(D2D1_COLORMATRIX_PROP_COLOR_MATRIX, m);
    }
    m_dc->SetTarget(m_back);
    m_dc->Clear(bg);
    m_style->backdrop(f);
    // (on dark: added, light on light; on a light background: laid over it)
    D2D1_COMPOSITE_MODE mode = dark ? D2D1_COMPOSITE_MODE_PLUS : D2D1_COMPOSITE_MODE_SOURCE_OVER;
    if (glow > 0) {
        m_dc->SetTransform(D2D1::Matrix3x2F::Scale(4, 4));
        m_dc->DrawImage(m_strength, D2D1_INTERPOLATION_MODE_LINEAR, mode);
        m_dc->SetTransform(D2D1::IdentityMatrix());
    }
    m_dc->DrawImage(m_scene[m_cur], D2D1_INTERPOLATION_MODE_LINEAR, mode);
    overlays(f, s, now);
    HRESULT hr = m_dc->EndDraw();
    if (perf_on()) {
        LARGE_INTEGER e;
        QueryPerformanceCounter(&e);
        m_statCpu += (e.QuadPart - q.QuadPart) / m_qpf * 1000;
    }
    if (hr == (HRESULT)D2DERR_RECREATE_TARGET) { teardown(false); return false; }
    hr = m_swap->Present(1, 0);
    if (hr == DXGI_ERROR_DEVICE_REMOVED || hr == DXGI_ERROR_DEVICE_RESET) { teardown(false); return false; }
    if (hr == DXGI_STATUS_OCCLUDED) Sleep(50);  // (minimised: no hurry)
    busy = !m_snd.quiet || m_captionA > 0.001f || now - m_announceAt < 2.0 || now < m_captionUntil;
    return true;
}

void Renderer::overlays(Frame& f, const Snapshot& s, double now) {
    if (!m_dw) return;
    float ts = f.s * (1 + (f.zoom - 1) * 0.6f);
    if (m_fmtScale != ts) {
        release(m_bigFmt);
        release(m_titleFmt);
        release(m_detailFmt);
        release(m_titleLay);
        release(m_detailLay);
        m_fmtScale = ts;
        m_dw->CreateTextFormat(L"Segoe UI", nullptr, DWRITE_FONT_WEIGHT_BOLD, DWRITE_FONT_STYLE_NORMAL, DWRITE_FONT_STRETCH_NORMAL,
                               34 * ts, L"", &m_bigFmt);
        m_dw->CreateTextFormat(L"Segoe UI", nullptr, DWRITE_FONT_WEIGHT_SEMI_BOLD, DWRITE_FONT_STYLE_NORMAL,
                               DWRITE_FONT_STRETCH_NORMAL, 22 * ts, L"", &m_titleFmt);
        m_dw->CreateTextFormat(L"Segoe UI", nullptr, DWRITE_FONT_WEIGHT_NORMAL, DWRITE_FONT_STYLE_NORMAL, DWRITE_FONT_STRETCH_NORMAL,
                               15 * ts, L"", &m_detailFmt);
        for (IDWriteTextFormat* t : { m_titleFmt, m_detailFmt })
            if (t) {
                t->SetWordWrapping(DWRITE_WORD_WRAPPING_NO_WRAP);
                DWRITE_TRIMMING trim{ DWRITE_TRIMMING_GRANULARITY_CHARACTER, 0, 0 };
                IDWriteInlineObject* dots = nullptr;
                m_dw->CreateEllipsisTrimmingSign(t, &dots);
                t->SetTrimming(&trim, dots);
                release(dots);
            }
        m_captionSeen = 0;
    }
    // the style's name, when it's chosen: spaced capitals, fading in and out
    if (s.announceGen != m_announceSeen) {
        m_announceSeen = s.announceGen;
        m_announceAt = now;
    }
    double age = now - m_announceAt;
    if (age < 2.0 && m_bigFmt) {
        float a = (float)std::min({ age / 0.2, 1.0, (2.0 - age) / 0.6 });
        std::wstring name = vis_name(s.look.style);
        for (auto& c : name) c = (wchar_t)towupper(c);
        IDWriteTextLayout* lay = nullptr;
        if (SUCCEEDED(m_dw->CreateTextLayout(name.c_str(), (UINT32)name.size(), m_bigFmt, f.w, f.h, &lay))) {
            lay->SetTextAlignment(DWRITE_TEXT_ALIGNMENT_CENTER);
            lay->SetParagraphAlignment(DWRITE_PARAGRAPH_ALIGNMENT_CENTER);
            float size = std::min(34 * ts, f.h * 0.22f);
            lay->SetFontSize(size, { 0, (UINT32)name.size() });
            IDWriteTextLayout1* l1 = nullptr;
            if (SUCCEEDED(lay->QueryInterface(__uuidof(IDWriteTextLayout1), (void**)&l1))) {
                l1->SetCharacterSpacing(size * 0.18f, size * 0.18f, 0, { 0, (UINT32)name.size() });
                l1->Release();
            }
            f.color(f.dark ? D2D1::ColorF(0, 0, 0, 0.5f * a) : alpha(f.bg, 0.6f * a));
            m_dc->DrawTextLayout({ 2 * f.s, 2 * f.s }, lay, m_brush);
            f.color(f.dark ? D2D1::ColorF(1, 1, 1, a) : alpha(cref(s.look.text), a));
            m_dc->DrawTextLayout({ 0, 0 }, lay, m_brush);
            lay->Release();
        }
    }
    // full screen: what's playing
    if (s.captionGen != m_captionSeen) {
        m_captionSeen = s.captionGen;
        m_captionUntil = now + 6;
        release(m_cover);
        release(m_titleLay);
        release(m_detailLay);
        const VisCaption& c = s.caption;
        if (c.cover && c.coverW > 0 && c.coverH > 0 && c.cover->size() >= (size_t)c.coverW * c.coverH) {
            D2D1_BITMAP_PROPERTIES1 bp = D2D1::BitmapProperties1(D2D1_BITMAP_OPTIONS_NONE,
                                                                D2D1::PixelFormat(DXGI_FORMAT_B8G8R8A8_UNORM, D2D1_ALPHA_MODE_PREMULTIPLIED));
            m_dc->CreateBitmap(D2D1::SizeU(c.coverW, c.coverH), c.cover->data(), c.coverW * 4, &bp, &m_cover);
        }
        m_layW = 0;
    }
    float target = s.look.full && (s.captionHeld || now < m_captionUntil) && !s.caption.title.empty() ? 1.f : 0.f;
    m_captionA = smooth_to(m_captionA, target, f.dt, 0.12f);
    if (target == 0 && m_captionA < 0.01f) m_captionA = 0;
    if (m_captionA > 0) caption(f, s, m_captionA, ts);
}

void Renderer::caption(Frame& f, const Snapshot& s, float a, float ts) {
    float m = 32 * ts, art = m_cover ? 72 * ts : 0, gap = art ? 16 * ts : 0;
    float textW = std::max(40.f, std::min(f.w * 0.6f, f.w - 2 * m - art - gap));
    if (textW != m_layW) {
        release(m_titleLay);
        release(m_detailLay);
        m_layW = textW;
        const VisCaption& c = s.caption;
        if (m_titleFmt) m_dw->CreateTextLayout(c.title.c_str(), (UINT32)c.title.size(), m_titleFmt, textW, 40 * ts, &m_titleLay);
        if (m_detailFmt && !c.detail.empty())
            m_dw->CreateTextLayout(c.detail.c_str(), (UINT32)c.detail.size(), m_detailFmt, textW, 30 * ts, &m_detailLay);
    }
    float bottom = f.h - m, top = bottom - std::max(art, 58 * ts);
    // (a shade under it: it reads on anything)
    ID2D1LinearGradientBrush* shade = linear(m_dc, { { 0, D2D1::ColorF(0, 0, 0, 0) }, { 1, D2D1::ColorF(0, 0, 0, 0.55f) } });
    if (shade) {
        shade->SetStartPoint({ 0, top - 60 * ts });
        shade->SetEndPoint({ 0, f.h });
        shade->SetOpacity(a);
        m_dc->FillRectangle(D2D1::RectF(0, top - 60 * ts, f.w, f.h), shade);
        shade->Release();
    }
    if (m_cover) {
        D2D1_SIZE_F cs = m_cover->GetSize();
        float k = std::min(art / cs.width, art / cs.height);
        float cw = cs.width * k, ch = cs.height * k;
        D2D1_RECT_F r = D2D1::RectF(m, bottom - ch, m + cw, bottom);
        m_dc->DrawBitmap(m_cover, &r, a, D2D1_INTERPOLATION_MODE_HIGH_QUALITY_CUBIC, nullptr, nullptr);
    }
    float x = m + art + gap;
    float ty = m_detailLay ? bottom - 58 * ts : bottom - 40 * ts;
    if (m_titleLay) {
        f.color(D2D1::ColorF(1, 1, 1, a));
        m_dc->DrawTextLayout({ x, ty }, m_titleLay, m_brush, D2D1_DRAW_TEXT_OPTIONS_CLIP);
    }
    if (m_detailLay) {
        f.color(D2D1::ColorF(0.8f, 0.8f, 0.8f, a));
        m_dc->DrawTextLayout({ x, ty + 32 * ts }, m_detailLay, m_brush, D2D1_DRAW_TEXT_OPTIONS_CLIP);
    }
}

} // namespace

void Visualizer::Impl::run() {
    Renderer r(wnd);
    bool gaveUp = false;
    for (;;) {
        Snapshot s;
        {
            std::unique_lock<std::mutex> lk(mx);
            cv.wait(lk, [&] { return stop || (shown && !gaveUp && (dirty || running || busy)); });
            if (stop) break;
            dirty = false;
            s.look = look;
            s.tap = tap;
            s.clockPos = clockPos;
            s.clockQpc = clockQpc;
            s.running = running;
            s.announceGen = announceGen;
            s.captionHeld = captionHeld;
            s.caption = caption;
            s.captionGen = captionGen;
        }
        if (!r.ready() && !r.create()) {
            gaveUp = true;
            failed = true;
            busy = false;
            PostMessageW(notify, failMsg, 0, 0);
            continue;
        }
        // (paced by the display: the next frame when the last is on its way)
        if (r.waitable()) WaitForSingleObjectEx(r.waitable(), 100, TRUE);
        bool more = false;
        if (!r.frame(s, more)) { busy = true; continue; }       // (the device lost: made again)
        busy = more;
    }
}

Visualizer::Visualizer() : d(std::make_unique<Impl>()) {}
Visualizer::~Visualizer() { destroy(); }

static LRESULT CALLBACK vis_proc(HWND hwnd, UINT msg, WPARAM wp, LPARAM lp) {
    switch (msg) {
        case WM_NCHITTEST: return HTTRANSPARENT;        // (the mouse is the video area's)
        case WM_ERASEBKGND: return 1;
        case WM_PAINT: {                                // (until the first frame: the background)
            PAINTSTRUCT ps;
            HDC dc = BeginPaint(hwnd, &ps);
            HBRUSH b = CreateSolidBrush((COLORREF)GetWindowLongPtrW(hwnd, 0));
            FillRect(dc, &ps.rcPaint, b);
            DeleteObject(b);
            EndPaint(hwnd, &ps);
            return 0;
        }
    }
    return DefWindowProcW(hwnd, msg, wp, lp);
}

bool Visualizer::create(HWND parent, HWND notify, UINT failMsg) {
    if (d->wnd) return true;
    static bool registered = false;
    if (!registered) {
        WNDCLASSEXW wc{ sizeof(wc) };
        wc.lpfnWndProc = vis_proc;
        wc.hInstance = GetModuleHandleW(nullptr);
        wc.lpszClassName = L"ExplorerPlusVisual";
        wc.cbWndExtra = sizeof(LONG_PTR);
        registered = RegisterClassExW(&wc) != 0;
    }
    d->wnd = CreateWindowExW(0, L"ExplorerPlusVisual", L"Visualization", WS_CHILD | WS_CLIPSIBLINGS, 0, 0, 0, 0, parent,
                             nullptr, GetModuleHandleW(nullptr), nullptr);
    if (!d->wnd) return false;
    d->notify = notify;
    d->failMsg = failMsg;
    Impl* impl = d.get();
    d->th = std::thread([impl] { impl->run(); });
    return true;
}

void Visualizer::destroy() {
    if (!d || !d->wnd) return;
    {
        std::lock_guard<std::mutex> lk(d->mx);
        d->stop = true;
        d->cv.notify_one();
    }
    if (d->th.joinable()) d->th.join();
    DestroyWindow(d->wnd);
    d->wnd = nullptr;
}

HWND Visualizer::hwnd() const { return d->wnd; }
bool Visualizer::failed() const { return d->failed; }

void Visualizer::place(const RECT* r) {
    if (!d->wnd) return;
    bool show = r && r->right > r->left && r->bottom > r->top;
    if (show == d->placed && (!show || EqualRect(r, &d->at))) return;
    d->placed = show;
    if (show) {
        d->at = *r;
        SetWindowPos(d->wnd, HWND_TOP, r->left, r->top, r->right - r->left, r->bottom - r->top, SWP_NOACTIVATE | SWP_SHOWWINDOW);
    } else {
        ShowWindow(d->wnd, SW_HIDE);
    }
    std::lock_guard<std::mutex> lk(d->mx);
    d->shown = show;
    d->wake();
}

void Visualizer::set_look(const VisLook& look) {
    if (d->wnd) SetWindowLongPtrW(d->wnd, 0, (LONG_PTR)(look.full ? RGB(0, 0, 0) : look.bg));
    std::lock_guard<std::mutex> lk(d->mx);
    if (look == d->look) return;
    d->look = look;
    d->wake();
}

void Visualizer::set_source(const SpectrumTap& tap) {
    std::lock_guard<std::mutex> lk(d->mx);
    if (tap == d->tap) return;
    d->tap = tap;
    d->wake();
}

void Visualizer::set_clock(LONGLONG pos, bool running) {
    LARGE_INTEGER q;
    QueryPerformanceCounter(&q);
    std::lock_guard<std::mutex> lk(d->mx);
    d->clockPos = pos;
    d->clockQpc = q.QuadPart;
    if (running != d->running) {
        d->running = running;
        d->wake();
    }
}

void Visualizer::set_caption(const VisCaption& c) {
    std::lock_guard<std::mutex> lk(d->mx);
    d->caption = c;
    ++d->captionGen;
    d->wake();
}

void Visualizer::show_caption(bool on) {
    std::lock_guard<std::mutex> lk(d->mx);
    if (on == d->captionHeld) return;
    d->captionHeld = on;
    d->wake();
}

void Visualizer::announce() {
    std::lock_guard<std::mutex> lk(d->mx);
    ++d->announceGen;
    d->wake();
}
