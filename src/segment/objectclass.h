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
