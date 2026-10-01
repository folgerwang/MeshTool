#ifndef NOMINMAX
#define NOMINMAX
#endif
#include "httplib.h"   // before windows.h (winsock2)
#include "qwen_client.h"

#include <chrono>
#include <cstring>
#include "stb_image_write.h"

namespace
{
    std::string Base64(const std::vector<uint8_t>& data)
    {
        static const char kTable[] = "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/";
        std::string out;
        out.reserve((data.size() + 2) / 3 * 4);
        size_t i = 0;
        for (; i + 2 < data.size(); i += 3)
        {
            uint32_t v = (uint32_t(data[i]) << 16) | (uint32_t(data[i + 1]) << 8) | data[i + 2];
            out += kTable[(v >> 18) & 63]; out += kTable[(v >> 12) & 63];
            out += kTable[(v >> 6) & 63];  out += kTable[v & 63];
        }
        if (i < data.size())
        {
            uint32_t v = uint32_t(data[i]) << 16;
            if (i + 1 < data.size()) v |= uint32_t(data[i + 1]) << 8;
            out += kTable[(v >> 18) & 63]; out += kTable[(v >> 12) & 63];
            out += (i + 1 < data.size()) ? kTable[(v >> 6) & 63] : '=';
            out += '=';
        }
        return out;
    }

    std::string JsonEscape(const std::string& s)
    {
        std::string out;
        out.reserve(s.size() + 16);
        for (unsigned char c : s)
        {
            switch (c)
            {
            case '"':  out += "\\\""; break;
            case '\\': out += "\\\\"; break;
            case '\n': out += "\\n"; break;
            case '\r': out += "\\r"; break;
            case '\t': out += "\\t"; break;
            default:
                if (c < 0x20) { char buf[8]; snprintf(buf, sizeof(buf), "\\u%04x", c); out += buf; }
                else out += char(c);
            }
        }
        return out;
    }

    // Reads the JSON string starting at s[pos] == '"'; returns false if malformed.
    bool ParseJsonString(const std::string& s, size_t pos, std::string& out)
    {
        if (pos >= s.size() || s[pos] != '"') return false;
        out.clear();
        for (size_t i = pos + 1; i < s.size(); i++)
        {
            char c = s[i];
            if (c == '"') return true;
            if (c != '\\') { out += c; continue; }
            if (++i >= s.size()) return false;
            switch (s[i])
            {
            case 'n': out += '\n'; break;
            case 'r': out += '\r'; break;
            case 't': out += '\t'; break;
            case 'b': out += '\b'; break;
            case 'f': out += '\f'; break;
            case 'u':
            {
                if (i + 4 >= s.size()) return false;
                unsigned code = unsigned(strtoul(s.substr(i + 1, 4).c_str(), nullptr, 16));
                i += 4;
                // UTF-8 encode (surrogate pairs are not needed for our answers).
                if (code < 0x80) out += char(code);
                else if (code < 0x800) { out += char(0xC0 | (code >> 6)); out += char(0x80 | (code & 63)); }
                else { out += char(0xE0 | (code >> 12)); out += char(0x80 | ((code >> 6) & 63)); out += char(0x80 | (code & 63)); }
                break;
            }
            default: out += s[i]; break;   // \" \\ \/
            }
        }
        return false;
    }

    // Value of the first "key": "<string>" at or after `from`.
    bool FindJsonString(const std::string& json, const char* key, size_t from, std::string& out)
    {
        std::string pattern = std::string("\"") + key + "\"";
        size_t k = json.find(pattern, from);
        while (k != std::string::npos)
        {
            size_t p = k + pattern.size();
            while (p < json.size() && (json[p] == ' ' || json[p] == '\t' || json[p] == '\n' || json[p] == '\r')) p++;
            if (p < json.size() && json[p] == ':')
            {
                p++;
                while (p < json.size() && (json[p] == ' ' || json[p] == '\t' || json[p] == '\n' || json[p] == '\r')) p++;
                return ParseJsonString(json, p, out);
            }
            k = json.find(pattern, k + 1);
        }
        return false;
    }

