#pragma once
// H.264 MP4 writer on Windows Media Foundation (built into Windows; no
// external encoder needed). Frames are tightly packed RGB rows, top-down.
#include <cstdint>
#include <string>
#include <vector>

struct IMFSinkWriter;

class VideoWriter
{
public:
    VideoWriter() = default;
    ~VideoWriter() { Close(); }
    VideoWriter(const VideoWriter&) = delete;
    VideoWriter& operator=(const VideoWriter&) = delete;

    // Even width and height; fps frames per second.
    bool Open(const std::string& path, int width, int height, int fps, std::string& error);
    bool WriteFrame(const uint8_t* rgb, std::string& error);
    // Finalises the file (the MP4 index is written here: a file left open is unplayable).
    void Close();

    bool IsOpen() const { return m_writer != nullptr; }
    int  Width() const { return m_width; }
    int  Height() const { return m_height; }
    int  Fps() const { return m_fps; }
    int64_t Frames() const { return m_frames; }
    const std::string& Path() const { return m_path; }

private:
    IMFSinkWriter* m_writer = nullptr;
    unsigned long  m_stream = 0;
    int            m_width = 0, m_height = 0, m_fps = 30;
    int64_t        m_frames = 0;
    bool           m_mfStarted = false;
    std::string    m_path;
    std::vector<uint8_t> m_bgra;
};
