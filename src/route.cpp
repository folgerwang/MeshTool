#include "route.h"

#include <windows.h>
#include <winhttp.h>
#include <cmath>
#include <cstdlib>
#include <cstring>
#include <GeographicLib/Geodesic.hpp>

#pragma comment(lib, "winhttp.lib")

namespace {

const char* kUserAgent = "MeshTool/1.0 route capture (https://github.com/fengquanw/MeshTool)";

std::wstring Widen(const std::string& s)
{
    if (s.empty()) return std::wstring();
    int n = MultiByteToWideChar(CP_UTF8, 0, s.c_str(), (int)s.size(), nullptr, 0);
    std::wstring w(n, 0);
    MultiByteToWideChar(CP_UTF8, 0, s.c_str(), (int)s.size(), &w[0], n);
    return w;
}

std::string UrlEncode(const std::string& s)
{
    static const char* hex = "0123456789ABCDEF";
    std::string out;
    for (unsigned char c : s)
    {
        if (isalnum(c) || c == '-' || c == '_' || c == '.' || c == '~')
            out += (char)c;
        else if (c == ' ')
            out += '+';
        else
        {
            out += '%';
            out += hex[c >> 4];
            out += hex[c & 15];
        }
    }
    return out;
}

// Value of the first "key":"..." after `from` (JSON string, \" and \\ unescaped).
bool JsonString(const std::string& json, const char* key, size_t from, std::string& out)
{
    std::string pattern = std::string("\"") + key + "\"";
    size_t k = json.find(pattern, from);
    while (k != std::string::npos)
    {
        size_t p = k + pattern.size();
        while (p < json.size() && isspace((unsigned char)json[p])) p++;
        if (p < json.size() && json[p] == ':')
        {
            p++;
            while (p < json.size() && isspace((unsigned char)json[p])) p++;
            if (p < json.size() && json[p] == '"')
            {
                out.clear();
                for (p++; p < json.size() && json[p] != '"'; p++)
                {
                    if (json[p] == '\\' && p + 1 < json.size()) { p++; if (json[p] == 'u') { p += 4; out += '?'; continue; } }
                    out += json[p];
                }
                return true;
            }
            return false;
        }
        k = json.find(pattern, k + 1);
    }
    return false;
}

bool JsonNumber(const std::string& json, const char* key, size_t from, double& out)
{
    std::string pattern = std::string("\"") + key + "\":";
    size_t k = json.find(pattern, from);
    if (k == std::string::npos) return false;
    out = strtod(json.c_str() + k + pattern.size(), nullptr);
    return true;
}

} // namespace