    void AppendBytes(void* context, void* data, int size)
    {
        auto* out = static_cast<std::vector<uint8_t>*>(context);
        const uint8_t* p = static_cast<const uint8_t*>(data);
        out->insert(out->end(), p, p + size);
    }
}

bool EncodePngRgb(int width, int height, const uint8_t* rgb, std::vector<uint8_t>& out)
{
    out.clear();
    return stbi_write_png_to_func(AppendBytes, &out, width, height, 3, rgb, width * 3) != 0;
}

bool WritePngRgb(const std::string& path, int width, int height, const uint8_t* rgb)
{
    return stbi_write_png(path.c_str(), width, height, 3, rgb, width * 3) != 0;
}

QwenClient::QwenClient(const std::string& serverUrl, const std::string& model)
    : m_model(model)
{
    // "http://host:port[/...]"
    std::string s = serverUrl;
    size_t scheme = s.find("://");
    if (scheme != std::string::npos) s = s.substr(scheme + 3);
    size_t slash = s.find('/');
    if (slash != std::string::npos) s = s.substr(0, slash);
    size_t colon = s.rfind(':');
    if (colon != std::string::npos)
    {
        m_port = atoi(s.c_str() + colon + 1);
        s = s.substr(0, colon);
    }
    m_host = s.empty() ? "127.0.0.1" : s;
}

bool QwenClient::CheckModel(std::string& error)
{
    httplib::Client cli(m_host, m_port);
    cli.set_connection_timeout(3);
    cli.set_read_timeout(30);
    std::string body = "{\"model\":\"" + JsonEscape(m_model) + "\"}";
    auto res = cli.Post("/api/show", body, "application/json");
    if (!res)
    {
        error = "Cannot reach the model server at " + m_host + ":" + std::to_string(m_port) + " (is Ollama running?)";
        return false;
    }
    if (res->status != 200)
    {
        error = "Model '" + m_model + "' not found on the server (HTTP " + std::to_string(res->status) + ")";
        return false;
    }
    if (res->body.find("\"vision\"") == std::string::npos)
    {
        error = "Model '" + m_model + "' has no vision capability";
        return false;
    }
    return true;
}

bool QwenClient::Ask(const std::string& prompt, const std::vector<Image>& images,
                     std::string& answer, std::string& error, double* seconds,
                     const std::string& jsonSchema)
{
    auto t0 = std::chrono::steady_clock::now();

    std::string body;
    body.reserve(1 << 20);
    body += "{\"model\":\"" + JsonEscape(m_model) + "\",\"stream\":false,\"think\":false,\"keep_alive\":\"30m\",";
    if (!jsonSchema.empty())
        body += "\"format\":" + jsonSchema + ",";
    body += ""
            "\"options\":{\"temperature\":0,\"num_ctx\":8192},"
            "\"messages\":[{\"role\":\"user\",\"content\":\"" + JsonEscape(prompt) + "\",\"images\":[";
    std::vector<uint8_t> png;
    for (size_t i = 0; i < images.size(); i++)
    {
        if (!EncodePngRgb(images[i].width, images[i].height, images[i].rgb, png))
        {
            error = "PNG encoding failed";
            return false;
        }
        if (i) body += ',';
        body += '"';
        body += Base64(png);
        body += '"';
    }
    body += "]}]}";

    httplib::Client cli(m_host, m_port);
    cli.set_connection_timeout(5);
    cli.set_read_timeout(600);      // the first request loads the model
    cli.set_write_timeout(60);
    auto res = cli.Post("/api/chat", body, "application/json");
    if (seconds)
        *seconds = std::chrono::duration<double>(std::chrono::steady_clock::now() - t0).count();
    if (!res)
    {
        error = "Request to " + m_host + ":" + std::to_string(m_port) + " failed: " + httplib::to_string(res.error());
        return false;
    }
    std::string server_error;
    if (FindJsonString(res->body, "error", 0, server_error))
    {
        error = "Model server error: " + server_error;
        return false;
    }
    if (res->status != 200)
    {
        error = "Model server returned HTTP " + std::to_string(res->status);
        return false;
    }
    size_t msg = res->body.find("\"message\"");
    if (msg == std::string::npos || !FindJsonString(res->body, "content", msg, answer))
    {
        error = "Unexpected model server response";
        return false;
    }
    return true;
}
