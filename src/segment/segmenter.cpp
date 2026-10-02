#ifndef NOMINMAX
#define NOMINMAX
#endif
#include "segmenter.h"

#include <algorithm>
#include <chrono>
#include <cmath>
#include <cstdarg>
#include <cstdio>
#include <cstring>
#include <deque>
#include <filesystem>
#include <limits>
#include <map>
#include <queue>

#include "objectclass.h"
#include "qwen_client.h"

namespace fs = std::filesystem;

namespace
{
// ============================================================================
// Logging: <debugDir>/segment.log
// ============================================================================
std::string g_log_path;

void SegLog(const char* fmt, ...)
{
    if (g_log_path.empty()) return;
    FILE* f = fopen(g_log_path.c_str(), "a");
    if (!f) return;
    va_list args;
    va_start(args, fmt);
    vfprintf(f, fmt, args);
    va_end(args);
    fclose(f);
}

// ============================================================================
// Small helpers
// ============================================================================
struct UnionFind
{
    std::vector<int32_t> parent;
    explicit UnionFind(size_t n) : parent(n) { for (size_t i = 0; i < n; i++) parent[i] = int32_t(i); }
    int32_t Find(int32_t x)
    {
        while (parent[x] != x) { parent[x] = parent[parent[x]]; x = parent[x]; }
        return x;
    }
    void Union(int32_t a, int32_t b)
    {
        a = Find(a); b = Find(b);
        if (a != b) parent[std::max(a, b)] = std::min(a, b);
    }
};

const float kInf = std::numeric_limits<float>::infinity();

inline uint8_t R8(uint32_t c) { return uint8_t(c >> 16); }
inline uint8_t G8(uint32_t c) { return uint8_t(c >> 8); }
inline uint8_t B8(uint32_t c) { return uint8_t(c); }

// sRGB 8-bit -> CIE Lab (D65)
void RgbToLab(uint8_t r8, uint8_t g8, uint8_t b8, float lab[3])
{
    auto lin = [](float c) { c /= 255.0f; return c <= 0.04045f ? c / 12.92f : powf((c + 0.055f) / 1.055f, 2.4f); };
    float r = lin(r8), g = lin(g8), b = lin(b8);
    float x = (0.4124f * r + 0.3576f * g + 0.1805f * b) / 0.95047f;
    float y = (0.2126f * r + 0.7152f * g + 0.0722f * b);
    float z = (0.0193f * r + 0.1192f * g + 0.9505f * b) / 1.08883f;
    auto f = [](float t) { return t > 0.008856f ? cbrtf(t) : 7.787f * t + 16.0f / 116.0f; };
    float fx = f(x), fy = f(y), fz = f(z);
    lab[0] = 116.0f * fy - 16.0f;
    lab[1] = 500.0f * (fx - fy);
    lab[2] = 200.0f * (fy - fz);
}

// ============================================================================
// Texture decoding (captured textures: DXT1/3/5 or RGBA8)
// ============================================================================
struct DecodedTexture
{
    int w = 0, h = 0;
    std::vector<uint32_t> rgb;   // 0x00RRGGBB
};

bool DecodeTexture(const core::Texture2DInfo* info, DecodedTexture& out)
{
    if (!info || !info->m_levelCount || !info->m_mips[0].m_imageData)
        return false;
    const uint32_t w = info->m_mips[0].m_width, h = info->m_mips[0].m_height;
    const uint8_t* src = reinterpret_cast<const uint8_t*>(info->m_mips[0].m_imageData.get());
    const uint32_t size = info->m_mips[0].m_size;
    if (!w || !h || w > 16384 || h > 16384)
        return false;
    out.w = int(w); out.h = int(h);
    out.rgb.assign(size_t(w) * h, 0);
    const uint32_t fmt = info->m_format;
    const uint32_t blocks = ((w + 3) / 4) * ((h + 3) / 4);
    if (fmt == 0x83F0 || fmt == 0x83F1)          // DXT1
    {
        if (size < blocks * 8) return false;
        core::Dxt1Convertor::DecodeDxt1Texture(out.rgb.data(), w, h, src);
    }
    else if (fmt == 0x83F2)                       // DXT3
    {
        if (size < blocks * 16) return false;
        core::DecodeDxt3Texture(out.rgb.data(), w, h, src);
    }
    else if (fmt == 0x83F3)                       // DXT5
    {
        if (size < blocks * 16) return false;
        core::DecodeDxt5Texture(out.rgb.data(), w, h, src);
    }
    else if (fmt == 0x1908 || fmt == 0x1907)      // RGBA / RGB bytes
    {
        const uint32_t ch = fmt == 0x1908 ? 4 : 3;
        if (size < size_t(w) * h * ch) return false;
        for (size_t i = 0; i < size_t(w) * h; i++)
            out.rgb[i] = (uint32_t(src[i * ch]) << 16) | (uint32_t(src[i * ch + 1]) << 8) | src[i * ch + 2];
    }
    else
        return false;
    for (uint32_t& c : out.rgb) c &= 0x00FFFFFF;
    return true;
}

// ============================================================================
// Ground model (digital terrain model) from the top-surface height map.
//
//   1. Candidates: the lowest surface point of each cell, at its true position
//      (so a sloped cell's minimum carries no bias).
//   2. Flood from the lowest candidate over neighbouring cells, accepting an
//      uphill step only within maxSlope * distance + tolerance: streets and
//      hills connect, roofs are cut off by their walls.
//   3. Per cell, a weighted local plane through the accepted points around it
//      (search widened under big buildings); pixels blend the planes of the
//      four nearest cell centres. Exact on planes, follows curved terrain.
//   4. Enclosed ground the flood missed (courtyards) is accepted if it lies on
//      that surface, and the planes are refitted.
// ============================================================================
void BuildGroundModel(const std::vector<float>& top, int W, int H, double res, double maxRadius,
                      std::vector<float>& ground)
{
    const float kNone = -std::numeric_limits<float>::infinity();
    const int c = std::max(2, int(round(5.0 / res)));          // 5 m cells
    const int cw = (W + c - 1) / c, ch = (H + c - 1) / c;
    const double maxSlope = 0.35, tolerance = 0.8;              // rise per metre, metres

    struct Cand { float x = 0, y = 0, z = 0; bool valid = false; bool ground = false; };
    std::vector<Cand> cand(size_t(cw) * ch);
    for (int y = 0; y < H; y++)
        for (int x = 0; x < W; x++)
        {
            float z = top[size_t(y) * W + x];
            if (z == kNone) continue;
            Cand& k = cand[size_t(y / c) * cw + x / c];
            if (!k.valid || z < k.z) { k.valid = true; k.x = x + 0.5f; k.y = y + 0.5f; k.z = z; }
        }

    // Pits (holes, tile seams, geometry below the surface): far lower than
    // every neighbour. They are not ground and must not seed the flood.
    int pits = 0;
    for (int cy = 0; cy < ch; cy++)
        for (int cx = 0; cx < cw; cx++)
        {
            Cand& k = cand[size_t(cy) * cw + cx];
            if (!k.valid) continue;
            float lowest_nb = std::numeric_limits<float>::infinity();
            int nbs = 0;
            for (int dy = -1; dy <= 1; dy++)
                for (int dx = -1; dx <= 1; dx++)
                {
                    int nx = cx + dx, ny = cy + dy;
                    if ((!dx && !dy) || nx < 0 || ny < 0 || nx >= cw || ny >= ch) continue;
                    const Cand& n = cand[size_t(ny) * cw + nx];
                    if (n.valid) { lowest_nb = std::min(lowest_nb, n.z); nbs++; }
                }
            if (nbs >= 3 && k.z < lowest_nb - 2.0f) { k.valid = false; pits++; }
        }

    // 2. Seeds: in every block (~2x the ground radius), the cells near the
    //    block's 10th-percentile height - street level in a city - rather than
    //    one global minimum, which a single outlier could capture. Then flood,
    //    lowest first.
    using QItem = std::pair<float, int>;
    std::priority_queue<QItem, std::vector<QItem>, std::greater<QItem>> pq;
    const int block = std::max(4, int(round(2.0 * maxRadius / (c * res))));
    int seeds = 0;
    std::vector<float> zs;
    for (int by = 0; by < ch; by += block)
        for (int bx = 0; bx < cw; bx += block)
        {
            zs.clear();
            for (int cy = by; cy < std::min(ch, by + block); cy++)
                for (int cx = bx; cx < std::min(cw, bx + block); cx++)
                    if (cand[size_t(cy) * cw + cx].valid) zs.push_back(cand[size_t(cy) * cw + cx].z);
            if (zs.size() < 4) continue;
            size_t p10 = zs.size() / 10;
            std::nth_element(zs.begin(), zs.begin() + p10, zs.end());
            const float level = zs[p10] + float(tolerance);
            for (int cy = by; cy < std::min(ch, by + block); cy++)
                for (int cx = bx; cx < std::min(cw, bx + block); cx++)
                {
                    Cand& k = cand[size_t(cy) * cw + cx];
                    if (k.valid && !k.ground && k.z <= level)
                    {
                        k.ground = true;
                        pq.push({ k.z, cy * cw + cx });
                        seeds++;
                    }
                }
        }
    if (seeds == 0) { ground.assign(size_t(W) * H, 0.0f); return; }
    while (!pq.empty())
    {
        int i = pq.top().second; pq.pop();
        int cx = i % cw, cy = i / cw;
        for (int dy = -1; dy <= 1; dy++)
            for (int dx = -1; dx <= 1; dx++)
            {
                int nx = cx + dx, ny = cy + dy;
                if ((!dx && !dy) || nx < 0 || ny < 0 || nx >= cw || ny >= ch) continue;
                Cand& n = cand[size_t(ny) * cw + nx];
                if (!n.valid || n.ground) continue;
                double d = hypot(n.x - cand[i].x, n.y - cand[i].y) * res;
                if (n.z - cand[i].z <= maxSlope * d + tolerance)
                {
                    n.ground = true;
                    pq.push({ n.z, ny * cw + nx });
                }
            }
    }

    // 3. Local planes per cell centre: z = a + b * (x - cx) + e * (y - cy), in metres.
    const int max_ring = std::max(2, int(ceil(maxRadius / (c * res))));
    std::vector<double> plane(size_t(cw) * ch * 3, 0.0);
    std::vector<uint8_t> has_plane(size_t(cw) * ch, 0);
    auto fit_planes = [&]() {
        double global_sum = 0.0; int global_n = 0;
        for (const Cand& k : cand) if (k.ground) { global_sum += k.z; global_n++; }
        const double global_mean = global_n ? global_sum / global_n : 0.0;
        for (int cy = 0; cy < ch; cy++)
            for (int cx = 0; cx < cw; cx++)
            {
                const double px = (cx + 0.5) * c, py = (cy + 0.5) * c;
                double S[3][3] = {}, B[3] = {};
                int n = 0;
                for (int ring = 2; ring <= max_ring; ring = ring < 4 ? ring + 1 : ring * 2)
                {
                    memset(S, 0, sizeof(S)); memset(B, 0, sizeof(B)); n = 0;
                    for (int yy = std::max(0, cy - ring); yy <= std::min(ch - 1, cy + ring); yy++)
                        for (int xx = std::max(0, cx - ring); xx <= std::min(cw - 1, cx + ring); xx++)
                        {
                            const Cand& k = cand[size_t(yy) * cw + xx];
                            if (!k.ground) continue;
                            double u = (k.x - px) * res, v = (k.y - py) * res;
                            double w = 1.0 / (1.0 + (u * u + v * v) / (25.0 * c * res * c * res));
                            double a[3] = { 1.0, u, v };
                            for (int r = 0; r < 3; r++) { B[r] += w * a[r] * k.z; for (int s = 0; s < 3; s++) S[r][s] += w * a[r] * a[s]; }
                            n++;
                        }
                    if (n >= 6) break;
                }
                double* p = &plane[(size_t(cy) * cw + cx) * 3];
                if (n == 0) { p[0] = global_mean; p[1] = p[2] = 0.0; has_plane[size_t(cy) * cw + cx] = 1; continue; }
                // Solve S * p = B (3x3, Cramer); fall back to the weighted mean if degenerate.
                double det = S[0][0] * (S[1][1] * S[2][2] - S[1][2] * S[2][1])
                           - S[0][1] * (S[1][0] * S[2][2] - S[1][2] * S[2][0])
                           + S[0][2] * (S[1][0] * S[2][1] - S[1][1] * S[2][0]);
                if (n >= 3 && fabs(det) > 1e-6 * (S[0][0] * S[1][1] * S[2][2] + 1e-12))
                {
                    for (int col = 0; col < 3; col++)
                    {
                        double M[3][3];
                        memcpy(M, S, sizeof(M));
                        for (int r = 0; r < 3; r++) M[r][col] = B[r];
                        p[col] = (M[0][0] * (M[1][1] * M[2][2] - M[1][2] * M[2][1])
                                - M[0][1] * (M[1][0] * M[2][2] - M[1][2] * M[2][0])
                                + M[0][2] * (M[1][0] * M[2][1] - M[1][1] * M[2][0])) / det;
                    }
                    // A plane extrapolated far from its points can run away; cap the slope.
                    double g = hypot(p[1], p[2]);
                    if (g > 0.6) { p[1] *= 0.6 / g; p[2] *= 0.6 / g; }
                }
                else
                {
                    p[0] = B[0] / S[0][0];
                    p[1] = p[2] = 0.0;
                }
                has_plane[size_t(cy) * cw + cx] = 1;
            }
    };
    auto evaluate = [&](double x, double y) {
        // Blend the planes of the four nearest cell centres.
        double fx = x / c - 0.5, fy = y / c - 0.5;
        int x0 = std::clamp(int(floor(fx)), 0, cw - 1), y0 = std::clamp(int(floor(fy)), 0, ch - 1);
        int x1 = std::min(x0 + 1, cw - 1), y1 = std::min(y0 + 1, ch - 1);
        double tx = std::clamp(fx - x0, 0.0, 1.0), ty = std::clamp(fy - y0, 0.0, 1.0);
        auto at = [&](int cx, int cy) {
            const double* p = &plane[(size_t(cy) * cw + cx) * 3];
            return p[0] + p[1] * (x - (cx + 0.5) * c) * res + p[2] * (y - (cy + 0.5) * c) * res;
        };
        return (1 - ty) * ((1 - tx) * at(x0, y0) + tx * at(x1, y0)) + ty * ((1 - tx) * at(x0, y1) + tx * at(x1, y1));
    };

    fit_planes();
    // Robust refit: drop accepted points well off the surface - roofs that
    // slipped in (above) and remaining pits (below).
    int dropped = 0;
    for (int pass = 0; pass < 2; pass++)
    {
        int n = 0;
        for (Cand& k : cand)
        {
            if (!k.ground) continue;
            double r = k.z - evaluate(k.x, k.y);
            if (r > 1.5 || r < -2.5) { k.ground = false; n++; }
        }
        dropped += n;
        if (!n) break;
        fit_planes();
    }
    // 4. Courtyards and other enclosed ground: accept candidates on the surface.
    int added = 0;
    for (Cand& k : cand)
        if (k.valid && !k.ground && k.z <= evaluate(k.x, k.y) + tolerance) { k.ground = true; added++; }
    if (added)
        fit_planes();

    int accepted = 0, valid = 0;
    for (const Cand& k : cand) { valid += k.valid; accepted += k.ground; }
    SegLog("  ground model: %d x %d cells of %.1f m, %d pits ignored, %d seeds, %d of %d cells ground "
           "(%d dropped as off-surface, %d enclosed added)\n",
           cw, ch, c * res, pits, seeds, accepted, valid, dropped, added);

    ground.assign(size_t(W) * H, 0.0f);
    for (int y = 0; y < H; y++)
        for (int x = 0; x < W; x++)
            ground[size_t(y) * W + x] = float(evaluate(x + 0.5, y + 0.5));
}

// ============================================================================
// Set-of-marks drawing
// ============================================================================
const uint8_t kDigits[10][7] = {
    { 0x0E, 0x11, 0x13, 0x15, 0x19, 0x11, 0x0E }, { 0x04, 0x0C, 0x04, 0x04, 0x04, 0x04, 0x0E },
    { 0x0E, 0x11, 0x01, 0x02, 0x04, 0x08, 0x1F }, { 0x1F, 0x02, 0x04, 0x02, 0x01, 0x11, 0x0E },
    { 0x02, 0x06, 0x0A, 0x12, 0x1F, 0x02, 0x02 }, { 0x1F, 0x10, 0x1E, 0x01, 0x01, 0x11, 0x0E },
    { 0x06, 0x08, 0x10, 0x1E, 0x11, 0x11, 0x0E }, { 0x1F, 0x01, 0x02, 0x04, 0x08, 0x08, 0x08 },
    { 0x0E, 0x11, 0x11, 0x0E, 0x11, 0x11, 0x0E }, { 0x0E, 0x11, 0x11, 0x0F, 0x01, 0x02, 0x0C },
};

void PutPixel(std::vector<uint8_t>& img, int T, int x, int y, uint8_t r, uint8_t g, uint8_t b)
{
    if (x < 0 || y < 0 || x >= T || y >= T) return;
    uint8_t* p = &img[(size_t(y) * T + x) * 3];
    p[0] = r; p[1] = g; p[2] = b;
}

// White number on a black box, centred at (cx, cy).
void DrawNumber(std::vector<uint8_t>& img, int T, int cx, int cy, int number)
{
    const int scale = 2, gw = 5 * scale, gh = 7 * scale, gap = scale;
    std::string s = std::to_string(number);
    int tw = int(s.size()) * (gw + gap) - gap, th = gh;
    int x0 = std::clamp(cx - tw / 2, 3, std::max(3, T - tw - 3));
    int y0 = std::clamp(cy - th / 2, 3, std::max(3, T - th - 3));
    for (int y = y0 - 3; y < y0 + th + 3; y++)
        for (int x = x0 - 3; x < x0 + tw + 3; x++)
            PutPixel(img, T, x, y, 0, 0, 0);
    for (size_t c = 0; c < s.size(); c++)
    {
        const uint8_t* glyph = kDigits[s[c] - '0'];
        int gx = x0 + int(c) * (gw + gap);
        for (int row = 0; row < 7; row++)
            for (int col = 0; col < 5; col++)
                if (glyph[row] & (0x10 >> col))
                    for (int dy = 0; dy < scale; dy++)
                        for (int dx = 0; dx < scale; dx++)
                            PutPixel(img, T, gx + col * scale + dx, y0 + row * scale + dy, 255, 255, 255);
    }
}

// Parses {"1":"B","2":"R"} or "1:B 2:R" into (number, letter) pairs;
// tolerant of extra text.
std::vector<std::pair<int, char>> ParseAnswer(const std::string& s)
{
    std::vector<std::pair<int, char>> out;
    size_t i = 0;
    while (i < s.size())
    {
        if (!isdigit(uint8_t(s[i]))) { i++; continue; }
        int num = 0;
        while (i < s.size() && isdigit(uint8_t(s[i]))) num = num * 10 + (s[i++] - '0');
        size_t j = i;
        while (j < s.size() && (s[j] == ' ' || s[j] == '\t' || s[j] == '"')) j++;
        if (j < s.size() && (s[j] == ':' || s[j] == '='))
        {
            j++;
            while (j < s.size() && (s[j] == ' ' || s[j] == '\t' || s[j] == '"')) j++;
            if (j < s.size() && isalpha(uint8_t(s[j])))
            {
                out.push_back({ num, s[j] });
                i = j + 1;
                continue;
            }
        }
        i = j;
    }
    return out;
}

// ============================================================================
// Region bookkeeping
// ============================================================================
struct Region
{
    bool    raised = false;
    int64_t area = 0;           // pixels
    double  sx = 0, sy = 0;     // pixel coordinate sums
    double  sr = 0, sg = 0, sb = 0;
    double  sh = 0;             // height above ground sum
    double  votes[kObjClassCount] = {};
    ObjectClass cls = kObjUnknown;
    int32_t object = -1;
};

ObjectClass FallbackClass(const Region& r, double m2PerPixel)
{
    double n = std::max<int64_t>(r.area, 1);
    double R = r.sr / n, G = r.sg / n, B = r.sb / n;
    double exg = 2.0 * G - R - B;                    // excess green
    double lum = 0.299 * R + 0.587 * G + 0.114 * B;
    double mx = std::max({ R, G, B }), mn = std::min({ R, G, B });
    double sat = mx > 0 ? (mx - mn) / mx : 0;
    double area_m2 = r.area * m2PerPixel;
    double h = r.sh / n;
    if (r.raised)
    {
        if (area_m2 < 20.0 && h < 2.6) return kObjCar;   // size first: cars come in any colour
        if (exg > 18.0) return kObjTree;
        return kObjBuilding;
    }
    if (exg > 18.0) return kObjPlants;
    // No water here: building shadows are dark and bluish too. Water comes
    // from the model, directly or through a similar labelled neighbour.
    if (lum < 100 && sat < 0.18) return kObjRoad;
    return kObjGround;
}

} // namespace

