#pragma once
#include "corevector.h"
#include "corematrix.h"
#include "corequaternion.h"
#include "corebounds.h"

namespace core
{
constexpr double PI = 3.1415926535897932384626;
typedef vec2<float> vec2f; ///< float 2-vectors
typedef vec2<double> vec2d; ///< double 2-vectors
typedef vec2<int32_t> vec2i; ///< signed integer 2-vectors
typedef vec2<uint32_t> vec2ui; ///< unsigned integer 2-vectors
typedef vec3<float> vec3f; ///< float 3-vectors
typedef vec3<double> vec3d; ///< double 3-vectors
typedef vec3<int32_t> vec3i; ///< signed integer 3-vectors
typedef vec3<uint32_t> vec3ui; ///< unsigned integer 4-vectors
typedef vec4<float> vec4f; ///< float 4-vectors
typedef vec4<double> vec4d; ///< double 4-vectors
typedef vec4<int32_t> vec4i; ///< signed integer 4-vectors
typedef vec4<uint32_t> vec4ui; ///< unsigned integer 4-vectors
typedef matrix3<float> matrix3f; ///< float 4x4 matrices
typedef matrix3<double> matrix3d; ///< float 4x4 matrices
typedef matrix4<float> matrix4f; ///< float 4x4 matrices
typedef matrix4<double> matrix4d; ///< float 4x4 matrices
typedef quaternion<float> quaternionf; ///< float quaternions

typedef bounds2<float> bounds2f;
typedef bounds2<double> bounds2d;
typedef bounds2<int32_t> bounds2i;
typedef bounds2<uint32_t> bounds2ui;
typedef bounds3<float> bounds3f;
typedef bounds3<double> bounds3d;
typedef bounds3<int32_t> bounds3i;
typedef bounds3<uint32_t> bounds3ui;
typedef vec3d GpsCoord;

template<typename T>
T DegreesToRadians(T degree)
{
    return T(degree / 180.0 * PI);
}

template<typename T>
T RadiansToDegrees(T radians)
{
    return T(radians / PI * 180.0);
}
}
