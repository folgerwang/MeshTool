#pragma once

#include <cstdint>
#include "meshdata.h"

// Display and prompt metadata for ObjectClass (meshdata.h).
struct ObjectClassInfo
{
    const char* name;       // object name prefix and UI label
    char        letter;     // code in Qwen answers
    float       color[3];   // class-colour view
};

inline const ObjectClassInfo& GetObjectClassInfo(ObjectClass cls)
{
    static const ObjectClassInfo kInfo[kObjClassCount] = {
        { "unknown",  '?', { 0.55f, 0.55f, 0.55f } },
        { "ground",   'G', { 0.72f, 0.66f, 0.55f } },
        { "road",     'R', { 0.32f, 0.32f, 0.36f } },
        { "building", 'B', { 0.90f, 0.45f, 0.25f } },
        { "car",      'C', { 0.95f, 0.15f, 0.55f } },
        { "tree",     'T', { 0.15f, 0.55f, 0.18f } },
        { "plants",   'P', { 0.55f, 0.85f, 0.30f } },
        { "water",    'W', { 0.20f, 0.45f, 0.95f } },
    };
    return kInfo[cls < kObjClassCount ? cls : kObjUnknown];
}

inline ObjectClass ObjectClassFromLetter(char c)
{
    if (c >= 'a' && c <= 'z') c = char(c - 'a' + 'A');
    for (int i = 1; i < kObjClassCount; i++)
        if (GetObjectClassInfo(ObjectClass(i)).letter == c)
            return ObjectClass(i);
    return kObjUnknown;
}

// Raised objects are split into instances (building_012); ground classes
// become one area object each (road, water, ...).
inline bool IsInstanceClass(ObjectClass cls)
{
    return cls == kObjBuilding || cls == kObjCar || cls == kObjTree;
}

// A colour per index (building, capture, ...): hues a golden-ratio step
// apart, so neighbouring indices never look alike.
inline void DistinctColor(int32_t index, float rgb[3])
{
    float h = float(index) * 0.618034f;
    h = (h - float(int(h))) * 6.0f;
    const float s = (index & 1) ? 0.55f : 0.75f, v = (index & 2) ? 0.80f : 0.95f;
    int i = int(h);
    float f = h - float(i);
    float p = v * (1.0f - s), q = v * (1.0f - s * f), t = v * (1.0f - s * (1.0f - f));
    switch (i % 6)
    {
    case 0: rgb[0] = v; rgb[1] = t; rgb[2] = p; break;
    case 1: rgb[0] = q; rgb[1] = v; rgb[2] = p; break;
    case 2: rgb[0] = p; rgb[1] = v; rgb[2] = t; break;
    case 3: rgb[0] = p; rgb[1] = q; rgb[2] = v; break;
    case 4: rgb[0] = t; rgb[1] = p; rgb[2] = v; break;
    default: rgb[0] = v; rgb[1] = p; rgb[2] = q; break;
    }
}
