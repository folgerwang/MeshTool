#pragma once
#include "base.h"

enum ShaderList
{
    kBasicVert,
    kBasicNoLightVert,
    kBasicLightVert,
    kScreenQuadVert,
    kBasicPixel,
    kBasicNoLightPixel,
    kBasicLightPixel,
    kScreenQuadPixel,
    kNumShaders,
};

enum ProgramList
{
    kBasicProgram,
    kBasicNoLightProgram,
    kBasicLightProgram,
    kScreenQuadProgram,
    kNumPrograms,
};
