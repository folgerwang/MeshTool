#pragma once

#include <cstdint>
#include <string>
#include <vector>

// Minimal client for a vision model served by Ollama (/api/chat), used to
// label numbered image regions. Blocking; call from a worker thread.
class QwenClient
{
public:
    // serverUrl like "http://127.0.0.1:11434"; model like "qwen3.8:latest".
    QwenClient(const std::string& serverUrl, const std::string& model);

    // Sends `prompt` with RGB images (tightly packed, width*height*3 bytes)
    // and returns the model's text answer. On failure returns false and sets
    // `error`. `seconds` receives the request's wall time.
    // `jsonSchema` (optional) constrains the answer to that JSON schema
    // (Ollama structured outputs): no prose, only allowed values.
    struct Image { int width = 0, height = 0; const uint8_t* rgb = nullptr; };
    bool Ask(const std::string& prompt, const std::vector<Image>& images,
             std::string& answer, std::string& error, double* seconds = nullptr,
             const std::string& jsonSchema = "");

    // Checks the server is up and the model exists (and can see images).
    bool CheckModel(std::string& error);

private:
    std::string m_host;
    int         m_port = 11434;
    std::string m_model;
};

// PNG-encodes tightly packed RGB pixels.
bool EncodePngRgb(int width, int height, const uint8_t* rgb, std::vector<uint8_t>& out);
// Writes tightly packed RGB pixels as a PNG file.
bool WritePngRgb(const std::string& path, int width, int height, const uint8_t* rgb);
