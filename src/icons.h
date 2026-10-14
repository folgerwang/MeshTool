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

// Segmentation: a scene split into class-coloured parts (building, tree,
// water, road), with a sparkle for the AI labelling.
inline void GenSegmentIcon(uint8_t* buf)
{
    memset(buf, 0, ICON_BYTES);
    FillRect(buf, P(4),  P(6),  P(15), P(16), 230, 115, 64);    // building
    FillRect(buf, P(17), P(6),  P(25), P(16), 40, 150, 50);     // tree
    FillRect(buf, P(4),  P(18), P(12), P(27), 60, 120, 240);    // water
    FillRect(buf, P(14), P(18), P(25), P(27), 150, 150, 162);   // road
    // four-point sparkle, top right
    const int cx = P(26), cy = P(6);
    DrawTriangle(buf, cx - P(1), cy, cx, cy - P(5), cx + P(1), cy, 255, 255, 255);
    DrawTriangle(buf, cx - P(1), cy, cx, cy + P(5), cx + P(1), cy, 255, 255, 255);
    DrawTriangle(buf, cx, cy - P(1), cx - P(5), cy, cx, cy + P(1), 255, 255, 255);
    DrawTriangle(buf, cx, cy - P(1), cx + P(5), cy, cx, cy + P(1), 255, 255, 255);
}

// Route capture: a winding route from a start marker to a destination pin,
// with the camera waypoints the capture visits along it.
inline void GenRouteIcon(uint8_t* buf)
{
    memset(buf, 0, ICON_BYTES);
    // Road: a light grey polyline stamped from discs for an even, solid stroke
    // (DrawLine's thickness leaves diagonals thin). Markers go on top.
    const int route[][2] = { {6, 25}, {13, 22}, {12, 15}, {20, 12}, {26, 6} };
    for (int i = 0; i + 1 < 5; i++)
    {
        const int x0 = P(route[i][0]), y0 = P(route[i][1]), x1 = P(route[i+1][0]), y1 = P(route[i+1][1]);
        const int steps = abs(x1 - x0) > abs(y1 - y0) ? abs(x1 - x0) : abs(y1 - y0);
        for (int s = 0; s <= steps; s++)
            FillCircle(buf, x0 + (x1 - x0) * s / steps, y0 + (y1 - y0) * s / steps, P(2), 200, 205, 215);
    }
    // Camera waypoints: blue dots with a dark ring so they read on the road.
    const int stops[][2] = { {13, 22}, {12, 15}, {20, 12} };
    for (const int* w : stops)
    {
        FillCircle(buf, P(w[0]), P(w[1]), P(3), 20, 22, 28);
        FillCircle(buf, P(w[0]), P(w[1]), P(2), 100, 200, 255);
    }
    // Start: green marker.
    FillCircle(buf, P(6), P(25), P(5), 100, 230, 120);
    FillCircle(buf, P(6), P(25), P(2), 20, 22, 28);
    // Destination: red pin with a white centre.
    FillCircle(buf, P(26), P(6), P(5), 240, 80, 80);
    DrawTriangle(buf, P(22), P(8), P(26), P(14), P(30), P(8), 240, 80, 80);
    FillCircle(buf, P(26), P(6), P(2), 255, 240, 240);
}

// Drive simulation: a car on a road, seen from the side, heading right.
inline void GenSimIcon(uint8_t* buf)
{
    memset(buf, 0, ICON_BYTES);
    // Road with a dashed centre line.
    FillRect(buf, P(2), P(24), P(30), P(28), 95, 98, 108);
    for (int x = 4; x < 30; x += 6)
        FillRect(buf, P(x), P(26), P(x + 2), P(26), 230, 200, 90);
    // Body: lower box plus a cabin, in a warm orange that reads on the dark toolbar.
    FillRect(buf, P(5), P(16), P(27), P(22), 245, 150, 50);
    DrawTriangle(buf, P(9), P(16), P(12), P(10), P(13), P(16), 245, 150, 50);
    FillRect(buf, P(12), P(10), P(21), P(16), 245, 150, 50);
    DrawTriangle(buf, P(21), P(10), P(25), P(16), P(21), P(16), 245, 150, 50);
    // Windows.
    FillRect(buf, P(13), P(11), P(16), P(15), 40, 50, 70);
    FillRect(buf, P(18), P(11), P(21), P(15), 40, 50, 70);
    // Headlight.
    FillRect(buf, P(26), P(17), P(27), P(18), 255, 245, 200);
    // Wheels: dark tyres with grey hubs, overlapping the road.
    FillCircle(buf, P(10), P(22), P(3), 25, 27, 32);
    FillCircle(buf, P(22), P(22), P(3), 25, 27, 32);
    FillCircle(buf, P(10), P(22), P(1), 170, 175, 185);
    FillCircle(buf, P(22), P(22), P(1), 170, 175, 185);
    // Motion lines behind the car.
    FillRect(buf, P(1), P(17), P(3), P(17), 200, 205, 215);
    FillRect(buf, P(1), P(20), P(3), P(20), 200, 205, 215);
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
    ICON_SEGMENT,
    ICON_ROUTE,
    ICON_SIM,
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
        GenNavigateIcon,
        GenSegmentIcon,
        GenRouteIcon,
        GenSimIcon
    };
    return funcs[id];
}

} // namespace icons
