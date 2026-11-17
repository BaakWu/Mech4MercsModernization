// Sharper upscaling of the 800x600 front end (issue #20).
//
// The shell is drawn at 800x600 and stretched into the window. GDI's
// HALFTONE stretch is soft; this separable Lanczos-3 filter keeps text and
// edges crisper without adding anything to the picture (same pixels, better
// interpolation). 32-bit BGRX in and out, SSE float arithmetic, weight tables
// rebuilt only when a size changes.
#include <windows.h>

#include <emmintrin.h>

#include <cmath>
#include <vector>

namespace mw4scale {
namespace {

constexpr int kTaps = 6;  // Lanczos-3

struct Axis {
    int src = 0;
    int dst = 0;
    std::vector<int> index;     // dst * kTaps source indices (clamped)
    std::vector<float> weight;  // dst * kTaps weights (sum 1)
};

double Lanczos3(double t) {
    t = std::fabs(t);
    if (t < 1e-8) {
        return 1.0;
    }
    if (t >= 3.0) {
        return 0.0;
    }
    const double pi_t = 3.14159265358979323846 * t;
    return 3.0 * std::sin(pi_t) * std::sin(pi_t / 3.0) / (pi_t * pi_t);
}

void BuildAxis(Axis& axis, int src, int dst) {
    if (axis.src == src && axis.dst == dst) {
        return;
    }
    axis.src = src;
    axis.dst = dst;
    axis.index.assign(static_cast<size_t>(dst) * kTaps, 0);
    axis.weight.assign(static_cast<size_t>(dst) * kTaps, 0.0f);
    const double scale = static_cast<double>(src) / dst;
    for (int d = 0; d < dst; ++d) {
        const double center = (d + 0.5) * scale - 0.5;
        const int first = static_cast<int>(std::floor(center)) - (kTaps / 2 - 1);
        double sum = 0.0;
        double w[kTaps];
        for (int k = 0; k < kTaps; ++k) {
            w[k] = Lanczos3(center - (first + k));
            sum += w[k];
        }
        for (int k = 0; k < kTaps; ++k) {
            int i = first + k;
            i = i < 0 ? 0 : (i >= src ? src - 1 : i);
            axis.index[static_cast<size_t>(d) * kTaps + k] = i;
            axis.weight[static_cast<size_t>(d) * kTaps + k] = static_cast<float>(w[k] / sum);
        }
    }
}

Axis g_x;
Axis g_y;
std::vector<__m128> g_row;   // one source row as float BGRX
std::vector<__m128> g_temp;  // src_h rows of dst_w pixels, horizontally filtered

}  // namespace

// Scales src (sw x sh, 32-bit, pitch in bytes) to dst (dw x dh). Returns
// false if the sizes are unusable.
bool ScaleLanczos(const unsigned char* src, int src_pitch, int sw, int sh, unsigned char* dst,
                  int dst_pitch, int dw, int dh) {
    if (sw <= 0 || sh <= 0 || dw <= 0 || dh <= 0 || sw > 8192 || sh > 8192 || dw > 8192 ||
        dh > 8192) {
        return false;
    }
    BuildAxis(g_x, sw, dw);
    BuildAxis(g_y, sh, dh);
    g_row.resize(static_cast<size_t>(sw));
    g_temp.resize(static_cast<size_t>(sh) * dw);

    // Horizontal pass: every source row to dst_w pixels.
    for (int y = 0; y < sh; ++y) {
        const unsigned char* in = src + static_cast<size_t>(y) * src_pitch;
        const __m128i zero = _mm_setzero_si128();
        for (int x = 0; x < sw; ++x) {
            __m128i p = _mm_cvtsi32_si128(*reinterpret_cast<const int*>(in + x * 4));
            p = _mm_unpacklo_epi16(_mm_unpacklo_epi8(p, zero), zero);
            g_row[x] = _mm_cvtepi32_ps(p);
        }
        __m128* out = &g_temp[static_cast<size_t>(y) * dw];
        const int* index = g_x.index.data();
        const float* weight = g_x.weight.data();
        for (int x = 0; x < dw; ++x, index += kTaps, weight += kTaps) {
            __m128 acc = _mm_mul_ps(g_row[index[0]], _mm_set1_ps(weight[0]));
            for (int k = 1; k < kTaps; ++k) {
                acc = _mm_add_ps(acc, _mm_mul_ps(g_row[index[k]], _mm_set1_ps(weight[k])));
            }
            out[x] = acc;
        }
    }

    // Vertical pass, clamped to 0..255 and packed back to bytes.
    const __m128 lo = _mm_setzero_ps();
    const __m128 hi = _mm_set1_ps(255.0f);
    for (int y = 0; y < dh; ++y) {
        const int* index = &g_y.index[static_cast<size_t>(y) * kTaps];
        const float* weight = &g_y.weight[static_cast<size_t>(y) * kTaps];
        const __m128* rows[kTaps];
        __m128 w[kTaps];
        for (int k = 0; k < kTaps; ++k) {
            rows[k] = &g_temp[static_cast<size_t>(index[k]) * dw];
            w[k] = _mm_set1_ps(weight[k]);
        }
        unsigned int* out = reinterpret_cast<unsigned int*>(dst + static_cast<size_t>(y) * dst_pitch);
        for (int x = 0; x < dw; ++x) {
            __m128 acc = _mm_mul_ps(rows[0][x], w[0]);
            for (int k = 1; k < kTaps; ++k) {
                acc = _mm_add_ps(acc, _mm_mul_ps(rows[k][x], w[k]));
            }
            acc = _mm_min_ps(_mm_max_ps(acc, lo), hi);
            __m128i v = _mm_cvtps_epi32(acc);
            v = _mm_packs_epi32(v, v);
            v = _mm_packus_epi16(v, v);
            out[x] = static_cast<unsigned int>(_mm_cvtsi128_si32(v)) | 0xFF000000u;
        }
    }
    return true;
}

}  // namespace mw4scale
