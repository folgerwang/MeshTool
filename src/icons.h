#pragma once

// Embedded 32x32 RGBA icon pixel data for toolbar buttons.
// Each icon is a 32x32 grid drawn procedurally at init time.
// This avoids external file dependencies.

#include <cstdint>
#include <cstring>
#include <cmath>

namespace icons {

static const int ICON_SIZE = 48;
static const int ICON_BYTES = ICON_SIZE * ICON_SIZE * 4;

// Helper: set pixel in RGBA buffer
inline void PutPixel(uint8_t* buf, int x, int y, uint8_t r, uint8_t g, uint8_t b, uint8_t a = 255)
{
    if (x < 0 || x >= ICON_SIZE || y < 0 || y >= ICON_SIZE) return;
    int idx = (y * ICON_SIZE + x) * 4;
    buf[idx+0] = r; buf[idx+1] = g; buf[idx+2] = b; buf[idx+3] = a;
}

inline void FillRect(uint8_t* buf, int x0, int y0, int x1, int y1, uint8_t r, uint8_t g, uint8_t b, uint8_t a = 255)
{
    for (int y = y0; y <= y1; y++)
        for (int x = x0; x <= x1; x++)
            PutPixel(buf, x, y, r, g, b, a);
}

inline void FillCircle(uint8_t* buf, int cx, int cy, int radius, uint8_t r, uint8_t g, uint8_t b, uint8_t a = 255)
{
    for (int y = -radius; y <= radius; y++)
        for (int x = -radius; x <= radius; x++)
            if (x*x + y*y <= radius*radius)
                PutPixel(buf, cx+x, cy+y, r, g, b, a);
}

inline void DrawLine(uint8_t* buf, int x0, int y0, int x1, int y1, uint8_t r, uint8_t g, uint8_t b, int thickness = 1)
{
    int dx = abs(x1-x0), dy = abs(y1-y0);
    int sx = x0 < x1 ? 1 : -1, sy = y0 < y1 ? 1 : -1;
    int err = dx - dy;
    while (true)
    {
        for (int t = -thickness/2; t <= thickness/2; t++)
        {
            PutPixel(buf, x0+t, y0, r, g, b);
            PutPixel(buf, x0, y0+t, r, g, b);
        }
        if (x0 == x1 && y0 == y1) break;
        int e2 = 2*err;
        if (e2 > -dy) { err -= dy; x0 += sx; }
        if (e2 <  dx) { err += dx; y0 += sy; }
    }
}

inline void DrawTriangle(uint8_t* buf, int x0, int y0, int x1, int y1, int x2, int y2, uint8_t r, uint8_t g, uint8_t b)
{
    // Filled triangle using scanline
    auto swap = [](int& a, int& b_) { int t = a; a = b_; b_ = t; };
    int ya = y0, yb = y1, yc = y2;
    int xa = x0, xb = x1, xc = x2;
    if (ya > yb) { swap(ya, yb); swap(xa, xb); }
    if (ya > yc) { swap(ya, yc); swap(xa, xc); }
    if (yb > yc) { swap(yb, yc); swap(xb, xc); }

    for (int y = ya; y <= yc; y++)
    {
        float t1 = (yc != ya) ? (float)(y - ya) / (yc - ya) : 0;
        int xstart = xa + (int)(t1 * (xc - xa));
        int xend;
        if (y < yb)
        {
            float t2 = (yb != ya) ? (float)(y - ya) / (yb - ya) : 0;
            xend = xa + (int)(t2 * (xb - xa));
        }
        else
        {
            float t2 = (yc != yb) ? (float)(y - yb) / (yc - yb) : 0;
            xend = xb + (int)(t2 * (xc - xb));
        }
        if (xstart > xend) swap(xstart, xend);
        for (int x = xstart; x <= xend; x++)
            PutPixel(buf, x, y, r, g, b);
    }
}

// ---- Icon generators (all use S = ICON_SIZE, coordinates are proportional) ----
#define S ICON_SIZE
#define P(x32) ((x32) * S / 32)  // scale a 32-based coordinate to current ICON_SIZE

inline void GenLaunchIcon(uint8_t* buf)
{
    memset(buf, 0, ICON_BYTES);
    DrawTriangle(buf, P(10), P(6), P(10), P(25), P(26), P(16), 100, 240, 120);
}

inline void GenCaptureIcon(uint8_t* buf)
{
    memset(buf, 0, ICON_BYTES);
    FillCircle(buf, P(16), P(16), P(12), 100, 200, 255);
    FillCircle(buf, P(16), P(16), P(7), 20, 22, 28);
    FillCircle(buf, P(16), P(16), P(4), 100, 200, 255);
}

inline void GenStopIcon(uint8_t* buf)
{
    memset(buf, 0, ICON_BYTES);
    FillRect(buf, P(8), P(8), P(23), P(23), 240, 70, 70);
}

inline void GenGlobeIcon(uint8_t* buf)
{
    memset(buf, 0, ICON_BYTES);
    FillCircle(buf, P(16), P(16), P(13), 70, 150, 230);
    for (int x = P(4); x < P(28); x++) { PutPixel(buf, x, P(10), 50, 100, 180); PutPixel(buf, x, P(16), 50, 100, 180); PutPixel(buf, x, P(22), 50, 100, 180); }
    for (int y = P(4); y < P(28); y++) { PutPixel(buf, P(12), y, 50, 100, 180); PutPixel(buf, P(20), y, 50, 100, 180); }
    FillRect(buf, P(10), P(11), P(15), P(15), 100, 210, 100);
    FillRect(buf, P(17), P(8), P(22), P(13), 100, 210, 100);
    FillRect(buf, P(12), P(18), P(18), P(22), 100, 210, 100);
}

inline void GenTerrainIcon(uint8_t* buf)
{
    memset(buf, 0, ICON_BYTES);
    DrawTriangle(buf, P(4), P(26), P(16), P(6), P(28), P(26), 160, 120, 70);
    DrawTriangle(buf, P(14), P(26), P(22), P(14), P(30), P(26), 130, 95, 55);
    DrawTriangle(buf, P(13), P(10), P(16), P(6), P(19), P(10), 240, 245, 255);
}

inline void GenSplineIcon(uint8_t* buf)
{
    memset(buf, 0, ICON_BYTES);
    for (int i = 0; i < P(28); i++)
    {
        float t = (float)i / (float)(P(28) - 1);
        int x = P(3) + i;
        int y = P(16) + (int)(P(8) * sinf(t * 3.14159f * 1.5f));
        PutPixel(buf, x, y, 240, 180, 60);
        PutPixel(buf, x, y+1, 240, 180, 60);
        PutPixel(buf, x+1, y, 240, 180, 60);
    }
    FillCircle(buf, P(6), P(14), P(3), 255, 220, 80);
    FillCircle(buf, P(16), P(23), P(3), 255, 220, 80);
    FillCircle(buf, P(26), P(10), P(3), 255, 220, 80);
}

inline void GenImportIcon(uint8_t* buf)
{
    memset(buf, 0, ICON_BYTES);
    FillRect(buf, P(6), P(8), P(25), P(10), 180, 190, 210);
    FillRect(buf, P(6), P(22), P(25), P(24), 180, 190, 210);
    FillRect(buf, P(6), P(8), P(8), P(24), 180, 190, 210);
    FillRect(buf, P(23), P(8), P(25), P(24), 180, 190, 210);
    DrawLine(buf, P(16), P(2), P(16), P(18), 100, 230, 100, P(2));
    DrawTriangle(buf, P(11), P(15), P(16), P(21), P(21), P(15), 100, 230, 100);
}

inline void GenExportIcon(uint8_t* buf)
{
    memset(buf, 0, ICON_BYTES);
    FillRect(buf, P(6), P(14), P(25), P(16), 240, 180, 60);
    FillRect(buf, P(6), P(26), P(25), P(28), 240, 180, 60);
    FillRect(buf, P(6), P(14), P(8), P(28), 240, 180, 60);
    FillRect(buf, P(23), P(14), P(25), P(28), 240, 180, 60);
    DrawLine(buf, P(16), P(4), P(16), P(20), 255, 200, 70, P(2));
    DrawTriangle(buf, P(11), P(9), P(16), P(3), P(21), P(9), 255, 200, 70);
}

inline void GenPinIcon(uint8_t* buf)
{
    memset(buf, 0, ICON_BYTES);
    FillCircle(buf, P(16), P(12), P(8), 220, 80, 220);
    FillCircle(buf, P(16), P(12), P(4), 255, 160, 255);
    DrawTriangle(buf, P(12), P(18), P(16), P(28), P(20), P(18), 220, 80, 220);
}

inline void GenRegionIcon(uint8_t* buf)
{
    memset(buf, 0, ICON_BYTES);
    for (int i = P(4); i < P(28); i += P(3))
    {
        FillRect(buf, i, P(6), i+P(1), P(7), 160, 120, 200);
        FillRect(buf, i, P(24), i+P(1), P(25), 160, 120, 200);
    }
    for (int i = P(6); i < P(26); i += P(3))
    {
        FillRect(buf, P(4), i, P(5), i+P(1), 160, 120, 200);
        FillRect(buf, P(26), i, P(27), i+P(1), 160, 120, 200);
    }
    FillRect(buf, P(2), P(4), P(6), P(8), 200, 160, 240);
    FillRect(buf, P(25), P(4), P(29), P(8), 200, 160, 240);
    FillRect(buf, P(2), P(23), P(6), P(27), 200, 160, 240);
    FillRect(buf, P(25), P(23), P(29), P(27), 200, 160, 240);
    DrawLine(buf, P(14), P(16), P(18), P(16), 200, 160, 240, P(1));
    DrawLine(buf, P(16), P(14), P(16), P(18), 200, 160, 240, P(1));
}

inline void GenNavigateIcon(uint8_t* buf)
{
    memset(buf, 0, ICON_BYTES);
    FillCircle(buf, P(16), P(16), P(14), 60, 160, 230);
    FillCircle(buf, P(16), P(16), P(10), 20, 22, 28);
    DrawLine(buf, P(16), P(2), P(16), P(12), 100, 220, 255, P(2));
    DrawLine(buf, P(16), P(20), P(16), P(30), 100, 220, 255, P(2));
    DrawLine(buf, P(2), P(16), P(12), P(16), 100, 220, 255, P(2));
    DrawLine(buf, P(20), P(16), P(30), P(16), 100, 220, 255, P(2));
    FillCircle(buf, P(16), P(16), P(4), 255, 120, 60);
}

#undef P
#undef S

enum IconID
{
    ICON_LAUNCH = 0,
    ICON_CAPTURE,
    ICON_STOP,
    ICON_GLOBE,
    ICON_TERRAIN,
    ICON_SPLINE,
    ICON_IMPORT,
    ICON_EXPORT,
    ICON_PIN,
    ICON_REGION,
    ICON_NAVIGATE,
    ICON_COUNT
};

typedef void (*IconGenFunc)(uint8_t*);

inline IconGenFunc GetIconGenerator(IconID id)
{
    static IconGenFunc funcs[ICON_COUNT] = {
        GenLaunchIcon,
        GenCaptureIcon,
        GenStopIcon,
        GenGlobeIcon,
        GenTerrainIcon,
        GenSplineIcon,
        GenImportIcon,
        GenExportIcon,
        GenPinIcon,
        GenRegionIcon,
        GenNavigateIcon
    };
    return funcs[id];
}

} // namespace icons