std::string HttpGet(const std::string& url, std::string* error)
{
    auto fail = [&](const char* what) {
        char buf[160];
        snprintf(buf, sizeof(buf), "%s (error %lu)", what, GetLastError());
        if (error) *error = buf;
        return std::string();
    };
    std::wstring wurl = Widen(url);
    URL_COMPONENTS parts{};
    parts.dwStructSize = sizeof(parts);
    wchar_t host[256] = {}, path[2048] = {};
    parts.lpszHostName = host;      parts.dwHostNameLength = 256;
    parts.lpszUrlPath = path;       parts.dwUrlPathLength = 2048;
    if (!WinHttpCrackUrl(wurl.c_str(), (DWORD)wurl.size(), 0, &parts))
        return fail("bad URL");

    HINTERNET session = WinHttpOpen(Widen(kUserAgent).c_str(), WINHTTP_ACCESS_TYPE_AUTOMATIC_PROXY,
                                    WINHTTP_NO_PROXY_NAME, WINHTTP_NO_PROXY_BYPASS, 0);
    if (!session) return fail("WinHttpOpen failed");
    WinHttpSetTimeouts(session, 10000, 10000, 20000, 20000);
    std::string body;
    HINTERNET connect = WinHttpConnect(session, host, parts.nPort, 0);
    HINTERNET request = connect ? WinHttpOpenRequest(connect, L"GET", path, nullptr, WINHTTP_NO_REFERER,
                                                     WINHTTP_DEFAULT_ACCEPT_TYPES,
                                                     parts.nScheme == INTERNET_SCHEME_HTTPS ? WINHTTP_FLAG_SECURE : 0)
                                : nullptr;
    bool ok = request &&
              WinHttpSendRequest(request, L"Accept: application/json\r\n", (DWORD)-1, WINHTTP_NO_REQUEST_DATA, 0, 0, 0) &&
              WinHttpReceiveResponse(request, nullptr);
    DWORD status = 0, statusSize = sizeof(status);
    if (ok)
        WinHttpQueryHeaders(request, WINHTTP_QUERY_STATUS_CODE | WINHTTP_QUERY_FLAG_NUMBER,
                            WINHTTP_HEADER_NAME_BY_INDEX, &status, &statusSize, WINHTTP_NO_HEADER_INDEX);
    if (ok)
    {
        DWORD avail = 0;
        while (WinHttpQueryDataAvailable(request, &avail) && avail)
        {
            std::string chunk(avail, 0);
            DWORD got = 0;
            if (!WinHttpReadData(request, &chunk[0], avail, &got)) break;
            body.append(chunk, 0, got);
        }
    }
    const char* what = !connect ? "could not connect" : !request ? "could not open the request"
                       : !ok    ? "request failed" : nullptr;
    if (request) WinHttpCloseHandle(request);
    if (connect) WinHttpCloseHandle(connect);
    WinHttpCloseHandle(session);
    if (what) return fail(what);
    if (status != 200)
    {
        if (error) *error = "HTTP " + std::to_string(status) + " from " + std::string(url, 0, url.find('/', 8));
        return std::string();
    }
    return body;
}

bool ParseLatLon(const std::string& text, double& lat, double& lon)
{
    char* end = nullptr;
    const char* s = text.c_str();
    lat = strtod(s, &end);
    if (end == s) return false;
    while (*end == ' ' || *end == ',' || *end == ';') end++;
    const char* s2 = end;
    lon = strtod(s2, &end);
    if (end == s2) return false;
    while (*end == ' ') end++;
    return *end == 0 && fabs(lat) <= 90.0 && fabs(lon) <= 180.0;
}

bool Geocode(const std::string& query, double& lon, double& lat, std::string& name, std::string& error)
{
    if (ParseLatLon(query, lat, lon))
    {
        char buf[64];
        snprintf(buf, sizeof(buf), "%.6f, %.6f", lat, lon);
        name = buf;
        return true;
    }
    std::string body = HttpGet("https://nominatim.openstreetmap.org/search?format=json&limit=1&q=" + UrlEncode(query), &error);
    if (body.empty())
    {
        if (error.empty()) error = "empty answer from the geocoder";
        return false;
    }
    std::string latText, lonText;
    if (!JsonString(body, "lat", 0, latText) || !JsonString(body, "lon", 0, lonText))
    {
        error = "no match for \"" + query + "\"";
        return false;
    }
    lat = strtod(latText.c_str(), nullptr);
    lon = strtod(lonText.c_str(), nullptr);
    if (!JsonString(body, "display_name", 0, name)) name = query;
    return true;
}

