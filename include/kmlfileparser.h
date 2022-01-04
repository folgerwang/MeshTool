#pragma once
#include "coremath.h"

struct PolyLineInfo
{
    uint64_t                 num_items;
    unique_ptr<core::vec3d>  data;
    PolyLineInfo() : num_items(0), data(nullptr){}
};

enum GpsConvertMode
{
    kCvtNone,
    kCvtSphereModel,
    kCvtElipsoildModel
};

void ParseGoogleEarthKmlFile(const string& file_name,
                             vector<pair<uint32_t, unique_ptr<core::vec3d[]>>>& lines,
                             vector<pair<uint32_t, unique_ptr<core::vec3d[]>>>& polygons,
                             GpsConvertMode cvt_model = kCvtSphereModel);
void ParseDumpTextureKmlFile(const string& file_name);
