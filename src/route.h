#pragma once
// Route capture: two addresses -> a driving route -> camera waypoints along it.
//
// Geocoding uses OpenStreetMap Nominatim and routing the public OSRM demo
// server, both over HTTPS through WinHTTP (no key needed). Each waypoint is a
// camera above the route looking along it; the app flies Google Earth there
// through the follow link and captures once the view has settled.
#include <string>
#include <vector>

struct RouteWaypoint
{
    double lon = 0.0, lat = 0.0;   // degrees, on the route
    double heading = 0.0;          // degrees clockwise from north, along the route
    double distance = 0.0;         // metres from the start of the route
};

struct RoutePlan
{
    bool        ok = false;
    std::string error;
    std::string fromName, toName;                 // geocoder's names for the two ends
    double      fromLon = 0, fromLat = 0, toLon = 0, toLat = 0;
    std::vector<std::pair<double, double>> polyline;   // lon, lat
    double      lengthM = 0.0;
    std::vector<RouteWaypoint> waypoints;
};

// HTTPS GET. Returns the body (also for an HTTP error status, which goes to
// `status`); on a transport failure returns "" and sets `error`.
std::string HttpGet(const std::string& url, std::string* error, int* status = nullptr);

// "lat, lon" / "lat lon" typed directly, else an address for the geocoder.
bool ParseLatLon(const std::string& text, double& lat, double& lon);

// One address -> WGS84 and a display name.
bool Geocode(const std::string& query, double& lon, double& lat, std::string& name, std::string& error);

// Driving route between two points: polyline (lon, lat) and length in metres.
bool RouteBetween(double lon0, double lat0, double lon1, double lat1,
                  std::vector<std::pair<double, double>>& polyline, double& lengthM, std::string& error);

// Cameras every `spacingM` along the polyline (plus the end), heading along
// the route. With `lookBack` each position is visited twice, the second time
// turned around, so both street sides get a facing view.
std::vector<RouteWaypoint> SampleRoute(const std::vector<std::pair<double, double>>& polyline,
                                       double spacingM, bool lookBack);

// Whole pipeline, for a worker thread. Either end may be an address or "lat, lon".
RoutePlan PlanRoute(const std::string& from, const std::string& to, double spacingM, bool lookBack);

// North-up rectangle, sampled in alternating rows. Includes the boundary and
// limits the distance between samples to spacingM. No network lookup needed.
RoutePlan PlanZone(double lon0, double lat0, double lon1, double lat1, double spacingM);

// Bound the requested spacing by the smaller top-down camera footprint, with
// overlap on both axes. FOVs are degrees; missing values use conservative defaults.
double ZoneCaptureSpacing(double requestedM, double heightM, double overlap,
                          double hFov = 0.0, double vFov = 0.0);