bool RouteBetween(double lon0, double lat0, double lon1, double lat1,
                  std::vector<std::pair<double, double>>& polyline, double& lengthM, std::string& error)
{
    char url[320];
    snprintf(url, sizeof(url),
             "https://router.project-osrm.org/route/v1/driving/%.6f,%.6f;%.6f,%.6f?overview=full&geometries=geojson",
             lon0, lat0, lon1, lat1);
    std::string body = HttpGet(url, &error);
    if (body.empty())
    {
        if (error.empty()) error = "empty answer from the router";
        return false;
    }
    std::string code;
    if (JsonString(body, "code", 0, code) && code != "Ok")
    {
        std::string message;
        JsonString(body, "message", 0, message);
        error = "router: " + code + (message.empty() ? "" : " - " + message);
        return false;
    }
    size_t coords = body.find("\"coordinates\":");
    if (coords == std::string::npos || !JsonNumber(body, "distance", 0, lengthM))
    {
        error = "router answer without a route";
        return false;
    }
    polyline.clear();
    const char* p = body.c_str() + coords + 14;
    while (*p && *p != ']' && *p != '[') p++;
    // [[lon,lat],[lon,lat],...]
    if (*p == '[') p++;
    while (*p)
    {
        while (*p && *p != '[' && *p != ']') p++;
        if (*p != '[') break;
        char* end = nullptr;
        double lon = strtod(p + 1, &end);
        if (end == p + 1) break;
        while (*end == ',' || *end == ' ') end++;
        const char* s = end;
        double lat = strtod(s, &end);
        if (end == s) break;
        polyline.emplace_back(lon, lat);
        while (*end && *end != ']') end++;
        p = *end ? end + 1 : end;
    }
    if (polyline.size() < 2)
    {
        error = "route has no geometry";
        return false;
    }
    return true;
}

std::vector<RouteWaypoint> SampleRoute(const std::vector<std::pair<double, double>>& polyline,
                                       double spacingM, bool lookBack)
{
    std::vector<RouteWaypoint> out;
    if (polyline.size() < 2) return out;
    const GeographicLib::Geodesic& geod = GeographicLib::Geodesic::WGS84();
    spacingM = (std::max)(10.0, spacingM);
    double travelled = 0.0, nextAt = 0.0;
    for (size_t i = 0; i + 1 < polyline.size(); i++)
    {
        double lon0 = polyline[i].first, lat0 = polyline[i].second;
        double lon1 = polyline[i + 1].first, lat1 = polyline[i + 1].second;
        double s12 = 0, azi1 = 0, azi2 = 0;
        geod.Inverse(lat0, lon0, lat1, lon1, s12, azi1, azi2);
        if (s12 <= 0.0) continue;
        // Waypoints that fall inside this segment.
        while (nextAt <= travelled + s12)
        {
            double along = nextAt - travelled;
            RouteWaypoint w;
            double azi = 0;
            geod.Direct(lat0, lon0, azi1, along, w.lat, w.lon, azi);
            w.heading = fmod(azi + 360.0, 360.0);
            w.distance = nextAt;
            out.push_back(w);
            nextAt += spacingM;
        }
        travelled += s12;
    }
    // The destination, unless the last waypoint already sits on it.
    if (out.empty() || travelled - out.back().distance > spacingM * 0.4)
    {
        RouteWaypoint w;
        w.lon = polyline.back().first;
        w.lat = polyline.back().second;
        w.heading = out.empty() ? 0.0 : out.back().heading;
        w.distance = travelled;
        out.push_back(w);
    }
    if (lookBack)
    {
        std::vector<RouteWaypoint> both;
        for (const RouteWaypoint& w : out)
        {
            both.push_back(w);
            RouteWaypoint back = w;
            back.heading = fmod(w.heading + 180.0, 360.0);
            both.push_back(back);
        }
        out.swap(both);
    }
    return out;
}

RoutePlan PlanRoute(const std::string& from, const std::string& to, double spacingM, bool lookBack)
{
    RoutePlan plan;
    if (!Geocode(from, plan.fromLon, plan.fromLat, plan.fromName, plan.error))
    {
        plan.error = "Start: " + plan.error;
        return plan;
    }
    if (!Geocode(to, plan.toLon, plan.toLat, plan.toName, plan.error))
    {
        plan.error = "Destination: " + plan.error;
        return plan;
    }
    if (!RouteBetween(plan.fromLon, plan.fromLat, plan.toLon, plan.toLat, plan.polyline, plan.lengthM, plan.error))
        return plan;
    plan.waypoints = SampleRoute(plan.polyline, spacingM, lookBack);
    plan.ok = !plan.waypoints.empty();
    if (!plan.ok) plan.error = "the route produced no waypoints";
    return plan;
}
