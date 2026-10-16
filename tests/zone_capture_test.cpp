#include "route.h"
#include <GeographicLib/Geodesic.hpp>
#include <cmath>
#include <cstdio>
#include <limits>
#include <cstdlib>

static void Check(bool condition, const char* message)
{
    if (!condition) { fprintf(stderr, "%s\n", message); std::exit(1); }
}

int main()
{
    const double west = -122.421, east = -122.417, south = 37.773, north = 37.777;
    RoutePlan plan = PlanZone(west, south, east, north, 100);
    Check(plan.ok && plan.waypoints.size() > 4, "Zone must contain a grid");
    Check(plan.polyline.empty(), "Zone must not masquerade as a driving route");
    Check(plan.waypoints.front().lon == west && plan.waypoints.front().lat == north,
          "Grid starts at northwest corner");
    bool eastSeen = false, southSeen = false, reverseRow = false;
    for (size_t i = 0; i < plan.waypoints.size(); ++i)
    {
        const auto& p = plan.waypoints[i];
        Check(p.lon >= west - 1e-10 && p.lon <= east + 1e-10 &&
              p.lat >= south - 1e-10 && p.lat <= north + 1e-10, "Samples must stay inside bounds");
        eastSeen |= fabs(p.lon - east) < 1e-10;
        southSeen |= fabs(p.lat - south) < 1e-10;
        if (i)
        {
            const auto& prev = plan.waypoints[i - 1];
            double d;
            GeographicLib::Geodesic::WGS84().Inverse(prev.lat, prev.lon, p.lat, p.lon, d);
            Check(d <= 100.01, "Consecutive samples must respect requested spacing");
            Check(p.distance >= prev.distance, "Travel distance must increase");
            reverseRow |= p.lon < prev.lon;
        }
    }
    Check(eastSeen && southSeen && reverseRow, "Cover boundaries and alternate row direction");
    auto reversed = PlanZone(east, north, west, south, 100);
    Check(reversed.ok && reversed.waypoints.size() == plan.waypoints.size(), "Drag direction must not affect sampling");
    for (size_t i = 0; i < plan.waypoints.size(); ++i)
        Check(reversed.waypoints[i].lon == plan.waypoints[i].lon &&
              reversed.waypoints[i].lat == plan.waypoints[i].lat, "Reversed corners must produce the same path");
    Check(PlanZone(0, 0, 0.00001, 0.00001, 100).waypoints.size() == 4, "Tiny zones include four corners");
    Check(!PlanZone(0, 0, 0, 1, 100).ok, "Reject zero width");
    Check(!PlanZone(0, 0, 1, 0, 100).ok, "Reject zero height");
    Check(!PlanZone(0, 0, 10, 10, 10).ok, "Reject excessive sample counts before allocating");
    Check(!PlanZone(179, 0, -179, 1, 100).ok, "Reject date-line crossing");
    Check(!PlanZone(0, 85, 1, 86, 100).ok, "Reject unsupported polar region");
    Check(!PlanZone(0, 0, 1, 1, 0).ok, "Reject invalid spacing");
    Check(!PlanZone(0, 0, 1, 1, std::numeric_limits<double>::quiet_NaN()).ok, "Reject NaN");
    Check(PlanZone(10, 80, 10.001, 80.001, 100).ok, "Allow valid high-latitude zones");
    // Regression: 100 m requested at 150 m AGL, with GE's observed narrow
    // vertical FOV, must be densified to preserve overlap in both directions.
    const double effective = ZoneCaptureSpacing(100, 150, 0.6, 60, 43.4);
    Check(effective > 40 && effective < 50, "Camera footprint must limit the 100 m request");
    Check(ZoneCaptureSpacing(20, 150, 0.6, 60, 43.4) == 20, "Preserve a denser user request");
    Check(ZoneCaptureSpacing(100, 75, 0.6, 60, 43.4) < effective, "Lower cameras need denser samples");
    Check(fabs(ZoneCaptureSpacing(100, 150, 0.6, 43.4, 60) - effective) < 1e-9,
          "Portrait windows must use the narrower horizontal footprint");
    Check(ZoneCaptureSpacing(100, 150, 0.6) > 0 && ZoneCaptureSpacing(100, 150, 0.6) < effective,
          "Missing FOV must use a conservative fallback");
    Check(ZoneCaptureSpacing(100, -1, 0.6) == 0, "Reject invalid camera height");
    Check(ZoneCaptureSpacing(100, 150, 1) == 0, "Reject impossible overlap");
    auto zone = PlanZone(-122.4204, 37.7739, -122.4184, 37.7759, effective);
    Check(zone.ok && zone.waypoints.size() >= 30, "Regression zone needs more than the old 12 samples");
    const double footprint = effective / 0.4;
    // Across the regression rectangle, each point must be inside the central
    // 40% of at least one ground footprint (not just inside the full image).
    for (int y = 0; y <= 30; ++y)
        for (int x = 0; x <= 30; ++x)
        {
            double lat = 37.7739 + 0.002 * y / 30, lon = -122.4204 + 0.002 * x / 30;
            bool covered = false;
            for (const auto& camera : zone.waypoints)
            {
                double northDistance, eastDistance;
                GeographicLib::Geodesic::WGS84().Inverse(lat, lon, camera.lat, lon, northDistance);
                GeographicLib::Geodesic::WGS84().Inverse(lat, lon, lat, camera.lon, eastDistance);
                covered |= northDistance <= footprint * 0.2 + 0.01 && eastDistance <= footprint * 0.2 + 0.01;
            }
            Check(covered, "Regression zone must have continuous overlapping ground footprints");
        }
    puts("Zone capture planning and overlapping coverage checks passed.");
}
