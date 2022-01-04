#include "assert.h"
#include <fstream>
#include <sstream>
#include <filesystem>
#include "opencv2/opencv.hpp"

#include <QMessageBox>
#include "coremath.h"
#include "corefile.h"
#include "coregeographic.h"
#include "GpaDumpAnalyzeTool.h"
#include "coregeographic.h"
#include "meshdata.h"
#include "meshtexture.h"
#include "worlddata.h"
#include "kmlfileparser.h"
#include <fbxsdk.h>
#include "hfa/hfa_p.h"
#include "hfa/hfa.h"

using namespace std;
namespace fs = experimental::filesystem::v1;

void TFWInfo::ReadTfwFile(const string& file_name)
{
    uint32_t length = 0;
    char* memory = core::LoadFileToMemory(file_name, length);
    if (memory)
    {
        stringstream string_buf(memory);

        string_buf >> A; //Line 1 : A : pixel size in the x - direction in map units / pixel
        string_buf >> D; //Line 2 : D : rotation about y - axis
        string_buf >> B; //Line 3 : B : rotation about x - axis
        string_buf >> E; //Line 4 : E : pixel size in the y - direction in map units, almost always negative[3]
        string_buf >> C; //Line 5 : C : x - coordinate of the center of the upper left pixel
        string_buf >> F; //Line 6 : F : y - coordinate of the center of the upper left pixel
    }
}

void XMLInfo::ReadXmlFile(const string& file_name)
{
    uint32_t length = 0;
    char* memory = core::LoadFileToMemory(file_name, length);
    stringstream string_buf(memory);

    string tmp_string;
    while (string_buf)
    {
        string_buf >> tmp_string;

        if (tmp_string == "<bounding>")
        {
            for (int i = 0; i < 4; i++)
            {
                string_buf >> tmp_string;
                size_t s_t = tmp_string.find('>');
                size_t e_t = tmp_string.rfind('<');
                double t;
                stringstream(tmp_string.substr(s_t + 1, e_t - s_t - 1)) >> t;

                if (tmp_string.substr(0, s_t + 1) == "<westbc>")
                {
                    west_bc = t;
                }
                else if (tmp_string.substr(0, s_t + 1) == "<eastbc>")
                {
                    east_bc = t;
                }
                else if (tmp_string.substr(0, s_t + 1) == "<northbc>")
                {
                    north_bc = t;
                }
                else if (tmp_string.substr(0, s_t + 1) == "<southbc>")
                {
                    south_bc = t;
                }
            }
        }
        else if (tmp_string == "<utm>")
        {
            string_buf >> tmp_string;
            size_t s_t = tmp_string.find('>');
            size_t e_t = tmp_string.rfind('<');

            stringstream(tmp_string.substr(s_t + 1, e_t - s_t - 1)) >> zone;
        }
        else
        {
            size_t s_t = tmp_string.find('>');
            size_t e_t = tmp_string.rfind('<');
            double t;
            stringstream(tmp_string.substr(s_t + 1, e_t - s_t - 1)) >> t;

            if (tmp_string.substr(0, s_t + 1) == "<west>")
            {
                west_bc = t;
            }
            else if (tmp_string.substr(0, s_t + 1) == "<east>")
            {
                east_bc = t;
            }
            else if (tmp_string.substr(0, s_t + 1) == "<north>")
            {
                north_bc = t;
            }
            else if (tmp_string.substr(0, s_t + 1) == "<south>")
            {
                south_bc = t;
            }

        }
    }
}

