#ifndef NOMINMAX
#define NOMINMAX
#endif
#include "httplib.h"   // before windows.h: it pulls in winsock2
#include "geoview.h"

#include <cstdlib>
#include <fstream>
#include <windows.h>
#include <GLFW/glfw3.h>

GeoViewServer::GeoViewServer() = default;

GeoViewServer::~GeoViewServer()
{
    Stop();
}

bool GeoViewServer::Start()
{
    if (m_server)
        return true;

    m_server = std::make_unique<httplib::Server>();
    m_server->Get("/view", [this](const httplib::Request& req, httplib::Response& res) {
        auto num = [&req](const char* key, double& out) {
            if (!req.has_param(key)) return false;
            out = atof(req.get_param_value(key).c_str());
            return true;
        };
        GeoView v;
        bool ok = num("llon", v.lookLon) && num("llat", v.lookLat) && num("range", v.range) &&
                  num("head", v.heading) && num("tilt", v.tilt);
        num("lalt", v.lookAlt);
        v.hasLookAt = num("lon", v.laLon) && num("lat", v.laLat);
        num("clon", v.camLon);
        num("clat", v.camLat);
        num("calt", v.camAlt);
        num("hfov", v.hFov);
        num("vfov", v.vFov);
        if (ok)
        {
            v.valid = true;
            v.time = glfwGetTime();
            std::lock_guard<std::mutex> lock(m_mutex);
            m_latest = v;
        }
        // GE expects KML back; an empty document adds nothing to its view.
        res.set_content("<?xml version=\"1.0\" encoding=\"UTF-8\"?>"
                        "<kml xmlns=\"http://www.opengis.net/kml/2.2\"><Document/></kml>",
                        "application/vnd.google-earth.kml+xml");
    });

    for (int port = 47321; port < 47341; port++)
    {
        if (m_server->bind_to_port("127.0.0.1", port))
        {
            m_port = port;
            m_thread = std::thread([this] { m_server->listen_after_bind(); });
            return true;
        }
    }
    m_server.reset();
    return false;
}

void GeoViewServer::Stop()
{
    if (m_server)
    {
        m_server->stop();
        if (m_thread.joinable())
            m_thread.join();
        m_server.reset();
    }
    m_port = 0;
}

GeoView GeoViewServer::Latest() const
{
    std::lock_guard<std::mutex> lock(m_mutex);
    return m_latest;
}

std::string GeoViewServer::WriteKml(const std::string& fileName, const std::string& extraKml) const
{
    if (!m_port)
        return "";
    std::ofstream f(fileName);
    if (!f)
        return "";

    // viewFormat values are substituted by GE; '&' must be escaped in XML.
    f << "<?xml version=\"1.0\" encoding=\"UTF-8\"?>\n"
      << "<kml xmlns=\"http://www.opengis.net/kml/2.2\">\n"
      << "<Document>\n"
      << "  <name>MeshTool</name>\n"
      << extraKml
      << "  <NetworkLink>\n"
      << "    <name>MeshTool camera link</name>\n"
      << "    <visibility>1</visibility>\n"
      << "    <Link>\n"
      << "      <href>http://127.0.0.1:" << m_port << "/view</href>\n"
      << "      <viewRefreshMode>onStop</viewRefreshMode>\n"
      << "      <viewRefreshTime>0.2</viewRefreshTime>\n"
      << "      <viewFormat>lon=[lookatLon]&amp;lat=[lookatLat]"
         "&amp;llon=[lookatTerrainLon]&amp;llat=[lookatTerrainLat]&amp;lalt=[lookatTerrainAlt]"
         "&amp;range=[lookatRange]&amp;head=[lookatHeading]&amp;tilt=[lookatTilt]"
         "&amp;clon=[cameraLon]&amp;clat=[cameraLat]&amp;calt=[cameraAlt]"
         "&amp;hfov=[horizFov]&amp;vfov=[vertFov]</viewFormat>\n"
      << "    </Link>\n"
      << "  </NetworkLink>\n"
      << "</Document>\n"
      << "</kml>\n";
    f.close();

    char full[MAX_PATH];
    GetFullPathNameA(fileName.c_str(), MAX_PATH, full, nullptr);
    return std::string(full);
}
