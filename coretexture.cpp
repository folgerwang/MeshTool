#include "coretexture.h"
#include "glfunctionlist.h"
#include <fstream>
#include <algorithm>
#include "assert.h"

namespace core
{

void Dxt1Convertor::CompressImageDXT1( const uint8_t *inBuf, int width, int height, int channels, bool flipChannel, int &outputBytes, uint8_t *outBuf )
{
     uint8_t block[64];
     uint8_t minColor[4];
     uint8_t maxColor[4];
     out_data_ = outBuf;
     flip_channel_ = flipChannel;
     for ( int j = 0; j < height; j += 4, inBuf += width * 4 * channels )
     {
         for ( int i = 0; i < width; i += 4 )
         {
             ExtractBlock( inBuf + i * channels, width, channels, block );
             GetMinMaxColors( block, minColor, maxColor );
             EmitWord( ColorTo565( maxColor ) );
             EmitWord( ColorTo565( minColor ) );
             EmitColorIndices( block, minColor, maxColor );
         }
    }

    outputBytes = int(out_data_ - outBuf);
}

void Dxt1Convertor::DecodeDxt1Texture(uint32_t* dst_iamge_buffer, uint32_t w, uint32_t h, const uint8_t* dxt_src)
{
    uint32_t block_w = (w + 3) / 4;
    uint32_t block_h = (h + 3) / 4;

    for (uint32_t b_y = 0; b_y < block_h; b_y++)
    {
        for (uint32_t b_x = 0; b_x < block_w; b_x++)
        {
            DecodeDxt1Block(dst_iamge_buffer, b_x * 4, b_y * 4, w, dxt_src + (b_y * block_w + b_x) * 8);
        }
    }
}

void Dxt1Convertor::ExtractBlock( const uint8_t *inPtr, int width, int channels, uint8_t *colorBlock )
{
    for ( int j = 0; j < 4; j++ )
    {
        memcpy( &colorBlock[j*4*4], inPtr, size_t(channels) );
        memcpy( &colorBlock[j*4*4+4], inPtr+channels, size_t(channels) );
        memcpy( &colorBlock[j*4*4+8], inPtr+channels*2, size_t(channels) );
        memcpy( &colorBlock[j*4*4+12], inPtr+channels*3, size_t(channels) );
        inPtr += width * channels;
    }
}

uint16_t Dxt1Convertor::ColorTo565( const uint8_t *color )
{
    uint32_t r = color[flip_channel_ ? 2 : 0];
    uint32_t g = color[1];
    uint32_t b = color[flip_channel_ ? 0 : 2];

    return uint16_t(((r >> 3) << 11) | ((g >> 2) << 5) | (b >> 3));
}

void Dxt1Convertor::EmitByte( uint8_t b )
{
    out_data_[0] = b;
    out_data_ += 1;
}

void Dxt1Convertor::EmitWord( uint16_t s )
{
    out_data_[0] = ( s >> 0 ) & 255;
    out_data_[1] = ( s >> 8 ) & 255;
    out_data_ += 2;
}

void Dxt1Convertor::EmitDoubleWord( uint32_t i )
{
    out_data_[0] = ( i >> 0 ) & 255;
    out_data_[1] = ( i >> 8 ) & 255;
    out_data_[2] = ( i >> 16 ) & 255;
    out_data_[3] = ( i >> 24 ) & 255;
    out_data_ += 4;
}

#define INSET_SHIFT 4 // inset the bounding box with ( range >> shift )
void Dxt1Convertor::GetMinMaxColors( const uint8_t *colorBlock, uint8_t *minColor, uint8_t *maxColor )
{
    int i;
    uint8_t inset[3];
    minColor[0] = minColor[1] = minColor[2] = 255;
    maxColor[0] = maxColor[1] = maxColor[2] = 0;
    for ( i = 0; i < 16; i++ )
    {
        if ( colorBlock[i*4+0] < minColor[0] ) { minColor[0] = colorBlock[i*4+0]; }
        if ( colorBlock[i*4+1] < minColor[1] ) { minColor[1] = colorBlock[i*4+1]; }
        if ( colorBlock[i*4+2] < minColor[2] ) { minColor[2] = colorBlock[i*4+2]; }
        if ( colorBlock[i*4+0] > maxColor[0] ) { maxColor[0] = colorBlock[i*4+0]; }
        if ( colorBlock[i*4+1] > maxColor[1] ) { maxColor[1] = colorBlock[i*4+1]; }
        if ( colorBlock[i*4+2] > maxColor[2] ) { maxColor[2] = colorBlock[i*4+2]; }
    }
    inset[0] = ( maxColor[0] - minColor[0] ) >> INSET_SHIFT;
    inset[1] = ( maxColor[1] - minColor[1] ) >> INSET_SHIFT;
    inset[2] = ( maxColor[2] - minColor[2] ) >> INSET_SHIFT;

    minColor[0] = ( minColor[0] + inset[0] <= 255 ) ? minColor[0] + inset[0] : 255;
    minColor[1] = ( minColor[1] + inset[1] <= 255 ) ? minColor[1] + inset[1] : 255;
    minColor[2] = ( minColor[2] + inset[2] <= 255 ) ? minColor[2] + inset[2] : 255;
    maxColor[0] = ( maxColor[0] >= inset[0] ) ? maxColor[0] - inset[0] : 0;
    maxColor[1] = ( maxColor[1] >= inset[1] ) ? maxColor[1] - inset[1] : 0;
    maxColor[2] = ( maxColor[2] >= inset[2] ) ? maxColor[2] - inset[2] : 0;
}

#define C565_5_MASK 0xF8 // 0xFF minus last three bits
#define C565_6_MASK 0xFC // 0xFF minus last two bits
void Dxt1Convertor::EmitColorIndices( const uint8_t *colorBlock, const uint8_t *minColor, const uint8_t *maxColor )
{
    uint16_t colors[4][4];
    uint32_t result = 0;
    colors[0][0] = ( maxColor[0] & C565_5_MASK ) | ( maxColor[0] >> 5 );
    colors[0][1] = ( maxColor[1] & C565_6_MASK ) | ( maxColor[1] >> 6 );
    colors[0][2] = ( maxColor[2] & C565_5_MASK ) | ( maxColor[2] >> 5 );
    colors[1][0] = ( minColor[0] & C565_5_MASK ) | ( minColor[0] >> 5 );
    colors[1][1] = ( minColor[1] & C565_6_MASK ) | ( minColor[1] >> 6 );
    colors[1][2] = ( minColor[2] & C565_5_MASK ) | ( minColor[2] >> 5 );
    colors[2][0] = ( 2 * colors[0][0] + 1 * colors[1][0] ) / 3;
    colors[2][1] = ( 2 * colors[0][1] + 1 * colors[1][1] ) / 3;
    colors[2][2] = ( 2 * colors[0][2] + 1 * colors[1][2] ) / 3;
    colors[3][0] = ( 1 * colors[0][0] + 2 * colors[1][0] ) / 3;
    colors[3][1] = ( 1 * colors[0][1] + 2 * colors[1][1] ) / 3;
    colors[3][2] = ( 1 * colors[0][2] + 2 * colors[1][2] ) / 3;
    for ( int i = 15; i >= 0; i-- )
    {
        int c0 = colorBlock[i*4+0];
        int c1 = colorBlock[i*4+1];
        int c2 = colorBlock[i*4+2];
        int d0 = abs( colors[0][0] - c0 ) + abs( colors[0][1] - c1 ) + abs( colors[0][2] - c2 );
        int d1 = abs( colors[1][0] - c0 ) + abs( colors[1][1] - c1 ) + abs( colors[1][2] - c2 );
        int d2 = abs( colors[2][0] - c0 ) + abs( colors[2][1] - c1 ) + abs( colors[2][2] - c2 );
        int d3 = abs( colors[3][0] - c0 ) + abs( colors[3][1] - c1 ) + abs( colors[3][2] - c2 );
        int b0 = d0 > d3;
        int b1 = d1 > d2;
        int b2 = d0 > d2;
        int b3 = d1 > d3;
        int b4 = d2 > d3;
        int x0 = b1 & b2;
        int x1 = b0 & b3;
        int x2 = b0 & b4;
        result |= ( uint32_t(x2) | ( ( uint32_t(x0) | uint32_t(x1) ) << 1 ) ) << ( i << 1 );
    }
    EmitDoubleWord( result );
}

void Dxt1Convertor::DecodeDxt1Block(uint32_t* dst_iamge_buffer, uint32_t x, uint32_t y, uint32_t w, const uint8_t* dxt_block)
{
    uint32_t color_0 = (uint32_t(dxt_block[1]) << 8) | dxt_block[0];
    uint32_t color_1 = (uint32_t(dxt_block[3]) << 8) | dxt_block[2];
    uint32_t color_0_r = (color_0 << 8) & 0xf80000;
    uint32_t color_0_g = (color_0 << 5) & 0xfc00;
    uint32_t color_0_b = (color_0 << 3) & 0xf8;
    uint32_t color_1_r = (color_1 << 8) & 0xf80000;
    uint32_t color_1_g = (color_1 << 5) & 0xfc00;
    uint32_t color_1_b = (color_1 << 3) & 0xf8;
    uint32_t color8[4];

    color8[0] = color_0_r | color_0_g | color_0_b;
    color8[1] = color_1_r | color_1_g | color_1_b;

    if (color_0 > color_1)
    {
        color8[2] = color8[0] * 2 + color8[1];
        color8[3] = color8[0] + color8[1] * 2;

        uint32_t color_2_r = (color8[2] & 0x3ff0000) / 3;
        uint32_t color_2_g = (color8[2] & 0x3ff00) / 3;
        uint32_t color_2_b = (color8[2] & 0x3ff) / 3;
        uint32_t color_3_r = (color8[3] & 0x3ff0000) / 3;
        uint32_t color_3_g = (color8[3] & 0x3ff00) / 3;
        uint32_t color_3_b = (color8[3] & 0x3ff) / 3;

        color8[2] = (color_2_r & 0xff0000) | (color_2_g & 0xff00) | color_2_b;
        color8[3] = (color_3_r & 0xff0000) | (color_3_g & 0xff00) | color_3_b;
    }
    else
    {
        color8[2] = (color8[0] + color8[1]) >> 1;
        color8[3] = 0;
    }

    uint32_t index_list = dxt_block[4] | (uint32_t(dxt_block[5]) << 8) | (uint32_t(dxt_block[6]) << 16) | (uint32_t(dxt_block[7]) << 24);
    for (uint32_t b_y = 0; b_y < 4; b_y++)
    {
        for (uint32_t b_x = 0; b_x < 4; b_x++)
        {
            dst_iamge_buffer[(y + b_y) * w + x + b_x] = color8[index_list & 0x03];
            index_list >>= 2;
        }
    }
}

void ExportBmpImageFile(const string& file_name, const char* src_buffer, uint32_t buffer_size, uint32_t width, uint32_t height)
{
    ofstream outFile;
    outFile.open(file_name, ofstream::binary);
    BmpHeader bmp_header;
    bmp_header.m_bmpSize = sizeof(BmpHeader) + sizeof(DIBHeader) + buffer_size;
    bmp_header.m_iamgeDataOffset = sizeof(BmpHeader) + sizeof(DIBHeader);
    outFile.write(reinterpret_cast<char*>(&bmp_header), sizeof(BmpHeader));

    DIBHeader dib_header;
    dib_header.m_headSize = sizeof(DIBHeader);
    dib_header.m_width = width;
    dib_header.m_height = height;
    dib_header.m_numBitsPerPixel = 32;
    dib_header.m_pixelFormat = kBiRgb;
    dib_header.m_rawBitmapDataSize = buffer_size;
    outFile.write(reinterpret_cast<char*>(&dib_header), sizeof(DIBHeader));

    outFile.write(src_buffer, buffer_size);
    outFile.close();
}

void ExportBmpImageFile(const core::Texture2DInfo* texture_info, core::TextureFileInfo* tex_file_info)
{
    tex_file_info->is_dds = false;
    bool decode_dds = texture_info->m_format == kGLCmpsdRgbS3tcDxt1Ext || texture_info->m_format == kGLCmpsdRgbaS3tcDxt1Ext;
    uint32_t* dst_iamge_buffer = reinterpret_cast<uint32_t*>(texture_info->m_mips[0].m_imageData.get());
    uint32_t* tmp_image_buffer = nullptr;
    uint32_t w = texture_info->m_mips[0].m_width;
    uint32_t h = texture_info->m_mips[0].m_height;
    uint32_t buffer_size = w * h * 4;
    if (decode_dds)
    {
        tmp_image_buffer = new uint32_t[w * h];
        Dxt1Convertor::DecodeDxt1Texture(tmp_image_buffer, w, h, reinterpret_cast<uint8_t*>(texture_info->m_mips[0].m_imageData.get()));
    }

    uint32_t file_size = sizeof(BmpHeader) + sizeof(DIBHeader) + buffer_size;
    tex_file_info->size = file_size;
    tex_file_info->memory = make_unique<uint8_t[]>(file_size);
    uint8_t* mem_ofs = tex_file_info->memory.get();

    BmpHeader bmp_header;
    bmp_header.m_bmpSize = file_size;
    bmp_header.m_iamgeDataOffset = sizeof(BmpHeader) + sizeof(DIBHeader);
    memcpy(mem_ofs, &bmp_header, sizeof(BmpHeader));
    mem_ofs += sizeof(BmpHeader);

    DIBHeader dib_header;
    dib_header.m_headSize = sizeof(DIBHeader);
    dib_header.m_width = w;
    dib_header.m_height = h;
    dib_header.m_numBitsPerPixel = 32;
    dib_header.m_pixelFormat = kBiRgb;
    dib_header.m_rawBitmapDataSize = buffer_size;
    memcpy(mem_ofs, &dib_header, sizeof(DIBHeader));
    mem_ofs += sizeof(DIBHeader);

    memcpy(mem_ofs, decode_dds ? tmp_image_buffer : dst_iamge_buffer, buffer_size);

    SAFE_ARRAY_DELETE(tmp_image_buffer);
}

void ExportDdsImageFile(const core::Texture2DInfo* texture_info, core::TextureFileInfo* tex_file_info)
{
    tex_file_info->is_dds = true;
    tex_file_info->memory = nullptr;
    tex_file_info->size = 0;

    if (texture_info->m_levelCount == 0)
        return;

    bool is_compressed_texture = false;
    if (texture_info->m_format == kGLCmpsdRgbS3tcDxt1Ext ||
        texture_info->m_format == kGLCmpsdRgbaS3tcDxt1Ext ||
        texture_info->m_format == kGLCmpsdRgbaS3tcDxt3Ext ||
        texture_info->m_format == kGLCmpsdRgbaS3tcDxt5Ext)
        is_compressed_texture = true;

    uint32_t block_size = 16;

    DdsHeaderInfo dds_header;
    DdsHeader& header = dds_header.header;
    //DdsHeaderDx10& header10 = dds_header.header10;

    header.dwReserved2 = 0;
    memset(header.dwReserved1, 0, sizeof(header.dwReserved1));

    bool need_dx10_header = false;

    if (texture_info->m_format == kGlRgb)
    {
        header.ddspf = DdsPixelFormat(kDdsRgb, 0, 24, 0xff0000, 0xff00, 0xff);
    }
    else if (texture_info->m_format == kGlRgba || texture_info->m_format == kGlRgba8)
    {
        header.ddspf = DdsPixelFormat(kDdsRgba, 0, 32, 0xff0000, 0xff00, 0xff, 0xff000000);
    }
    else if (texture_info->m_format == kGlLuminance || texture_info->m_format == kGlAlpha8)
    {
        header.ddspf = DdsPixelFormat(kDdsLuminance, 0, 8, 0xff);
    }
    else if (texture_info->m_format == kGlLuminanceAlpha)
    {
        header.ddspf = DdsPixelFormat(kDdsLuminanceAlpha, 0, 16, 0xff, 0, 0, 0xff00);
    }
    else if (texture_info->m_format == kGLCmpsdRgbS3tcDxt1Ext || texture_info->m_format == kGLCmpsdRgbaS3tcDxt1Ext)
    {
        header.ddspf = DdsPixelFormat(kFourCcDxt1);
        block_size = 8;
    }
    else if (texture_info->m_format == kGLCmpsdRgbaS3tcDxt3Ext)
    {
        header.ddspf = DdsPixelFormat(kFourCcDxt3);
    }
    else if (texture_info->m_format == kGLCmpsdRgbaS3tcDxt5Ext)
    {
        header.ddspf = DdsPixelFormat(kFourCcDxt5);
    }
    else
    {
        // new texture format.
        assert(0);
    }

    dds_header.header.dwSize = 124;
    dds_header.header.dwFlags = kDdsdCaps |
                                kDdsdHeight |
                                kDdsdWidth |
                                (is_compressed_texture ? 0 : kDdsdPitch) |
                                kDdsdPixelFormat |
                                (texture_info->m_levelCount > 1 ? kDdsdMipmapCount : 0) |
                                (is_compressed_texture ? kDdsLinearSize : 0)/* |
                                kDdsDepth*/;
    dds_header.header.dwWidth = texture_info->m_mips[0].m_width;
    dds_header.header.dwHeight = texture_info->m_mips[0].m_height;
    dds_header.header.dwMipMapCount = texture_info->m_levelCount;
    dds_header.header.dwDepth = 1;
    if (is_compressed_texture)
        dds_header.header.dwPitchOrLinearSize = max(1u, (texture_info->m_mips[0].m_width + 3) / 4) * block_size;
    else
        dds_header.header.dwPitchOrLinearSize = (texture_info->m_mips[0].m_width * dds_header.header.ddspf.dwRgbBitCount + 7) / 8;
    dds_header.header.dwCaps = kDdsCapsTexture;
    if (texture_info->m_levelCount > 1)
    {
        dds_header.header.dwCaps |= (kDdsCapsComplex | kDdsCapsMipmap);
    }
    dds_header.header.dwCaps2 = 0;
    dds_header.header.dwCaps3 = dds_header.header.dwCaps4 = 0;

    uint32_t header_size = sizeof(dds_header);
    if (!need_dx10_header)
        header_size -= sizeof(DdsHeaderDx10);

    uint32_t file_size = header_size;
    for (uint32_t i = 0; i < texture_info->m_levelCount; i++)
    {
        file_size += texture_info->m_mips[i].m_size;
    }

    tex_file_info->size = file_size;
    tex_file_info->memory = make_unique<uint8_t[]>(file_size);
    uint8_t* mem_ofs = tex_file_info->memory.get();

    memcpy(mem_ofs, &dds_header, header_size);
    mem_ofs += header_size;
    for (uint32_t i = 0; i < texture_info->m_levelCount; i++)
    {
        memcpy(mem_ofs, texture_info->m_mips[i].m_imageData.get(), texture_info->m_mips[i].m_size);
        mem_ofs += texture_info->m_mips[i].m_size;
    }
}
}

