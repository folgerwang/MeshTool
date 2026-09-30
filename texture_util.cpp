#include "texture_util.h"
#include "base.h"

// The stb implementations are compiled into this translation unit only.
#define STB_IMAGE_IMPLEMENTATION
#define STB_IMAGE_WRITE_IMPLEMENTATION
#include "stb_image.h"
#include "stb_image_write.h"

#define NVIMAGE_COMPRESSED_RGBA_S3TC_DXT1 0x83F1

void LoadTextureFromFile(const string& file_name, core::Texture2DInfo* tex_info)
{
    int w = 0;
    int h = 0;
    int file_channels = 0;
    unique_ptr<uint8_t, void(*)(void*)> src_tex(stbi_load(file_name.c_str(), &w, &h, &file_channels, 3), stbi_image_free);
    if (!src_tex)
    {
        return;
    }

    core::Dxt1Convertor dxt_cvt;
    int memory_size = ((w + 3) / 4) * ((h + 3) / 4) * 8;
    if (memory_size > 0)
    {
        tex_info->m_mips[0].m_imageData = make_unique<char[]>(uint32_t(memory_size));
        uint8_t* img_data = reinterpret_cast<uint8_t*>(tex_info->m_mips[0].m_imageData.get());
        dxt_cvt.CompressImageDXT1(src_tex.get(), w, h, 3, false, memory_size, img_data);
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
