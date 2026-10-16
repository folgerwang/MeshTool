#ifndef NOMINMAX
#define NOMINMAX
#endif
#include "videowriter.h"

#include <windows.h>
#include <mfapi.h>
#include <mfidl.h>
#include <mfreadwrite.h>
#include <mferror.h>
#include <algorithm>
#include <cstdio>

namespace
{
    template<class T> void Release(T*& p)
    {
        if (p) { p->Release(); p = nullptr; }
    }

    std::string HrText(const char* what, HRESULT hr)
    {
        char buf[160];
        snprintf(buf, sizeof(buf), "%s failed (0x%08lx)", what, (unsigned long)hr);
        return buf;
    }

    std::wstring Widen(const std::string& utf8)
    {
        if (utf8.empty()) return std::wstring();
        const int n = MultiByteToWideChar(CP_UTF8, 0, utf8.c_str(), int(utf8.size()), nullptr, 0);
        std::wstring w(size_t(n), L'\0');
        MultiByteToWideChar(CP_UTF8, 0, utf8.c_str(), int(utf8.size()), w.data(), n);
        return w;
    }
}

bool VideoWriter::Open(const std::string& path, int width, int height, int fps, std::string& error)
{
    Close();
    if (width < 16 || height < 16 || (width & 1) || (height & 1) || fps < 1)
    {
        error = "the video needs an even width and height of at least 16 pixels";
        return false;
    }
    // COM may already be up in another mode (the file dialogs): that is fine.
    const HRESULT co = CoInitializeEx(nullptr, COINIT_MULTITHREADED);
    if (FAILED(co) && co != RPC_E_CHANGED_MODE)
    {
        error = HrText("CoInitializeEx", co);
        return false;
    }
    HRESULT hr = MFStartup(MF_VERSION);
    if (FAILED(hr))
    {
        error = HrText("MFStartup", hr);
        return false;
    }
    m_mfStarted = true;

    IMFAttributes* attrs = nullptr;
    IMFMediaType* out = nullptr;
    IMFMediaType* in = nullptr;
    auto fail = [&](const char* what) {
        error = HrText(what, hr);
        Release(attrs); Release(out); Release(in);
        Close();
        return false;
    };
    hr = MFCreateAttributes(&attrs, 1);
    if (FAILED(hr)) return fail("MFCreateAttributes");
    attrs->SetUINT32(MF_READWRITE_ENABLE_HARDWARE_TRANSFORMS, TRUE);
    hr = MFCreateSinkWriterFromURL(Widen(path).c_str(), nullptr, attrs, &m_writer);
    if (FAILED(hr)) return fail("creating the MP4 file");

    // Output: H.264 at a bit rate that keeps 1080p30 around 7 Mbit/s.
    const uint32_t bitrate = uint32_t(std::clamp(double(width) * height * fps * 0.12, 2.0e6, 40.0e6));
    hr = MFCreateMediaType(&out);
    if (FAILED(hr)) return fail("MFCreateMediaType");
    out->SetGUID(MF_MT_MAJOR_TYPE, MFMediaType_Video);
    out->SetGUID(MF_MT_SUBTYPE, MFVideoFormat_H264);
    out->SetUINT32(MF_MT_AVG_BITRATE, bitrate);
    out->SetUINT32(MF_MT_INTERLACE_MODE, MFVideoInterlace_Progressive);
    MFSetAttributeSize(out, MF_MT_FRAME_SIZE, UINT32(width), UINT32(height));
    MFSetAttributeRatio(out, MF_MT_FRAME_RATE, UINT32(fps), 1);
    MFSetAttributeRatio(out, MF_MT_PIXEL_ASPECT_RATIO, 1, 1);
    DWORD stream = 0;
    hr = m_writer->AddStream(out, &stream);
    if (FAILED(hr)) return fail("adding the H.264 stream (no H.264 encoder?)");
    m_stream = stream;

    // Input: RGB32 (BGRA in memory), rows top-down (positive default stride).
    hr = MFCreateMediaType(&in);
    if (FAILED(hr)) return fail("MFCreateMediaType");
    in->SetGUID(MF_MT_MAJOR_TYPE, MFMediaType_Video);
    in->SetGUID(MF_MT_SUBTYPE, MFVideoFormat_RGB32);
    in->SetUINT32(MF_MT_INTERLACE_MODE, MFVideoInterlace_Progressive);
    in->SetUINT32(MF_MT_DEFAULT_STRIDE, UINT32(width * 4));
    MFSetAttributeSize(in, MF_MT_FRAME_SIZE, UINT32(width), UINT32(height));
    MFSetAttributeRatio(in, MF_MT_FRAME_RATE, UINT32(fps), 1);
    MFSetAttributeRatio(in, MF_MT_PIXEL_ASPECT_RATIO, 1, 1);
    hr = m_writer->SetInputMediaType(m_stream, in, nullptr);
    if (FAILED(hr)) return fail("setting the RGB input type");
    hr = m_writer->BeginWriting();
    if (FAILED(hr)) return fail("BeginWriting");

    Release(attrs); Release(out); Release(in);
    m_width = width;
    m_height = height;
    m_fps = fps;
    m_frames = 0;
    m_path = path;
    m_bgra.resize(size_t(width) * height * 4);
    return true;
}

bool VideoWriter::WriteFrame(const uint8_t* rgb, std::string& error)
{
    if (!m_writer)
    {
        error = "no video is open";
        return false;
    }
    // RGB -> BGRA, rows top-down (a positive default stride means top-down in Media Foundation).
    for (int y = 0; y < m_height; y++)
    {
        const uint8_t* src = rgb + size_t(y) * m_width * 3;
        uint8_t* dst = m_bgra.data() + size_t(y) * m_width * 4;
        for (int x = 0; x < m_width; x++)
        {
            dst[0] = src[2]; dst[1] = src[1]; dst[2] = src[0]; dst[3] = 255;
            src += 3; dst += 4;
        }
    }
    const DWORD bytes = DWORD(m_bgra.size());
    IMFMediaBuffer* buffer = nullptr;
    IMFSample* sample = nullptr;
    HRESULT hr = MFCreateMemoryBuffer(bytes, &buffer);
    if (SUCCEEDED(hr))
    {
        BYTE* data = nullptr;
        hr = buffer->Lock(&data, nullptr, nullptr);
        if (SUCCEEDED(hr))
        {
            memcpy(data, m_bgra.data(), bytes);
            buffer->Unlock();
            buffer->SetCurrentLength(bytes);
        }
    }
    if (SUCCEEDED(hr)) hr = MFCreateSample(&sample);
    if (SUCCEEDED(hr)) hr = sample->AddBuffer(buffer);
    const LONGLONG duration = 10000000LL / m_fps;
    if (SUCCEEDED(hr)) hr = sample->SetSampleTime(m_frames * duration);
    if (SUCCEEDED(hr)) hr = sample->SetSampleDuration(duration);
    if (SUCCEEDED(hr)) hr = m_writer->WriteSample(m_stream, sample);
    Release(sample);
    Release(buffer);
    if (FAILED(hr))
    {
        error = HrText("writing a video frame", hr);
        return false;
    }
    m_frames++;
    return true;
}

void VideoWriter::Close()
{
    if (m_writer)
    {
        m_writer->Finalize();
        Release(m_writer);
    }
    if (m_mfStarted)
    {
        MFShutdown();
        m_mfStarted = false;
    }
    m_bgra.clear();
    m_bgra.shrink_to_fit();
}
