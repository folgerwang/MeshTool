#pragma once
#include "base.h"

namespace core
{

template <class T> class vec2;
template <class T> class vec3;
template <class T> class vec4;

template <class T>
struct bounds3
{
    vec3<T>		bb_min;
    vec3<T>		bb_max;
    bool			b_valid = false;

    bounds3() { b_valid = false; }

    void Reset() { b_valid = false; }

    vec3<T> GetCentroid() const
    {
        return (bb_min + bb_max) / 2;
    }

    vec3<T> GetDiagonal() const
    {
        return bb_max - bb_min;
    }

    T GetRadius() const
    {
        vec3<T> diag = GetDiagonal();
        return length(diag) * 0.5;
    }

    int MaxExtent() const
    {
        vec3<T> d = GetDiagonal();
        if (d.x > d.y && d.x > d.z)
            return 0;
        else if (d.y > d.z)
            return 1;
        else
            return 2;
    }

    T SurfaceArea() const {
        vec3<T> d = GetDiagonal();
        return 2 * (d.x * d.y + d.x * d.z + d.y * d.z);
    }

    T Volume() const {
        vec3<T> d = GetDiagonal();
        return d.x * d.y * d.z;
    }

    vec3<T> Offset(const vec3<T> &p) const {
        vec3<T> o = p - bb_min;
        if (bb_max.x > bb_min.x) o.x /= bb_max.x - bb_min.x;
        if (bb_max.y > bb_min.y) o.y /= bb_max.y - bb_min.y;
        if (bb_max.z > bb_min.z) o.z /= bb_max.z - bb_min.z;
        return o;
    }

    vec3<T> & operator [] (int32_t i) {
        return i == 0 ? bb_min : bb_max;
    }

    const vec3<T> & operator [] (int32_t i) const {
        return i == 0 ? bb_min : bb_max;
    }

    friend bounds3<T>& operator += (bounds3<T>& lhs, const vec3<T> & rhs)
    {
        if (!lhs.b_valid)
        {
            lhs.bb_min = rhs;
            lhs.bb_max = rhs;
            lhs.b_valid = true;
        }
        else
        {
            lhs.bb_min = Min(lhs.bb_min, rhs);
            lhs.bb_max = Max(lhs.bb_max, rhs);
        }
        return lhs;
    }

    friend bounds3<T>& operator += (bounds3<T>& lhs, const bounds3<T> & rhs)
    {
        if (!lhs.b_valid)
        {
            lhs = rhs;
        }
        else if (rhs.b_valid)
        {
            lhs.bb_min = Min(lhs.bb_min, rhs.bb_min);
            lhs.bb_max = Max(lhs.bb_max, rhs.bb_max);
        }
        return lhs;
    }

    friend bounds3<T> operator + (bounds3<T>& lhs, const bounds3<T> & rhs)
    {
        bounds3<T> res;

        if (!lhs.b_valid)
        {
            res = rhs;
        }
        else if (rhs.b_valid)
        {
            res.bb_min = Min(lhs.bb_min, rhs.bb_min);
            res.bb_max = Max(lhs.bb_max, rhs.bb_max);
        }
        return res;
    }

    friend bounds3<T> operator ^ (bounds3<T>& lhs, const bounds3<T> & rhs)
    {
        bounds3<T> res;

        if (lhs.b_valid && rhs.b_valid)
        {
            bounds3<T> tmp_bbox;
            tmp_bbox.bb_min = Max(lhs.bb_min, rhs.bb_min);
            tmp_bbox.bb_max = Min(lhs.bb_max, rhs.bb_max);

            if (tmp_bbox.bb_min.x < tmp_bbox.bb_max.x &&
                tmp_bbox.bb_min.y < tmp_bbox.bb_max.y &&
                tmp_bbox.bb_min.z < tmp_bbox.bb_max.z)
            {
                res = tmp_bbox;
            }
        }
        return res;
    }

    friend bool operator == ( const bounds3<T> &lhs, const bounds3<T> &rhs )
    {
        return lhs.bb_min == rhs.bb_min && lhs.bb_max == rhs.bb_max;
    }

