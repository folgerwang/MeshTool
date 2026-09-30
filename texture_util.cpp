#include "texture_util.h"
#include "opencv2/opencv.hpp"
#include "base.h"

#define NVIMAGE_COMPRESSED_RGBA_S3TC_DXT1 0x83F1

void LoadTextureFromFile(const string& file_name, core::Texture2DInfo* tex_info)
{
    cv::Mat src_tex = cv::imread(file_name);
    core::Dxt1Convertor dxt_cvt;
    int w = src_tex.cols;
    int h = src_tex.rows;
    int memory_size = ((w + 3) / 4) * ((h + 3) / 4) * 8;
    if (memory_size > 0)
    {
        tex_info->m_mips[0].m_imageData = make_unique<char[]>(uint32_t(memory_size));
        uint8_t* img_data = reinterpret_cast<uint8_t*>(tex_info->m_mips[0].m_imageData.get());
        dxt_cvt.CompressImageDXT1(src_tex.data, w, h, src_tex.channels(), true, memory_size, img_data);
        tex_info->m_objectId = INVALID_VALUE;
        tex_info->m_levelCount = 1;
        tex_info->m_internalFormat =
            tex_info->m_format =
            tex_info->m_type = NVIMAGE_COMPRESSED_RGBA_S3TC_DXT1;
        tex_info->m_mips[0].m_width = uint32_t(w);
        tex_info->m_mips[0].m_height = uint32_t(h);
        tex_info->m_mips[0].m_size = uint32_t(memory_size);
    }
}