// ============================================================================
// Segmenter
// ============================================================================
Segmenter::~Segmenter()
{
    m_cancel = true;
    if (m_thread.joinable())
        m_thread.join();
}

bool Segmenter::Start(const std::vector<GroupMeshData*>& groups, const SegmentSettings& settings)
{
    if (m_running)
        return false;
    if (m_thread.joinable())
        m_thread.join();
    m_cancel = false;
    m_progress = 0.0f;
    m_result.reset();
    m_running = true;
    m_thread = std::thread(&Segmenter::Run, this, groups, settings);
    return true;
}

std::string Segmenter::Status() const
{
    std::lock_guard<std::mutex> lock(m_mutex);
    return m_status;
}

std::unique_ptr<SegmentResult> Segmenter::TakeResult()
{
    if (m_running)
        return nullptr;
    if (m_thread.joinable())
        m_thread.join();
    std::lock_guard<std::mutex> lock(m_mutex);
    return std::move(m_result);
}

void Segmenter::SetStatus(const std::string& s, float progress)
{
    {
        std::lock_guard<std::mutex> lock(m_mutex);
        m_status = s;
    }
    m_progress = progress;
    SegLog("[%5.1f%%] %s\n", progress * 100.0f, s.c_str());
}

void Segmenter::Run(std::vector<GroupMeshData*> groups, SegmentSettings st)
{
    auto result = std::make_unique<SegmentResult>();
    auto t_start = std::chrono::steady_clock::now();
    auto finish = [&](bool ok, const std::string& error) {
        result->ok = ok;
        result->error = error;
        double secs = std::chrono::duration<double>(std::chrono::steady_clock::now() - t_start).count();
        SegLog("=== finished in %.1f s: %s\n", secs, ok ? result->summary.c_str() : error.c_str());
        {
            std::lock_guard<std::mutex> lock(m_mutex);
            m_status = ok ? result->summary : ("Segmentation failed: " + error);
            m_result = std::move(result);
        }
        m_running = false;
    };

    std::error_code ec;
    fs::create_directories(st.debugDir, ec);
    g_log_path = (fs::path(st.debugDir) / "segment.log").string();
    SegLog("\n=== segmentation: model %s at %s, %.2f m/px, ground radius %.0f m, raised > %.1f m\n",
           st.model.c_str(), st.server.c_str(), st.metresPerPixel, st.groundRadius, st.raisedHeight);

    QwenClient qwen(st.server, st.model);
    if (st.useModel)
    {
        SetStatus("Checking model " + st.model + "...", 0.0f);
        std::string err;
        if (!qwen.CheckModel(err))
            return finish(false, err);
    }

    // ---------------------------------------------------------------------
    // 1. Collect triangle meshes
    // ---------------------------------------------------------------------
    SetStatus("Collecting triangles...", 0.01f);
    uint64_t tri_total = 0;
    core::bounds3d bounds;
    for (GroupMeshData* g : groups)
    {
        for (MeshData* m : g->meshes)
        {
            if (!m || !m->vertex_list || m->num_vertex <= 0 || m->draw_call_list.size() != 1 ||
                !m->draw_call_list[0].is_ge_mesh() || m->draw_call_list[0].get_index_count() < 3)
                continue;
            SegmentResult::MeshAssign a;
            a.group = g;
            a.mesh = m;
            a.triangleObject.assign(size_t(m->draw_call_list[0].get_index_count() / 3), -1);
            tri_total += a.triangleObject.size();
            result->assignments.push_back(std::move(a));
            if (m->bbox_ws.b_valid) bounds += m->bbox_ws;
        }
    }
    if (result->assignments.empty() || !bounds.b_valid)
        return finish(false, "No captured triangle meshes to segment.");
    SegLog("  %zu meshes, %llu triangles, extent %.1f x %.1f x %.1f m\n", result->assignments.size(),
           (unsigned long long)tri_total, bounds.GetDiagonal().x, bounds.GetDiagonal().y, bounds.GetDiagonal().z);

    double res = st.metresPerPixel;
    const double minX = bounds.bb_min.x, maxY = bounds.bb_max.y;
    const double spanX = bounds.bb_max.x - minX, spanY = maxY - bounds.bb_min.y;
    // Peak memory is ~60 bytes per orthophoto pixel: cap the pixel count.
    const double kMaxPixels = 40.0e6;
    const double pixels = (spanX / res) * (spanY / res);
    if (pixels > kMaxPixels)
    {
        res *= sqrt(pixels / kMaxPixels);
        SegLog("  area too large for %.2f m/px; using %.3f m/px\n", st.metresPerPixel, res);
    }
    const int W = std::max(1, int(ceil(spanX / res)));
    const int H = std::max(1, int(ceil(spanY / res)));
    const size_t N = size_t(W) * H;
    const double m2PerPixel = res * res;

    auto world = [&](const MeshData* m, uint32_t vi) {
        const core::vec3f& v = m->vertex_list[vi];
        return core::vec3d(v.x, v.y, v.z) + m->translation;
    };

    // ---------------------------------------------------------------------
    // 2. Top-down rasterization: colour, surface height, triangle id
    // ---------------------------------------------------------------------
    SetStatus("Rendering orthophoto " + std::to_string(W) + " x " + std::to_string(H) + " px...", 0.02f);
    std::vector<uint32_t> color(N, 0);
    std::vector<float> top(N, -kInf);
    {
        std::map<const core::Texture2DInfo*, DecodedTexture> textures;
        for (size_t ai = 0; ai < result->assignments.size(); ai++)
        {
            if (m_cancel) return finish(false, "Cancelled.");
            if ((ai & 63) == 0)
                m_progress = 0.02f + 0.10f * float(ai) / float(result->assignments.size());
            const SegmentResult::MeshAssign& a = result->assignments[ai];
            const MeshData* m = a.mesh;
            const DrawCallInfo& dc = m->draw_call_list[0];

            const DecodedTexture* tex = nullptr;
            if (m->uv_list && m->idx_in_texture_list < a.group->loaded_textures.size())
            {
                const core::Texture2DInfo* info = a.group->loaded_textures[m->idx_in_texture_list];
                auto it = textures.find(info);
                if (it == textures.end())
                {
                    DecodedTexture d;
                    if (!DecodeTexture(info, d)) d.w = 0;
                    it = textures.emplace(info, std::move(d)).first;
                }
                if (it->second.w > 0) tex = &it->second;
            }

            for (size_t t = 0; t < a.triangleObject.size(); t++)
            {
                uint32_t vi[3] = { dc.get_index(int(t * 3)), dc.get_index(int(t * 3 + 1)), dc.get_index(int(t * 3 + 2)) };
                if (vi[0] >= uint32_t(m->num_vertex) || vi[1] >= uint32_t(m->num_vertex) || vi[2] >= uint32_t(m->num_vertex))
                    continue;
                double sx[3], sy[3], z[3];
                for (int k = 0; k < 3; k++)
                {
                    core::vec3d p = world(m, vi[k]);
                    sx[k] = (p.x - minX) / res;
                    sy[k] = (maxY - p.y) / res;
                    z[k] = p.z;
                }
                double area2 = (sx[1] - sx[0]) * (sy[2] - sy[0]) - (sy[1] - sy[0]) * (sx[2] - sx[0]);
                if (fabs(area2) < 1e-9) continue;   // vertical (walls): invisible from above
                int x0 = std::max(0, int(floor(std::min({ sx[0], sx[1], sx[2] }))));
                int x1 = std::min(W - 1, int(ceil(std::max({ sx[0], sx[1], sx[2] }))));
                int y0 = std::max(0, int(floor(std::min({ sy[0], sy[1], sy[2] }))));
                int y1 = std::min(H - 1, int(ceil(std::max({ sy[0], sy[1], sy[2] }))));
                for (int py = y0; py <= y1; py++)
                {
                    double cy = py + 0.5;
                    for (int px = x0; px <= x1; px++)
                    {
                        double cx = px + 0.5;
                        double w0 = (sx[2] - sx[1]) * (cy - sy[1]) - (sy[2] - sy[1]) * (cx - sx[1]);
                        double w1 = (sx[0] - sx[2]) * (cy - sy[2]) - (sy[0] - sy[2]) * (cx - sx[2]);
                        double w2 = (sx[1] - sx[0]) * (cy - sy[0]) - (sy[1] - sy[0]) * (cx - sx[0]);
                        bool inside = area2 > 0 ? (w0 >= 0 && w1 >= 0 && w2 >= 0) : (w0 <= 0 && w1 <= 0 && w2 <= 0);
                        if (!inside) continue;
                        double b0 = w0 / area2, b1 = w1 / area2, b2 = w2 / area2;
                        float zz = float(b0 * z[0] + b1 * z[1] + b2 * z[2]);
                        size_t i = size_t(py) * W + px;
                        if (zz <= top[i]) continue;
                        top[i] = zz;
                        uint32_t c = 0x808080;
                        if (tex)
                        {
                            const core::vec2f& u0 = m->uv_list[vi[0]];
                            const core::vec2f& u1 = m->uv_list[vi[1]];
                            const core::vec2f& u2 = m->uv_list[vi[2]];
                            double u = b0 * u0.x + b1 * u1.x + b2 * u2.x;
                            double v = b0 * u0.y + b1 * u1.y + b2 * u2.y;
                            int tx = std::clamp(int(u * tex->w), 0, tex->w - 1);
                            int ty = std::clamp(int(v * tex->h), 0, tex->h - 1);
                            c = tex->rgb[size_t(ty) * tex->w + tx];
                        }
                        color[i] = c;
                    }
                }
            }
        }
    }
    size_t valid_px = 0;
    for (size_t i = 0; i < N; i++) if (top[i] > -kInf) valid_px++;
    SegLog("  orthophoto %d x %d at %.3f m/px, %.1f%% covered\n", W, H, res, 100.0 * valid_px / N);
    if (valid_px == 0)
        return finish(false, "Nothing visible from above.");

    // ---------------------------------------------------------------------
    // 3. Ground model: morphological opening (exact on planar slopes), then
    //    a light blur; height above ground per pixel.
    // ---------------------------------------------------------------------
    SetStatus("Estimating ground surface...", 0.13f);
    std::vector<float> ground;
    BuildGroundModel(top, W, H, res, st.groundRadius, ground);
    if (m_cancel) return finish(false, "Cancelled.");

    std::vector<float> hag(N, 0.0f);   // height above ground
    for (size_t i = 0; i < N; i++)
        hag[i] = top[i] > -kInf ? std::max(0.0f, top[i] - ground[i]) : 0.0f;

    // ---------------------------------------------------------------------
    // 4a. Raised regions: connected raised pixels without a height break
    // ---------------------------------------------------------------------
    SetStatus("Separating raised objects...", 0.17f);
    std::vector<int32_t> region(N, -1);
    std::vector<Region> regions;
    {
        std::vector<uint8_t> raised(N, 0);
        for (size_t i = 0; i < N; i++)
            raised[i] = top[i] > -kInf && hag[i] > st.raisedHeight;

        // Vegetation mask (excess green, box-smoothed over ~1 m): tree canopy
        // touching roofs at a similar height must not join them into one
        // region - the model labels a region by one number, and a canopy that
        // bridges a street would turn whole blocks into "tree".
        std::vector<uint8_t> veg(N, 0);
        {
            const int rad = std::max(1, int(1.0 / res));
            std::vector<float> exg(N, 0.0f), tmp(N, 0.0f);
            for (size_t i = 0; i < N; i++)
                if (top[i] > -kInf) exg[i] = 2.0f * G8(color[i]) - R8(color[i]) - B8(color[i]);
            for (int y = 0; y < H; y++)   // horizontal running sum
            {
                const float* row = &exg[size_t(y) * W];
                float s = 0.0f;
                for (int x = -rad; x < W; x++)
                {
                    if (x + rad < W) s += row[x + rad];
                    if (x - rad - 1 >= 0) s -= row[x - rad - 1];
                    if (x >= 0) tmp[size_t(y) * W + x] = s;
                }
            }
            const float norm = 1.0f / float((2 * rad + 1) * (2 * rad + 1));
            for (int x = 0; x < W; x++)   // vertical running sum
            {
                float s = 0.0f;
                for (int y = -rad; y < H; y++)
                {
                    if (y + rad < H) s += tmp[size_t(y + rad) * W + x];
                    if (y - rad - 1 >= 0) s -= tmp[size_t(y - rad - 1) * W + x];
                    if (y >= 0) veg[size_t(y) * W + x] = s * norm > 18.0f;
                }
            }
        }

        const float step = 1.5f;   // max height jump between neighbours of one object
        UnionFind uf(N);
        for (int y = 0; y < H; y++)
            for (int x = 0; x < W; x++)
            {
                size_t i = size_t(y) * W + x;
                if (!raised[i]) continue;
                if (x + 1 < W && raised[i + 1] && veg[i] == veg[i + 1] && fabs(top[i] - top[i + 1]) < step)
                    uf.Union(int32_t(i), int32_t(i + 1));
                if (y + 1 < H && raised[i + W] && veg[i] == veg[i + W] && fabs(top[i] - top[i + W]) < step)
                    uf.Union(int32_t(i), int32_t(i + W));
            }
        std::map<int32_t, int64_t> sizes;
        for (size_t i = 0; i < N; i++) if (raised[i]) sizes[uf.Find(int32_t(i))]++;
        const int64_t min_px = std::max<int64_t>(4, int64_t(3.0 / m2PerPixel));   // < 3 m^2: noise
        std::map<int32_t, int32_t> root_to_region;
        for (size_t i = 0; i < N; i++)
        {
            if (!raised[i]) continue;
            int32_t root = uf.Find(int32_t(i));
            if (sizes[root] < min_px) continue;   // left as ground
            auto it = root_to_region.find(root);
            if (it == root_to_region.end())
            {
                it = root_to_region.emplace(root, int32_t(regions.size())).first;
                regions.emplace_back();
                regions.back().raised = true;
            }
            region[i] = it->second;
        }
    }
    const int32_t num_raised = int32_t(regions.size());
    SegLog("  %d raised regions\n", num_raised);
    if (m_cancel) return finish(false, "Cancelled.");

    // ---------------------------------------------------------------------
    // 4b. Ground regions: SLIC superpixels on colour, made connected, then
    //     similar neighbours merged
    // ---------------------------------------------------------------------
    SetStatus("Segmenting ground surfaces...", 0.22f);
    {
        auto is_ground = [&](size_t i) { return top[i] > -kInf && region[i] < 0; };
        std::vector<float> lab(N * 3, 0.0f);
        for (size_t i = 0; i < N; i++)
            if (is_ground(i)) RgbToLab(R8(color[i]), G8(color[i]), B8(color[i]), &lab[i * 3]);

        const int S = std::max(4, int(6.0 / res));         // ~6 m superpixels
        const float m_compact = 10.0f;
        struct Center { float l, a, b, x, y; };
        std::vector<Center> centers;
        for (int y = S / 2; y < H; y += S)
            for (int x = S / 2; x < W; x += S)
            {
                size_t i = size_t(y) * W + x;
                if (is_ground(i)) centers.push_back({ lab[i * 3], lab[i * 3 + 1], lab[i * 3 + 2], float(x), float(y) });
            }
        std::vector<int32_t> slic(N, -1);
        std::vector<float> dist(N);
        const float inv_s2 = (m_compact * m_compact) / float(S * S);
        for (int iter = 0; iter < 5 && !m_cancel; iter++)
        {
            std::fill(dist.begin(), dist.end(), kInf);
            for (size_t k = 0; k < centers.size(); k++)
            {
                const Center& c = centers[k];
                int xa = std::max(0, int(c.x) - S), xb = std::min(W - 1, int(c.x) + S);
                int ya = std::max(0, int(c.y) - S), yb = std::min(H - 1, int(c.y) + S);
                for (int y = ya; y <= yb; y++)
                    for (int x = xa; x <= xb; x++)
                    {
                        size_t i = size_t(y) * W + x;
                        if (!is_ground(i)) continue;
                        float dl = lab[i * 3] - c.l, da = lab[i * 3 + 1] - c.a, db = lab[i * 3 + 2] - c.b;
                        float dx = x - c.x, dy = y - c.y;
                        float d = dl * dl + da * da + db * db + (dx * dx + dy * dy) * inv_s2;
                        if (d < dist[i]) { dist[i] = d; slic[i] = int32_t(k); }
                    }
            }
            std::vector<double> acc(centers.size() * 6, 0.0);
            for (int y = 0; y < H; y++)
                for (int x = 0; x < W; x++)
                {
                    size_t i = size_t(y) * W + x;
                    if (slic[i] < 0) continue;
                    double* a = &acc[size_t(slic[i]) * 6];
                    a[0] += lab[i * 3]; a[1] += lab[i * 3 + 1]; a[2] += lab[i * 3 + 2];
                    a[3] += x; a[4] += y; a[5] += 1;
                }
            for (size_t k = 0; k < centers.size(); k++)
            {
                const double* a = &acc[k * 6];
                if (a[5] > 0)
                    centers[k] = { float(a[0] / a[5]), float(a[1] / a[5]), float(a[2] / a[5]), float(a[3] / a[5]), float(a[4] / a[5]) };
            }
        }
        if (m_cancel) return finish(false, "Cancelled.");

        // Connected components of equal SLIC label (and stray ground pixels).
        std::vector<int32_t> comp(N, -1);
        std::vector<int64_t> comp_area;
        std::vector<double> comp_lab;
        {
            std::vector<int> stack;
            for (size_t s = 0; s < N; s++)
            {
                if (!is_ground(s) || comp[s] >= 0) continue;
                int32_t id = int32_t(comp_area.size());
                int32_t lbl = slic[s];
                comp_area.push_back(0);
                comp_lab.insert(comp_lab.end(), { 0.0, 0.0, 0.0 });
                stack.push_back(int(s));
                comp[s] = id;
                while (!stack.empty())
                {
                    int i = stack.back(); stack.pop_back();
                    comp_area[id]++;
                    comp_lab[size_t(id) * 3] += lab[size_t(i) * 3];
                    comp_lab[size_t(id) * 3 + 1] += lab[size_t(i) * 3 + 1];
                    comp_lab[size_t(id) * 3 + 2] += lab[size_t(i) * 3 + 2];
                    int x = i % W, y = i / W;
                    const int nb[4] = { x > 0 ? i - 1 : -1, x + 1 < W ? i + 1 : -1, y > 0 ? i - W : -1, y + 1 < H ? i + W : -1 };
                    for (int n : nb)
                        if (n >= 0 && comp[n] < 0 && is_ground(size_t(n)) && slic[n] == lbl) { comp[n] = id; stack.push_back(n); }
                }
            }
        }
        const size_t C = comp_area.size();
        for (size_t c = 0; c < C; c++)
            for (int k = 0; k < 3; k++) comp_lab[c * 3 + k] /= double(std::max<int64_t>(comp_area[c], 1));

        // Adjacency (shared boundary length) between components.
        std::map<std::pair<int32_t, int32_t>, int32_t> adjacency;
        for (int y = 0; y < H; y++)
            for (int x = 0; x < W; x++)
            {
                size_t i = size_t(y) * W + x;
                if (comp[i] < 0) continue;
                if (x + 1 < W && comp[i + 1] >= 0 && comp[i + 1] != comp[i])
                    adjacency[{ std::min(comp[i], comp[i + 1]), std::max(comp[i], comp[i + 1]) }]++;
                if (y + 1 < H && comp[i + W] >= 0 && comp[i + W] != comp[i])
                    adjacency[{ std::min(comp[i], comp[i + W]), std::max(comp[i], comp[i + W]) }]++;
            }
        auto lab_dist = [&](int32_t a, int32_t b) {
            double dl = comp_lab[size_t(a) * 3] - comp_lab[size_t(b) * 3];
            double da = comp_lab[size_t(a) * 3 + 1] - comp_lab[size_t(b) * 3 + 1];
            double db = comp_lab[size_t(a) * 3 + 2] - comp_lab[size_t(b) * 3 + 2];
            return sqrt(dl * dl + da * da + db * db);
        };

        // Merge similar neighbours (most similar first), then absorb pieces
        // below ~25 m^2 into their most similar neighbour.
        UnionFind uf(C);
        std::vector<std::pair<double, std::pair<int32_t, int32_t>>> edges;
        for (auto& e : adjacency) edges.push_back({ lab_dist(e.first.first, e.first.second), e.first });
        std::sort(edges.begin(), edges.end());
        const double kMergeDist = 10.0;   // road vs sidewalk is ~40, lane markings split roads at ~8
        for (auto& e : edges)
            if (e.first < kMergeDist) uf.Union(e.second.first, e.second.second);

        std::vector<int64_t> merged_area(C, 0);
        for (size_t c = 0; c < C; c++) merged_area[uf.Find(int32_t(c))] += comp_area[c];
        const int64_t small_px = int64_t(25.0 / m2PerPixel);
        for (auto& e : edges)
        {
            int32_t a = uf.Find(e.second.first), b = uf.Find(e.second.second);
            if (a == b) continue;
            if (merged_area[a] < small_px || merged_area[b] < small_px)
            {
                int64_t total = merged_area[a] + merged_area[b];
                uf.Union(a, b);
                merged_area[uf.Find(a)] = total;
            }
        }

        std::map<int32_t, int32_t> root_to_region;
        for (size_t i = 0; i < N; i++)
        {
            if (comp[i] < 0) continue;
            int32_t root = uf.Find(comp[i]);
            auto it = root_to_region.find(root);
            if (it == root_to_region.end())
            {
                it = root_to_region.emplace(root, int32_t(regions.size())).first;
                regions.emplace_back();
            }
            region[i] = it->second;
        }
    }
    SegLog("  %d ground regions\n", int(regions.size()) - num_raised);

    // Region statistics.
    for (int y = 0; y < H; y++)
        for (int x = 0; x < W; x++)
        {
            size_t i = size_t(y) * W + x;
            if (region[i] < 0) continue;
            Region& r = regions[size_t(region[i])];
            r.area++;
            r.sx += x; r.sy += y;
            r.sr += R8(color[i]); r.sg += G8(color[i]); r.sb += B8(color[i]);
            r.sh += hag[i];
        }

    // ---------------------------------------------------------------------
    // 5. Qwen labels numbered regions tile by tile
    // ---------------------------------------------------------------------
    const int T = st.tileSize;
    const int tiles_x = (W + T - 1) / T, tiles_y = (H + T - 1) / T;
    int tiles_done = 0, tiles_asked = 0, tiles_failed = 0;
    double model_seconds = 0.0;
    if (st.useModel)
    {
        std::vector<int64_t> tile_count(regions.size(), 0);
        std::vector<int32_t> touched;
        std::vector<uint8_t> img(size_t(T) * T * 3), marked(size_t(T) * T * 3);
        std::vector<int32_t> mark_of(regions.size(), 0);

        for (int ty = 0; ty < tiles_y && !m_cancel; ty++)
            for (int tx = 0; tx < tiles_x && !m_cancel; tx++)
            {
                tiles_done++;
                char status[128];
                snprintf(status, sizeof(status), "Qwen labelling tile %d / %d...", tiles_done, tiles_x * tiles_y);
                SetStatus(status, 0.28f + 0.67f * float(tiles_done - 1) / float(tiles_x * tiles_y));

                const int ox = tx * T, oy = ty * T;
                // Tile image and visible regions.
                touched.clear();
                size_t tile_valid = 0;
                for (int y = 0; y < T; y++)
                    for (int x = 0; x < T; x++)
                    {
                        int gx = ox + x, gy = oy + y;
                        uint8_t* p = &img[(size_t(y) * T + x) * 3];
                        if (gx >= W || gy >= H || top[size_t(gy) * W + gx] == -kInf) { p[0] = p[1] = p[2] = 0; continue; }
                        size_t i = size_t(gy) * W + gx;
                        p[0] = R8(color[i]); p[1] = G8(color[i]); p[2] = B8(color[i]);
                        tile_valid++;
                        int32_t r = region[i];
                        if (r < 0) continue;
                        if (tile_count[size_t(r)]++ == 0) touched.push_back(r);
                    }
                if (tile_valid < size_t(T) * T / 50)
                {
                    for (int32_t r : touched) tile_count[size_t(r)] = 0;
                    continue;
                }

                // Marks: raised first, then ground; biggest visible first.
                const int64_t min_mark = std::max<int64_t>(60, int64_t(6.0 / m2PerPixel));
                std::vector<int32_t> raised_marks, ground_marks;
                for (int32_t r : touched)
                    if (tile_count[size_t(r)] >= min_mark)
                        (regions[size_t(r)].raised ? raised_marks : ground_marks).push_back(r);
                auto by_size = [&](int32_t a, int32_t b) { return tile_count[size_t(a)] > tile_count[size_t(b)]; };
                std::sort(raised_marks.begin(), raised_marks.end(), by_size);
                std::sort(ground_marks.begin(), ground_marks.end(), by_size);
                if (raised_marks.size() > 60) raised_marks.resize(60);
                if (ground_marks.size() > 30) ground_marks.resize(30);
                std::vector<int32_t> marks = raised_marks;
                marks.insert(marks.end(), ground_marks.begin(), ground_marks.end());
                if (marks.empty())
                {
                    for (int32_t r : touched) tile_count[size_t(r)] = 0;
                    continue;
                }
                for (size_t k = 0; k < marks.size(); k++) mark_of[size_t(marks[k])] = int32_t(k + 1);

                // Marked image: outlines, and each number at the point deepest
                // inside its region (largest distance to the region border), so
                // it never sits on a neighbour - a road's centroid can lie beside
                // the road. Chamfer distance transform, regions limited to the tile.
                marked = img;
                auto region_at = [&](int x, int y) -> int32_t {
                    if (x < 0 || y < 0 || x >= T || y >= T || ox + x >= W || oy + y >= H) return -1;
                    return region[size_t(oy + y) * W + (ox + x)];
                };
                std::vector<uint16_t> depth(size_t(T) * T, 0);
                for (int y = 0; y < T; y++)
                    for (int x = 0; x < T; x++)
                    {
                        int32_t r = region_at(x, y);
                        if (r < 0) continue;
                        bool edge = region_at(x - 1, y) != r || region_at(x + 1, y) != r ||
                                    region_at(x, y - 1) != r || region_at(x, y + 1) != r;
                        if (edge && mark_of[size_t(r)])
                        {
                            if (regions[size_t(r)].raised) PutPixel(marked, T, x, y, 255, 230, 0);
                            else PutPixel(marked, T, x, y, 0, 230, 255);
                        }
                        depth[size_t(y) * T + x] = edge ? 1 : 0xFFFF;
                    }
                auto relax = [&](int x, int y, int nx, int ny, uint16_t cost) {
                    if (nx < 0 || ny < 0 || nx >= T || ny >= T) return;
                    uint16_t& d = depth[size_t(y) * T + x];
                    uint16_t nd = depth[size_t(ny) * T + nx];
                    if (nd != 0 && nd != 0xFFFF && nd + cost < d) d = uint16_t(nd + cost);
                };
                for (int y = 0; y < T; y++)
                    for (int x = 0; x < T; x++)
                        if (depth[size_t(y) * T + x] > 1)
                        { relax(x, y, x - 1, y, 3); relax(x, y, x, y - 1, 3); relax(x, y, x - 1, y - 1, 4); relax(x, y, x + 1, y - 1, 4); }
                for (int y = T - 1; y >= 0; y--)
                    for (int x = T - 1; x >= 0; x--)
                        if (depth[size_t(y) * T + x] > 1)
                        { relax(x, y, x + 1, y, 3); relax(x, y, x, y + 1, 3); relax(x, y, x + 1, y + 1, 4); relax(x, y, x - 1, y + 1, 4); }
                std::vector<int> best_px(marks.size(), -1);
                std::vector<uint16_t> best_depth(marks.size(), 0);
                for (int y = 0; y < T; y++)
                    for (int x = 0; x < T; x++)
                    {
                        int32_t r = region_at(x, y);
                        if (r < 0 || !mark_of[size_t(r)]) continue;
                        size_t k = size_t(mark_of[size_t(r)] - 1);
                        uint16_t d = depth[size_t(y) * T + x];
                        if (d != 0xFFFF && d > best_depth[k]) { best_depth[k] = d; best_px[k] = y * T + x; }
                    }
                for (size_t k = 0; k < marks.size(); k++)
                    if (best_px[k] >= 0) DrawNumber(marked, T, best_px[k] % T, best_px[k] / T, int(k + 1));

                // Prompt.
                char buf[256];
                std::string prompt =
                    "Image 1 is a top-down aerial orthophoto of a city";
                snprintf(buf, sizeof(buf), " (%.2f m per pixel, north is up).", res);
                prompt += buf;
                prompt += " Image 2 is the same picture with numbered regions: yellow outlines are raised objects, "
                          "cyan outlines are ground-level areas; each number is printed white on black inside its region.\n"
                          "3D data tells us which regions are raised:\n";
                if (!raised_marks.empty())
                {
                    snprintf(buf, sizeof(buf), "- Regions %d-%d are RAISED above the ground. Answer B=building, T=tree, C=car, O=other.\n",
                             1, int(raised_marks.size()));
                    prompt += buf;
                }
                if (!ground_marks.empty())
                {
                    snprintf(buf, sizeof(buf), "- Regions %d-%d are at GROUND level. Answer R=road, P=plants (grass, shrubs, low vegetation), "
                             "W=water, G=other ground (sidewalk, plaza, parking lot, bare soil, rail).\n",
                             int(raised_marks.size()) + 1, int(marks.size()));
                    prompt += buf;
                }
                prompt += "Answer with a JSON object that maps every region number to its letter, for example "
                          "{\"1\":\"B\",\"2\":\"T\",\"3\":\"R\"}.";

                // Structured output: one required property per mark, each limited to
                // the letters allowed for its kind - no prose, no impossible labels.
                std::string schema = "{\"type\":\"object\",\"properties\":{";
                std::string required;
                for (size_t k = 0; k < marks.size(); k++)
                {
                    bool raised = regions[size_t(marks[k])].raised;
                    std::string key = "\"" + std::to_string(k + 1) + "\"";
                    if (k) { schema += ","; required += ","; }
                    schema += key + (raised ? ":{\"enum\":[\"B\",\"T\",\"C\",\"O\"]}" : ":{\"enum\":[\"R\",\"P\",\"W\",\"G\"]}");
                    required += key;
                }
                schema += "},\"required\":[" + required + "]}";

                std::string answer, err;
                double secs = 0.0;
                QwenClient::Image images[2] = { { T, T, img.data() }, { T, T, marked.data() } };
                tiles_asked++;
                bool ok = qwen.Ask(prompt, { images[0], images[1] }, answer, err, &secs, schema);
                model_seconds += secs;

                snprintf(buf, sizeof(buf), "tile_%02d_%02d", ty, tx);
                WritePngRgb((fs::path(st.debugDir) / (std::string(buf) + "_marks.png")).string(), T, T, marked.data());
                if (!ok)
                {
                    tiles_failed++;
                    SegLog("  %s: %zu marks, model error after %.1f s: %s\n", buf, marks.size(), secs, err.c_str());
                    if (tiles_failed >= 3 && tiles_failed == tiles_asked)
                    {
                        for (int32_t r : touched) { tile_count[size_t(r)] = 0; mark_of[size_t(r)] = 0; }
                        return finish(false, "The model keeps failing: " + err);
                    }
                }
                else
                {
                    int accepted = 0;
                    for (auto& pr : ParseAnswer(answer))
                    {
                        if (pr.first < 1 || pr.first > int(marks.size())) continue;
                        Region& r = regions[size_t(marks[size_t(pr.first - 1)])];
                        char L = char(toupper(uint8_t(pr.second)));
                        ObjectClass cls = L == 'O' ? kObjUnknown : ObjectClassFromLetter(L);
                        bool fits = r.raised ? (L == 'O' || cls == kObjBuilding || cls == kObjTree || cls == kObjCar)
                                             : (cls == kObjRoad || cls == kObjPlants || cls == kObjWater || cls == kObjGround);
                        if (!fits) continue;
                        r.votes[cls] += double(tile_count[size_t(marks[size_t(pr.first - 1)])]);
                        accepted++;
                    }
                    SegLog("  %s: %zu marks (%zu raised), %d labels accepted, %.1f s\n    answer: %s\n",
                           buf, marks.size(), raised_marks.size(), accepted, secs, answer.c_str());
                }

                for (int32_t r : touched) { tile_count[size_t(r)] = 0; mark_of[size_t(r)] = 0; }
            }
        if (m_cancel) return finish(false, "Cancelled.");
    }

    // Final class per region: most-voted, else colour/geometry fallback.
    int voted = 0;
    for (Region& r : regions)
    {
        int best = -1;
        double best_v = 0.0;
        for (int c = 0; c < kObjClassCount; c++)
            if (r.votes[c] > best_v) { best_v = r.votes[c]; best = c; }
        if (best >= 0) { r.cls = ObjectClass(best); voted++; }
    }

    // Regions the model never saw (each tile marks only its biggest ones)
    // take the class of a labelled neighbour of the same kind and similar
    // colour - longest shared border first - spreading a few rings out.
    int inherited = 0;
    {
        const size_t RN = regions.size();
        std::vector<float> rlab(RN * 3, 0.0f);
        for (size_t k = 0; k < RN; k++)
        {
            const Region& r = regions[k];
            double n = double(std::max<int64_t>(r.area, 1));
            RgbToLab(uint8_t(r.sr / n), uint8_t(r.sg / n), uint8_t(r.sb / n), &rlab[k * 3]);
        }
        std::map<std::pair<int32_t, int32_t>, int32_t> border;
        for (int y = 0; y < H; y++)
            for (int x = 0; x < W; x++)
            {
                size_t i = size_t(y) * W + x;
                int32_t a = region[i];
                if (a < 0) continue;
                int32_t b = x + 1 < W ? region[i + 1] : -1;
                if (b >= 0 && b != a) border[{ std::min(a, b), std::max(a, b) }]++;
                b = y + 1 < H ? region[i + W] : -1;
                if (b >= 0 && b != a) border[{ std::min(a, b), std::max(a, b) }]++;
            }
        std::vector<std::vector<std::pair<int32_t, int32_t>>> nbrs(RN);   // (neighbour, border length)
        for (auto& e : border)
            if (regions[size_t(e.first.first)].raised == regions[size_t(e.first.second)].raised)
            {
                nbrs[size_t(e.first.first)].push_back({ e.first.second, e.second });
                nbrs[size_t(e.first.second)].push_back({ e.first.first, e.second });
            }
        const float kInheritDist = 15.0f;   // Lab distance; road vs sidewalk is ~40
        for (int ring = 0; ring < 8; ring++)
        {
            std::vector<std::pair<size_t, ObjectClass>> assign;
            for (size_t k = 0; k < RN; k++)
            {
                if (regions[k].cls != kObjUnknown || regions[k].votes[kObjUnknown] > 0) continue;
                int32_t best_len = 0;
                ObjectClass best_cls = kObjUnknown;
                for (auto& nb : nbrs[k])
                {
                    const Region& o = regions[size_t(nb.first)];
                    if (o.cls == kObjUnknown || nb.second <= best_len) continue;
                    float dl = rlab[k * 3] - rlab[size_t(nb.first) * 3];
                    float da = rlab[k * 3 + 1] - rlab[size_t(nb.first) * 3 + 1];
                    float db = rlab[k * 3 + 2] - rlab[size_t(nb.first) * 3 + 2];
                    if (sqrtf(dl * dl + da * da + db * db) > kInheritDist) continue;
                    best_len = nb.second;
                    best_cls = o.cls;
                }
                if (best_cls != kObjUnknown) assign.push_back({ k, best_cls });
            }
            if (assign.empty()) break;
            for (auto& a : assign) regions[a.first].cls = a.second;
            inherited += int(assign.size());
        }
    }

    int by_colour = 0;
    for (Region& r : regions)
        if (r.cls == kObjUnknown && r.votes[kObjUnknown] <= 0)
        {
            r.cls = FallbackClass(r, m2PerPixel);
            by_colour++;
        }
    SegLog("  %zu regions: %d labelled by the model, %d from similar labelled neighbours, %d by colour/geometry\n",
           regions.size(), voted, inherited, by_colour);

    // ---------------------------------------------------------------------
    // 6. Objects: one per raised region, one per ground class
    // ---------------------------------------------------------------------
    SetStatus("Building objects...", 0.96f);
    int counters[kObjClassCount] = {};
    int32_t class_object[kObjClassCount];
    std::fill(std::begin(class_object), std::end(class_object), -1);
    auto object_for_class = [&](ObjectClass cls) {
        if (class_object[cls] < 0)
        {
            class_object[cls] = int32_t(result->objects.size());
            SceneObject o;
            o.name = GetObjectClassInfo(cls).name;
            o.cls = cls;
            result->objects.push_back(o);
        }
        return class_object[cls];
    };
    for (Region& r : regions)
    {
        if (r.raised)
        {
            ObjectClass cls = r.cls == kObjUnknown ? kObjBuilding : r.cls;
            char name[64];
            snprintf(name, sizeof(name), "%s_%03d", GetObjectClassInfo(cls).name, ++counters[cls]);
            SceneObject o;
            o.name = name;
            o.cls = cls;
            r.object = int32_t(result->objects.size());
            result->objects.push_back(o);
        }
        else
            r.object = object_for_class(r.cls == kObjUnknown ? kObjGround : r.cls);
    }

    // Per-pixel lookups for triangles: raised regions grown slightly (walls
    // sit on their outline), ground regions filled everywhere (under trees,
    // under buildings).
    std::vector<int32_t> raised_map(N, -1), ground_map(N, -1);
    for (size_t i = 0; i < N; i++)
    {
        if (region[i] < 0) continue;
        (regions[size_t(region[i])].raised ? raised_map : ground_map)[i] = region[i];
    }
    {
        int grow = std::max(1, int(round(0.75 / res)));
        for (int it = 0; it < grow; it++)
        {
            std::vector<int32_t> next = raised_map;
            for (int y = 0; y < H; y++)
                for (int x = 0; x < W; x++)
                {
                    size_t i = size_t(y) * W + x;
                    if (raised_map[i] >= 0) continue;
                    if (x > 0 && raised_map[i - 1] >= 0) next[i] = raised_map[i - 1];
                    else if (x + 1 < W && raised_map[i + 1] >= 0) next[i] = raised_map[i + 1];
                    else if (y > 0 && raised_map[i - W] >= 0) next[i] = raised_map[i - W];
                    else if (y + 1 < H && raised_map[i + W] >= 0) next[i] = raised_map[i + W];
                }
            raised_map.swap(next);
        }
        std::queue<size_t> q;
        for (size_t i = 0; i < N; i++) if (ground_map[i] >= 0) q.push(i);
        while (!q.empty())
        {
            size_t i = q.front(); q.pop();
            int x = int(i % W), y = int(i / W);
            const int64_t nb[4] = { x > 0 ? int64_t(i) - 1 : -1, x + 1 < W ? int64_t(i) + 1 : -1,
                                    y > 0 ? int64_t(i) - W : -1, y + 1 < H ? int64_t(i) + W : -1 };
            for (int64_t n : nb)
                if (n >= 0 && ground_map[size_t(n)] < 0) { ground_map[size_t(n)] = ground_map[i]; q.push(size_t(n)); }
        }
    }

    // ---------------------------------------------------------------------
    // 7. Every triangle -> object
    // ---------------------------------------------------------------------
    SetStatus("Assigning triangles...", 0.97f);
    const float lift = float(st.raisedHeight * 0.4);
    const int search = std::max(2, int(round(1.5 / res)));
    size_t counts[kObjClassCount] = {};
    for (SegmentResult::MeshAssign& a : result->assignments)
    {
        const MeshData* m = a.mesh;
        const DrawCallInfo& dc = m->draw_call_list[0];
        for (size_t t = 0; t < a.triangleObject.size(); t++)
        {
            uint32_t vi[3] = { dc.get_index(int(t * 3)), dc.get_index(int(t * 3 + 1)), dc.get_index(int(t * 3 + 2)) };
            if (vi[0] >= uint32_t(m->num_vertex) || vi[1] >= uint32_t(m->num_vertex) || vi[2] >= uint32_t(m->num_vertex))
                continue;
            core::vec3d p0 = world(m, vi[0]), p1 = world(m, vi[1]), p2 = world(m, vi[2]);
            core::vec3d c = (p0 + p1 + p2) * (1.0 / 3.0);
            int px = std::clamp(int((c.x - minX) / res), 0, W - 1);
            int py = std::clamp(int((maxY - c.y) / res), 0, H - 1);
            size_t i = size_t(py) * W + px;
            float h = float(c.z) - ground[i];
            // Terrain hidden under an object (no ground samples there, so the
            // ground model is interpolated and may sit a little low): flat,
            // occluded from above and near the ground -> ground, not the object.
            core::vec3d n = cross(p1 - p0, p2 - p0);
            double nlen = length(n);
            bool flat = nlen > 0.0 && fabs(n.z) / nlen > 0.7;
            bool occluded = c.z < top[i] - 1.0;
            bool hidden_ground = flat && occluded && h < 2.0f;
            // Walls reach up even when their centre is low (car sides): judge
            // steep triangles by their highest corner.
            if (!flat)
                h = std::max({ h, float(p0.z) - ground[i], float(p1.z) - ground[i], float(p2.z) - ground[i] });
            int32_t r = -1;
            if (h > lift && !hidden_ground)
            {
                r = raised_map[i];
                for (int rad = 1; r < 0 && rad <= search; rad++)   // walls just outside the outline
                    for (int dy = -rad; dy <= rad && r < 0; dy++)
                        for (int dx = -rad; dx <= rad && r < 0; dx++)
                        {
                            int x = px + dx, y = py + dy;
                            if (x >= 0 && y >= 0 && x < W && y < H) r = raised_map[size_t(y) * W + x];
                        }
            }
            if (r < 0) r = ground_map[i];
            int32_t obj = r >= 0 ? regions[size_t(r)].object : object_for_class(kObjGround);
            a.triangleObject[t] = obj;
            counts[result->objects[size_t(obj)].cls]++;
        }
    }

    // Debug images: orthophoto and class map (downsampled to <= 4096 px).
    {
        int step = std::max(1, (std::max(W, H) + 4095) / 4096);
        int dw = (W + step - 1) / step, dh = (H + step - 1) / step;
        std::vector<uint8_t> ortho(size_t(dw) * dh * 3), classes(size_t(dw) * dh * 3), regs(size_t(dw) * dh * 3),
                             heights(size_t(dw) * dh * 3);
        for (int y = 0; y < dh; y++)
            for (int x = 0; x < dw; x++)
            {
                size_t i = size_t(y * step) * W + size_t(x * step);
                uint8_t* o = &ortho[(size_t(y) * dw + x) * 3];
                uint8_t* c = &classes[(size_t(y) * dw + x) * 3];
                o[0] = R8(color[i]); o[1] = G8(color[i]); o[2] = B8(color[i]);
                c[0] = c[1] = c[2] = 0;
                uint8_t* rg = &regs[(size_t(y) * dw + x) * 3];
                uint32_t hsh = region[i] < 0 ? 0u : uint32_t(region[i] + 1) * 2654435761u;
                rg[0] = uint8_t(hsh >> 24); rg[1] = uint8_t(hsh >> 16); rg[2] = uint8_t(hsh >> 8);
                // Height above ground: raised = grey by height (white >= 25 m), ground = dark blue.
                uint8_t* hg = &heights[(size_t(y) * dw + x) * 3];
                if (top[i] == -kInf) { hg[0] = hg[1] = hg[2] = 0; }
                else if (hag[i] > st.raisedHeight)
                {
                    uint8_t v = uint8_t(std::min(255.0f, 60.0f + hag[i] * 7.8f));
                    hg[0] = hg[1] = hg[2] = v;
                }
                else { hg[0] = 10; hg[1] = 25; hg[2] = uint8_t(70 + 100.0 * hag[i] / st.raisedHeight); }
                if (region[i] >= 0)
                {
                    const float* col = GetObjectClassInfo(result->objects[size_t(regions[size_t(region[i])].object)].cls).color;
                    c[0] = uint8_t(col[0] * 255); c[1] = uint8_t(col[1] * 255); c[2] = uint8_t(col[2] * 255);
                }
            }
        WritePngRgb((fs::path(st.debugDir) / "orthophoto.png").string(), dw, dh, ortho.data());
        WritePngRgb((fs::path(st.debugDir) / "classes.png").string(), dw, dh, classes.data());
        WritePngRgb((fs::path(st.debugDir) / "regions.png").string(), dw, dh, regs.data());
        WritePngRgb((fs::path(st.debugDir) / "height_above_ground.png").string(), dw, dh, heights.data());
    }

    char summary[512];
    int instances[kObjClassCount] = {};
    for (const SceneObject& o : result->objects) instances[o.cls]++;
    snprintf(summary, sizeof(summary),
             "Segmented %zu objects: %d buildings, %d trees, %d cars + road/plants/water/ground areas "
             "(%d tiles, %.0f s in the model%s)",
             result->objects.size(), instances[kObjBuilding], instances[kObjTree], instances[kObjCar],
             tiles_asked, model_seconds, tiles_failed ? ", some tiles failed - see segment.log" : "");
    result->summary = summary;
    SegLog("  triangles: ground %zu, road %zu, building %zu, car %zu, tree %zu, plants %zu, water %zu\n",
           counts[kObjGround], counts[kObjRoad], counts[kObjBuilding], counts[kObjCar], counts[kObjTree],
           counts[kObjPlants], counts[kObjWater]);
    m_progress = 1.0f;
    finish(true, "");
}