void DumpUSGSTexture(const vector<shared_ptr<string>>& file_name_list,
                     vector<DumppedTextureInfo>& dumpped_texture_info_list)
{
    for (uint32_t i = 0; i < file_name_list.size(); i++)
    {
        DumppedTextureInfo dumpped_tex_info;
        dumpped_tex_info.file_name = file_name_list[i];
        const string& base_name = file_name_list[i].get()->substr(0, file_name_list[i].get()->rfind("."));

    #if		USE_USGS_MAP_TEXTURE
        dumpped_tex_info.tex_body = cv::imread(base_name + ".jpg");
        dumpped_tex_info.tex_id = LoadMapTexture(dumpped_tex_info.tex_body.cols, dumpped_tex_info.tex_body.rows, dumpped_tex_info.tex_body.data);
    #else
        // load dds file.
        {
#if 0
            core::Texture2DInfo texture_info;
            uint32_t len = 0;
            uint8_t* ddsData = reinterpret_cast<uint8_t*>(core::LoadFileToMemory(file_name_list[i], len));
            ddsData += 4;
            core::DdsHeader* ddsh = reinterpret_cast<core::DdsHeader*>(ddsData);
            ddsData += sizeof(core::DdsHeader);
            core::DdsHeaderDx10* ddsh10 = nullptr;

            if ((ddsh->ddspf.dwFlags & core::kDdsFourcc) && (ddsh->ddspf.dwFourCC == core::kFourCcDx10))
            {
                //This DDS file uses the DX10 header extension
                //fread(&ddsh10, sizeof(DDS_HEADER_10), 1, fp);
                ddsh10 = reinterpret_cast<core::DdsHeaderDx10*>(ddsData);
                ddsData += sizeof(core::DdsHeaderDx10);
            }

            int32_t blockSizeX = 4;
            int32_t blockSIzeY = 4;
            uint32_t bytesPerElement;

            texture_info.m_levelCount = ddsh->dwMipMapCount;

             switch (ddsh->ddspf.dwFourCC)
             {
             case core::kFourCcDxt1:
                 texture_info.m_format = NVIMAGE_COMPRESSED_RGBA_S3TC_DXT1;
                 texture_info.m_internalFormat = NVIMAGE_COMPRESSED_RGBA_S3TC_DXT1;
                 texture_info.m_type = NVIMAGE_COMPRESSED_RGBA_S3TC_DXT1;
                 bytesPerElement = 8;
                 break;

             case core::kFourCcDxt2:
             case core::kFourCcDxt3:
                 texture_info.m_format = NVIMAGE_COMPRESSED_RGBA_S3TC_DXT3;
                 texture_info.m_internalFormat = NVIMAGE_COMPRESSED_RGBA_S3TC_DXT3;
                 texture_info.m_type = NVIMAGE_COMPRESSED_RGBA_S3TC_DXT3;
                 bytesPerElement = 16;
                 break;

             case core::kFourCcDxt4:
             case core::kFourCcDxt5:
                 texture_info.m_format = NVIMAGE_COMPRESSED_RGBA_S3TC_DXT5;
                 texture_info.m_internalFormat = NVIMAGE_COMPRESSED_RGBA_S3TC_DXT5;
                 texture_info.m_type = NVIMAGE_COMPRESSED_RGBA_S3TC_DXT5;
                 bytesPerElement = 16;
                 break;
             }

            int32_t w = int32_t(ddsh->dwWidth), h = int32_t(ddsh->dwHeight), d = 1;
            for (uint32_t level = 0; level < ddsh->dwMipMapCount; level++)
            {
                int32_t bw = (w - 1) / blockSizeX + 1;
                int32_t bh = (h - 1) / blockSIzeY + 1;
                int32_t readSize = bw * bh * d * int32_t(bytesPerElement);

                texture_info.m_mips[level].m_width = uint32_t(w);
                texture_info.m_mips[level].m_height = uint32_t(h);
                texture_info.m_mips[level].m_size = uint32_t(readSize);
                texture_info.m_mips[level].m_imageData = reinterpret_cast<char*>(ddsData);
                ddsData += readSize;

                //reduce mip sizes
                w = (w > 1) ? w >> 1 : 1;
                h = (h > 1) ? h >> 1 : 1;
                d = (d > 1) ? d >> 1 : 1;
            }

 /*           texture_info.m_texId = 0;
            glGenTextures(1, &texture_info.m_texId);

            glBindTexture(GL_TEXTURE_2D, texture_info.m_texId);
            dumpped_tex_info.tex_id = texture_info.m_texId;

            glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_CLAMP);
            glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_CLAMP);
            glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_LINEAR);
            glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_LINEAR);

            for (uint32_t l = 0; l < texture_info.m_levelCount; l++)
            {
                glCompressedTexImage2D(GL_TEXTURE_2D, l, texture_info.m_internalFormat, texture_info.m_mips[l].m_width, texture_info.m_mips[l].m_height,
                    0, texture_info.m_mips[l].m_size, texture_info.m_mips[l].m_imageData);
            }*/
#endif
        }
    #endif

        dumpped_tex_info.xml_info.ReadXmlFile(base_name + ".kml");

        dumpped_tex_info.bbox += core::vec2d(dumpped_tex_info.xml_info.east_bc, dumpped_tex_info.xml_info.north_bc);
        dumpped_tex_info.bbox += core::vec2d(dumpped_tex_info.xml_info.west_bc, dumpped_tex_info.xml_info.south_bc);

        dumpped_texture_info_list.push_back(dumpped_tex_info);
    }
}
