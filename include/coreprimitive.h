#pragma once
#include "coremath.h"

class CoreGLSLProgram;

namespace core
{
#define SAFE_RELEASE(p) { if ( (p) ) { (p)->Release(); (p) = nullptr; } }
#define SAFE_DELETE(p) if( (p) ) delete (p); (p) = nullptr;
#define SAFE_ARRAY_DELETE(p) if( (p) ) delete[] (p); (p) = nullptr;

struct Primitive
{
    bounds3f			   bbox_ws;
    bounds3d               bbox_gps;

    virtual void draw(CoreGLSLProgram* program) = 0;
};

};