    friend bool operator != ( const bounds3<T> &lhs, const bounds3<T> &rhs )
    {
        return lhs.bb_min != rhs.bb_min || lhs.bb_max != rhs.bb_max;
    }
};

template <class T>
struct bounds2
{
    vec2<T>		bb_min;
    vec2<T>		bb_max;
    bool		b_valid = false;

    bounds2() { b_valid = false; }
    bounds2(const bounds3<T>& bbox)
    {
        b_valid = bbox.b_valid;
        bb_min = bbox.bb_min;
        bb_max = bbox.bb_max;
    }

    void Reset() { b_valid = false; }

    vec2<T> GetCentroid() const
    {
        return (bb_min + bb_max) / 2;
    }

    vec2<T> GetDiagonal() const
    {
        return bb_max - bb_min;
    }

    T GetRadius() const
    {
        vec2<T> diag = GetDiagonal();
        return length(diag) * 0.5;
    }

    int MaxExtent() const
    {
        vec2<T> d = GetDiagonal();
        if (d.x > d.y)
            return 0;
        else
            return 1;
    }

    T SurfaceArea() const {
        vec2<T> d = GetDiagonal();
        return d.x * d.y;
    }

    T Volume() const {
        vec2<T> d = GetDiagonal();
        return d.x * d.y;
    }

    vec2<T> Offset(const vec2<T> &p) const {
        vec2<T> o = p - bb_min;
        if (bb_max.x > bb_min.x) o.x /= bb_max.x - bb_min.x;
        if (bb_max.y > bb_min.y) o.y /= bb_max.y - bb_min.y;
        return o;
    }

    vec2<T> & operator [] (int32_t i) {
        return i == 0 ? bb_min : bb_max;
    }

    const vec2<T> & operator [] (int32_t i) const {
        return i == 0 ? bb_min : bb_max;
    }

    friend bounds2<T>& operator += (bounds2<T>& lhs, const vec2<T> & rhs)
    {
        if (!lhs.b_valid)
        {
            lhs.bb_min = rhs;
            lhs.bb_max = rhs;
            lhs.b_valid = true;
        }
        else
        {
            lhs.bb_min = Min(lhs.bb_min, rhs);
            lhs.bb_max = Max(lhs.bb_max, rhs);
        }
        return lhs;
    }

    friend bounds2<T>& operator += (bounds2<T>& lhs, const bounds2<T> & rhs)
    {
        if (!lhs.b_valid)
        {
            lhs = rhs;
        }
        else if (rhs.b_valid)
        {
            lhs.bb_min = Min(lhs.bb_min, rhs.bb_min);
            lhs.bb_max = Max(lhs.bb_max, rhs.bb_max);
        }
        return lhs;
    }

    friend bounds2<T> operator + (bounds2<T>& lhs, const bounds2<T> & rhs)
    {
        bounds2<T> res;

        if (!lhs.b_valid)
        {
            res = rhs;
        }
        else if (rhs.b_valid)
        {
            res.bb_min = Min(lhs.bb_min, rhs.bb_min);
            res.bb_max = Max(lhs.bb_max, rhs.bb_max);
        }
        return res;
    }

    friend bounds2<T> operator ^ (const bounds2<T>& lhs, const bounds2<T> & rhs)
    {
        bounds2<T> res;

        if (lhs.b_valid && rhs.b_valid)
        {
            bounds2<T> tmp_bbox;
            tmp_bbox.bb_min = Max(lhs.bb_min, rhs.bb_min);
            tmp_bbox.bb_max = Min(lhs.bb_max, rhs.bb_max);

            if (tmp_bbox.bb_min.x < tmp_bbox.bb_max.x && tmp_bbox.bb_min.y < tmp_bbox.bb_max.y)
            {
                res = tmp_bbox;
                res.b_valid = true;
            }
        }
        return res;
    }

    friend bool operator == ( const bounds2<T> &lhs, const bounds2<T> &rhs )
    {
        return lhs.bb_min == rhs.bb_min && lhs.bb_max == rhs.bb_max;
    }

    friend bool operator != ( const bounds2<T> &lhs, const bounds2<T> &rhs )
    {
        return lhs.bb_min != rhs.bb_min || lhs.bb_max != rhs.bb_max;
    }
};
};
