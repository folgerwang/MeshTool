#pragma once

#include <string>
#include <mutex>
#include <thread>
#include <memory>

// Google Earth's current view, as GE reports it through a KML NetworkLink
// (viewRefreshMode onStop): every time the camera comes to rest GE requests
// http://127.0.0.1:<port>/view?<view parameters>. This is what ties captured
// eye-space geometry to real GPS coordinates.
struct GeoView
{
    bool   valid = false;
    double time = 0.0;          // glfwGetTime() when received

    // GE's LookAt: the camera sits `range` metres from (laLon, laLat) at
    // altitude 0 (sea level) - not from the terrain point, as captures show.
    bool   hasLookAt = false;
    double laLon = 0.0, laLat = 0.0;                        // degrees
    double range = 0.0;                                     // metres
    double heading = 0.0, tilt = 0.0;                       // degrees (tilt 0 = looking straight down)

    // The terrain at screen centre (cross-check only).
    double lookLon = 0.0, lookLat = 0.0, lookAlt = 0.0;   // degrees, metres above sea level

    // Camera position as GE reports it (used as a cross-check).
    double camLon = 0.0, camLat = 0.0, camAlt = 0.0;
    double hFov = 0.0, vFov = 0.0;
};

namespace httplib { class Server; }

class GeoViewServer
{
public:
    GeoViewServer();    // out of line: httplib::Server is incomplete here
    ~GeoViewServer();

    // Starts listening on 127.0.0.1 (first free port from 47321). Returns false
    // if no port could be bound.
    bool Start();
    void Stop();
    int  Port() const { return m_port; }

    GeoView Latest() const;

    // KML that makes GE report its view here. `extraKml` is inserted into the
    // Document (e.g. a fly-to LookAt). Returns the absolute file path, or "".
    std::string WriteKml(const std::string& fileName, const std::string& extraKml = "") const;

private:
    std::unique_ptr<httplib::Server> m_server;
    std::thread         m_thread;
    int                 m_port = 0;
    mutable std::mutex  m_mutex;
    GeoView             m_latest;
};
