#pragma once
#include "coremath.h"
#include "coretexture.h"
#include "opencv2/opencv.hpp"

#define USE_USGS_MAP_TEXTURE 0

struct TFWInfo
{
    double A, B, C, D, E, F;

    void ReadTfwFile(const string& file_name);

    core::vec2d ToUTMLocation(const core::vec2i& pixel_loc)
    {
        core::vec2d r;
        r.x = pixel_loc.x * A + pixel_loc.y * B + C;
        r.y = pixel_loc.x * D + pixel_loc.y * E + F;

        return r;
    }

    core::vec2f FromUTMLocation(const core::vec2d& utm_loc, const core::vec2f& inv_img_size) const
    {
        core::vec2f r;
        double delta = A * E - D * B;
        r.x = float((E * utm_loc.x - B * utm_loc.y + B * F - E * C) / delta) * inv_img_size.x;
        r.y = float((-D * utm_loc.x + A * utm_loc.y + D * C - A * F) / delta) * inv_img_size.y;

        return r;
    }

    core::vec2d GetUTMLocation()
    {
        return core::vec2d(C, F);
    }
};

struct XMLInfo
{
    double	west_bc;
    double	east_bc;
    double  north_bc;
    double  south_bc;
    int32_t zone;

    void ReadXmlFile(const string& file_name);
};

struct DumppedTextureInfo
{
    shared_ptr<string> file_name;
    core::bounds2d	bbox;
    uint32_t        tex_id;
    cv::Mat         tex_body;
    TFWInfo         tfw_info;
    XMLInfo         xml_info;
};