// ============================================================================
// Mesh splitting
// ============================================================================
std::vector<MeshData*> ApplySegmentation(const SegmentResult& result)
{
    std::vector<MeshData*> replaced;
    std::map<GroupMeshData*, std::map<int32_t, int32_t>> local_ids;   // global object -> group object

    // Segmentation re-labels the whole group: forget earlier objects.
    for (const SegmentResult::MeshAssign& a : result.assignments)
    {
        if (local_ids.count(a.group)) continue;
        local_ids[a.group];
        a.group->objects.clear();
        for (MeshData* m : a.group->meshes) if (m) m->object_id = -1;
    }

    for (const SegmentResult::MeshAssign& a : result.assignments)
    {
        GroupMeshData* g = a.group;
        MeshData* m = a.mesh;
        auto pos = std::find(g->meshes.begin(), g->meshes.end(), m);
        if (pos == g->meshes.end()) continue;
        const DrawCallInfo& dc = m->draw_call_list[0];

        std::map<int32_t, std::vector<uint32_t>> parts;   // object -> triangles
        for (size_t t = 0; t < a.triangleObject.size(); t++)
            parts[a.triangleObject[t]].push_back(uint32_t(t));

        std::vector<MeshData*> pieces;
        for (auto& part : parts)
        {
            int32_t local = -1;
            if (part.first >= 0)
            {
                auto& ids = local_ids[g];
                auto it = ids.find(part.first);
                if (it == ids.end())
                {
                    it = ids.emplace(part.first, int32_t(g->objects.size())).first;
                    g->objects.push_back(result.objects[size_t(part.first)]);
                    g->objects.back().bbox_ws.Reset();
                }
                local = it->second;
            }

            std::vector<int32_t> remap(size_t(m->num_vertex), -1);
            std::vector<uint32_t> used;
            std::vector<uint32_t> indices;
            indices.reserve(part.second.size() * 3);
            for (uint32_t t : part.second)
                for (int k = 0; k < 3; k++)
                {
                    uint32_t v = dc.get_index(int(t * 3 + k));
                    if (v >= uint32_t(m->num_vertex)) v = 0;
                    if (remap[v] < 0) { remap[v] = int32_t(used.size()); used.push_back(v); }
                    indices.push_back(uint32_t(remap[v]));
                }

            MeshData* p = new MeshData;
            p->num_vertex = int32_t(used.size());
            p->idx_in_texture_list = m->idx_in_texture_list;
            p->translation = m->translation;
            p->dumpped_matrix = m->dumpped_matrix;
            p->object_id = local;
            p->vertex_list = std::make_unique<core::vec3f[]>(used.size());
            if (m->uv_list) p->uv_list = std::make_unique<core::vec2f[]>(used.size());
            if (m->color_list) p->color_list = std::make_unique<uint32_t[]>(used.size());
            p->bbox_ws.Reset();
            for (size_t k = 0; k < used.size(); k++)
            {
                p->vertex_list[k] = m->vertex_list[used[k]];
                if (p->uv_list) p->uv_list[k] = m->uv_list[used[k]];
                if (p->color_list) p->color_list[k] = m->color_list[used[k]];
                const core::vec3f& v = p->vertex_list[k];
                p->bbox_ws += core::vec3d(v.x, v.y, v.z) + p->translation;
            }
            p->add_draw_call_list(kGlTriangles, int32_t(indices.size()), int32_t(used.size()));
            DrawCallInfo& pdc = p->get_last_draw_call_info();
            for (uint32_t idx : indices) pdc.add_index(idx);
            if (local >= 0 && p->bbox_ws.b_valid)
                g->objects[size_t(local)].bbox_ws += p->bbox_ws;
            pieces.push_back(p);
        }

        size_t at = size_t(pos - g->meshes.begin());
        g->meshes.erase(g->meshes.begin() + at);
        g->meshes.insert(g->meshes.begin() + at, pieces.begin(), pieces.end());
        replaced.push_back(m);
    }
    return replaced;
}
