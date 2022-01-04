#pragma once
#include "base.h"
#include "coreprimitive.h"

namespace core
{

enum BiFormat
{
    kBiRgb,				//none. most common
    kBiRle8,			//8 - bit / pixel	Can be used only with 8 - bit / pixel bitmaps
    kBiRle4,			//4 - bit / pixel	Can be used only with 4 - bit / pixel bitmaps
    kBiBitFields,		//OS22XBITMAPHEADER : Huffman 1D	BITMAPV2INFOHEADER : RGB bit field masks, BITMAPV3INFOHEADER + : RGBA
    kBiJpeg,			//OS22XBITMAPHEADER : RLE - 24	BITMAPV4INFOHEADER + : JPEG image for printing[12]
    kBiPng,				//BITMAPV4INFOHEADER + : PNG image for printing[12]
    kBiAlphaBitFields,	//RGBA bit field masks	only Windows CE 5.0 with.NET 4.0 or later
    kBiCMYK,			//none	only Windows Metafile CMYK[3]
    kBiCMYKRLE8,		//RLE - 8	only Windows Metafile CMYK
    kBiCMYKRLE4,		//RLE - 4	only Windows Metafile CMYK
};

#pragma pack(1)
struct BmpHeader
{
    char		m_b;
    char		m_m;
    uint32_t	m_bmpSize;
    uint16_t	m_reserve0;
    uint16_t	m_reserve1;
    uint32_t	m_iamgeDataOffset;

    BmpHeader() : m_b('B'), m_m('M'), m_reserve0(0), m_reserve1(0) {}
};

struct DIBHeader
{
    uint32_t	m_headSize;
    uint32_t	m_width;
    uint32_t	m_height;
    uint16_t	m_numColorPlanes;
    uint16_t	m_numBitsPerPixel;
    BiFormat	m_pixelFormat;
    uint32_t	m_rawBitmapDataSize;
    uint32_t	m_printResolutionX;
    uint32_t	m_printResolutionY;
    uint32_t	m_numColorsInPallet;
    uint32_t	m_numImportantColors;

    DIBHeader() : m_numColorPlanes(1),
                  m_printResolutionX(2835),
                  m_printResolutionY(2835),
                  m_numColorsInPallet(0),
                  m_numImportantColors(0)
    {}
};

enum
{
    kDdsFourcc = 0x00000004,  // DDPF_FOURCC
    kDdsRgb = 0x00000040,  // DDPF_RGB
    kDdsRgba = 0x00000041,  // DDPF_RGB | DDPF_ALPHAPIXELS
    kDdsLuminance = 0x00020000,  // DDPF_LUMINANCE
    kDdsLuminanceAlpha = 0x00020001,  // DDPF_LUMINANCE | DDPF_ALPHAPIXELS
    kDdsAlpha = 0x00000002,  // DDPF_ALPHA
    kDdsPal8 = 0x00000020,  // DDPF_PALETTEINDEXED8
};

struct DdsPixelFormat {
    uint32_t		dwSize;
    uint32_t		dwFlags;
    uint32_t		dwFourCC;
    uint32_t		dwRgbBitCount;
    uint32_t		dwRBitMask;
    uint32_t		dwGBitMask;
    uint32_t		dwBBitMask;
    uint32_t		dwABitMask;

    DdsPixelFormat() : dwSize(sizeof(DdsPixelFormat)) {}

    DdsPixelFormat(uint32_t _dwFourCC) :
        dwSize(sizeof(DdsPixelFormat)),
        dwFlags(kDdsFourcc),
        dwFourCC(_dwFourCC),
        dwRgbBitCount(0),
        dwRBitMask(0),
        dwGBitMask(0),
        dwBBitMask(0),
        dwABitMask(0)
        {}

