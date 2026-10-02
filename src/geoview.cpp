#ifndef NOMINMAX
#define NOMINMAX
#endif
#include "httplib.h"   // before windows.h: it pulls in winsock2
#include "geoview.h"

#include <cstdio>
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
    // GE's view arrives on /view: when GE's camera comes to rest, and once a
    // second besides. A link refreshed by time instead of view gets zeros for
    // every view parameter, so a view at range 0 is no view.
    auto storeView = [this](const httplib::Request& req) {
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
        if (ok && v.range > 0.0)
        {
            v.valid = true;
            v.time = glfwGetTime();
            std::lock_guard<std::mutex> lock(m_mutex);
            m_latest = v;
        }
    };
    m_server->Get("/view", [storeView](const httplib::Request& req, httplib::Response& res) {
        storeView(req);
        // GE expects KML back; an empty document adds nothing to its view.
        res.set_content("<?xml version=\"1.0\" encoding=\"UTF-8\"?>"
                        "<kml xmlns=\"http://www.opengis.net/kml/2.2\"><Document/></kml>",
                        "application/vnd.google-earth.kml+xml");
    });

    // Viewport follow. The polled link must not fly (flyToView on a link
    // whose answer has no view sends GE to 0,0), so a new camera comes as a
    // one-off nested link that does fly: its document holds just that
    // camera. The next poll drops the nested link; GE stays where it is.
    m_server->Get("/follow", [this](const httplib::Request&, httplib::Response& res) {
        std::string body;
        {
            std::lock_guard<std::mutex> lock(m_mutex);
            m_followPollTime = glfwGetTime();
            if (m_followServed != m_followSeq)
            {
                m_followServed = m_followSeq;
                m_followDeliveredTime = m_followPollTime;
                body = "<NetworkLink><name>MeshTool camera</name><flyToView>1</flyToView><Link><href>"
                       "http://127.0.0.1:" + std::to_string(m_port) + "/fly?seq=" + std::to_string(m_followSeq) +
                       "</href></Link></NetworkLink>";
            }
        }
        res.set_header("Cache-Control", "no-cache");
        res.set_content("<?xml version=\"1.0\" encoding=\"UTF-8\"?>"
                        "<kml xmlns=\"http://www.opengis.net/kml/2.2\"><Document>" + body + "</Document></kml>",
                        "application/vnd.google-earth.kml+xml");
    });
    m_server->Get("/fly", [this](const httplib::Request&, httplib::Response& res) {
        std::string camera;
        {
            std::lock_guard<std::mutex> lock(m_mutex);
            camera = m_followKml;
        }
        res.set_header("Cache-Control", "no-cache");
        res.set_content("<?xml version=\"1.0\" encoding=\"UTF-8\"?>"
                        "<kml xmlns=\"http://www.opengis.net/kml/2.2\"><Document>" + camera + "</Document></kml>",
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

void GeoViewServer::SetFollowCamera(double lon, double lat, double alt, double heading, double tilt)
{
    char kml[512];
    snprintf(kml, sizeof(kml),
             "<Camera><longitude>%.9f</longitude><latitude>%.9f</latitude><altitude>%.2f</altitude>"
             "<heading>%.3f</heading><tilt>%.3f</tilt><roll>0</roll>"
             "<altitudeMode>absolute</altitudeMode></Camera>",
             lon, lat, alt, heading, tilt);
    std::lock_guard<std::mutex> lock(m_mutex);
    m_followKml = kml;
    m_followSeq++;
}

bool GeoViewServer::FollowPending(double* deliveredTime) const
{
    std::lock_guard<std::mutex> lock(m_mutex);
    if (deliveredTime)
        *deliveredTime = m_followDeliveredTime;
    return m_followServed != m_followSeq;
}

double GeoViewServer::LastFollowPoll() const
{
    std::lock_guard<std::mutex> lock(m_mutex);
    return m_followPollTime;
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
      << "      <refreshMode>onInterval</refreshMode>\n"
      << "      <refreshInterval>1</refreshInterval>\n"
      << "      <viewRefreshMode>onStop</viewRefreshMode>\n"
      << "      <viewRefreshTime>0.2</viewRefreshTime>\n"
      << "      <viewFormat>lon=[lookatLon]&amp;lat=[lookatLat]"
         "&amp;llon=[lookatTerrainLon]&amp;llat=[lookatTerrainLat]&amp;lalt=[lookatTerrainAlt]"
         "&amp;range=[lookatRange]&amp;head=[lookatHeading]&amp;tilt=[lookatTilt]"
         "&amp;clon=[cameraLon]&amp;clat=[cameraLat]&amp;calt=[cameraAlt]"
         "&amp;hfov=[horizFov]&amp;vfov=[vertFov]</viewFormat>\n"
      << "    </Link>\n"
      << "  </NetworkLink>\n"
      << "  <NetworkLink>\n"
      << "    <name>MeshTool viewport follow</name>\n"
      << "    <visibility>1</visibility>\n"
      << "    <flyToView>0</flyToView>\n"
      << "    <Link>\n"
      << "      <href>http://127.0.0.1:" << m_port << "/follow</href>\n"
      << "      <refreshMode>onInterval</refreshMode>\n"
      << "      <refreshInterval>0.5</refreshInterval>\n"
      << "    </Link>\n"
      << "  </NetworkLink>\n"
      << "</Document>\n"
      << "</kml>\n";
    f.close();

    char full[MAX_PATH];
    GetFullPathNameA(fileName.c_str(), MAX_PATH, full, nullptr);
    return std::string(full);
}
