#include "file_downloader.h"

// Note: HTTPS requires OpenSSL. If not available, only HTTP works.
// #define CPPHTTPLIB_OPENSSL_SUPPORT
#include "httplib.h"

#include <fstream>
#include <thread>
#include <stdexcept>
#include <algorithm>

// ---------------------------------------------------------------------------
// Helper: parse a URL into scheme, host and path components.
// Supports http://host/path and https://host/path.
// ---------------------------------------------------------------------------
namespace {

struct UrlParts {
    bool        use_ssl = false;
    std::string host;
    int         port    = -1;   // -1 means default (80 / 443)
    std::string path    = "/";
};

UrlParts parseUrl(const std::string& url)
{
    UrlParts parts;

    std::string remainder = url;

    // Scheme
    if (remainder.rfind("https://", 0) == 0) {
        parts.use_ssl = true;
        remainder = remainder.substr(8);
    } else if (remainder.rfind("http://", 0) == 0) {
        parts.use_ssl = false;
        remainder = remainder.substr(7);
    } else {
        // Default to http
        parts.use_ssl = false;
    }

    // Split host and path
    auto slash_pos = remainder.find('/');
    std::string host_port;
    if (slash_pos != std::string::npos) {
        host_port  = remainder.substr(0, slash_pos);
        parts.path = remainder.substr(slash_pos);
    } else {
        host_port  = remainder;
        parts.path = "/";
    }

    // Check for port
    auto colon_pos = host_port.find(':');
    if (colon_pos != std::string::npos) {
        parts.host = host_port.substr(0, colon_pos);
        parts.port = std::stoi(host_port.substr(colon_pos + 1));
    } else {
        parts.host = host_port;
        parts.port = parts.use_ssl ? 443 : 80;
    }

    return parts;
}

} // anonymous namespace

// ---------------------------------------------------------------------------
// FileDownloader implementation
// ---------------------------------------------------------------------------

FileDownloader::FileDownloader()  = default;
FileDownloader::~FileDownloader() = default;

bool FileDownloader::downloadFile(const std::string& url,
                                  const std::string& file_id,
                                  const std::string& dir_path)
{
    UrlParts parts = parseUrl(url);

    // Build output path
    std::string out_path = dir_path;
    if (!out_path.empty() && out_path.back() != '/' && out_path.back() != '\\') {
        out_path += '/';
    }
    out_path += file_id;

    // Perform GET request
    httplib::Result res;

#ifdef CPPHTTPLIB_OPENSSL_SUPPORT
    if (parts.use_ssl) {
        httplib::SSLClient cli(parts.host, parts.port);
        cli.enable_server_certificate_verification(false);
        cli.set_follow_location(true);
        res = cli.Get(parts.path.c_str());
    } else
#endif
    {
        httplib::Client cli(parts.host, parts.port);
        cli.set_follow_location(true);
        res = cli.Get(parts.path.c_str());
    }

    if (!res || res->status != 200) {
        return false;
    }

    // Write body to file
    std::ofstream ofs(out_path, std::ios::binary);
    if (!ofs.is_open()) {
        return false;
    }
    ofs.write(res->body.data(), static_cast<std::streamsize>(res->body.size()));
    ofs.close();

    return true;
}

void FileDownloader::downloadFileAsync(
        const std::string& url,
        const std::string& file_id,
        const std::string& dir_path,
        std::function<void(const std::string& path, const std::string& id)> on_complete,
        std::function<void(const std::string& error)> on_error)
{
    // Capture copies for the detached thread
    std::thread([=]() {
        try {
            bool ok = const_cast<FileDownloader*>(this)->downloadFile(url, file_id, dir_path);
            if (ok) {
                std::string out_path = dir_path;
                if (!out_path.empty() && out_path.back() != '/' && out_path.back() != '\\') {
                    out_path += '/';
                }
                out_path += file_id;
                if (on_complete) {
                    on_complete(out_path, file_id);
                }
            } else {
                if (on_error) {
                    on_error("HTTP request failed for: " + url);
                }
            }
        } catch (const std::exception& ex) {
            if (on_error) {
                on_error(std::string("Exception: ") + ex.what());
            }
        }
    }).detach();
}