    DdsPixelFormat(uint32_t _dwFlags,
                   uint32_t _dwFourCC,
                   uint32_t _dwRgbBitCount,
                   uint32_t _dwRBitMask,
                   uint32_t _dwGBitMask = 0,
                   uint32_t _dwBBitMask = 0,
                   uint32_t _dwABitMask = 0) :
        dwSize(sizeof(DdsPixelFormat)),
        dwFlags(_dwFlags),
        dwFourCC(_dwFourCC),
        dwRgbBitCount(_dwRgbBitCount),
        dwRBitMask(_dwRBitMask),
        dwGBitMask(_dwGBitMask),
        dwBBitMask(_dwBBitMask),
        dwABitMask(_dwABitMask)
    {}
};

enum
{
    kDdsdCaps		= 0x01,
    kDdsdHeight		= 0x02,
    kDdsdWidth		= 0x04,
    kDdsdPitch		= 0x08,
    kDdsdPixelFormat = 0x1000,
    kDdsdMipmapCount = 0x20000,
    kDdsLinearSize	 = 0x80000,
    kDdsDepth		 = 0x800000,
};

struct DdsHeader {
    uint32_t         dwSize;
    uint32_t         dwFlags;
    uint32_t         dwHeight;
    uint32_t         dwWidth;
    uint32_t         dwPitchOrLinearSize;
    uint32_t         dwDepth;
    uint32_t         dwMipMapCount;
    uint32_t         dwReserved1[11];
    DdsPixelFormat	 ddspf;
    uint32_t         dwCaps;
    uint32_t         dwCaps2;
    uint32_t         dwCaps3;
    uint32_t         dwCaps4;
    uint32_t         dwReserved2;
} ;

enum DdsDimensionInfo
{
    kDdsDimensionTexture1D = 2,
    kDdsDimensionTexture2D = 3,
    kDdsDimensionTexture3D = 4,
};

enum
{
    kDdsCapsComplex = 0x08,
    kDdsCapsMipmap = 0x400000,
    kDdsCapsTexture = 0x1000,
};

enum DdsFourCc
{
    kFourCcDx10			= 0x30315844,	//"DX10"
    kFourCcDxt1			= 0x31545844,	//"DXT1"
    kFourCcDxt2			= 0x32545844,	//"DXT2"
    kFourCcDxt3			= 0x33545844,	//"DXT3"
    kFourCcDxt4			= 0x34545844,	//"DXT4"
    kFourCcDxt5			= 0x35545844,	//"DXT5"
};

enum
{
    kDdsAlphaModeUnknown	= 0x00,
    kDdsAlphaModeStraight	= 0x01,
    kDdsAlphaModePremultiplied = 0x02,
    kDdsAlphaModeOpaque		= 0x03,
    kDdsAlphaModeCustom		= 0x04,
};

enum DxgiFormat
{
    kDxgiRgba32f = 2,
    kDxgiRgb32f = 6,
    kDxgiRgba16f = 10,
    kDxgiRg11b10f = 26,
    kDxgiRgba8Unorm = 28,
    kDxgiR32f = 41,
    kDxgiRg8Unorm = 49,
    kDxgiR8Unorm = 61,
    kDxgiA8Unorm = 65,
    kDxgiBc1Unorm = 71,
    kDxgiBc2Unorm = 74,
    kDxgiBc3Unorm = 77,
    kDxgiBc4Unorm = 80,
    kDxgiBc4Snorm = 81,
    kDxgiBc5Unorm = 83,
    kDxgiBc5Snorm = 84,
    kDxgiBgra8Unorm = 87,
    kDxgiBc6hUf16 = 95,
    kDxgiBc6hSf16 = 96,
    kDxgiBc7Unorm = 98,
};

#define    NVIMAGE_COMPRESSED_RGB_S3TC_DXT1    0x83F0
#define    NVIMAGE_COMPRESSED_RGBA_S3TC_DXT1    0x83F1
#define    NVIMAGE_COMPRESSED_RGBA_S3TC_DXT3    0x83F2
#define    NVIMAGE_COMPRESSED_RGBA_S3TC_DXT5    0x83F3

struct DdsHeaderDx10
{
    DxgiFormat        dxgiFormat;
    DdsDimensionInfo  resourceDimension;
    uint32_t          miscFlag;
    uint32_t          arraySize;
    uint32_t          miscFlags2;
};

struct DdsHeaderInfo
{
    uint32_t		dwMagic;
    DdsHeader		header;
    DdsHeaderDx10	header10;

    DdsHeaderInfo() : dwMagic(0x20534444) {}
};

struct Dxt1Convertor
{
    void CompressImageDXT1( const uint8_t *inBuf, int width, int height, int channels, bool flipChannel, int &outputBytes, uint8_t *outBuf );
    static void DecodeDxt1Texture(uint32_t* dst_iamge_buffer, uint32_t w, uint32_t h, const uint8_t* dxt_src);

private:
    void ExtractBlock( const uint8_t *inPtr, int width, int channels, uint8_t *colorBlock );
    uint16_t ColorTo565( const uint8_t *color );
    void EmitByte( uint8_t b );
    void EmitWord( uint16_t s );
    void EmitDoubleWord( uint32_t i );
    void GetMinMaxColors( const uint8_t *colorBlock, uint8_t *minColor, uint8_t *maxColor );
    void EmitColorIndices( const uint8_t *colorBlock, const uint8_t *minColor, const uint8_t *maxColor );
    static void DecodeDxt1Block(uint32_t* dst_iamge_buffer, uint32_t x, uint32_t y, uint32_t w, const uint8_t* dxt_block);

    uint8_t *out_data_;
    bool flip_channel_ = false;
};

#pragma pack()

struct TextureFileInfo
{
    bool            is_dds;
    string          tex_name;
    uint32_t        size;
    unique_ptr<uint8_t[]> memory;

    TextureFileInfo() : is_dds(false),
                        size(0),
                        memory(nullptr)
    {}
};

struct Texture2DSurfaceInfo
{
    uint32_t		m_width;
    uint32_t		m_height;
    uint32_t		m_size;
    unique_ptr<char[]>	m_imageData;

    Texture2DSurfaceInfo() : m_width(0),
                             m_height(0),
                             m_size(0),
                             m_imageData(nullptr)
    {
    }
};

struct Texture2DInfo
{
    uint32_t                m_objectId;
    uint32_t                m_levelCount;
    uint32_t                m_internalFormat;
    uint32_t                m_format;
    uint32_t                m_type;
    Texture2DSurfaceInfo	m_mips[15];
};

void ExportBmpImageFile(const string& file_name, const char* src_buffer, uint32_t buffer_size, uint32_t width, uint32_t height);
void ExportBmpImageFile(const core::Texture2DInfo* texture_info, core::TextureFileInfo* tex_file_info);
void ExportDdsImageFile(const core::Texture2DInfo* texture_info, core::TextureFileInfo* tex_file_info);

};
