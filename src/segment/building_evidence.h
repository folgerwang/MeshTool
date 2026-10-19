#pragma once
#include <array>
#include <vector>
#include <algorithm>
#include <cmath>
#include <limits>

// Keep thresholds aligned with tools/building_refine/building_evidence.py.
// Geometry evidence is independent of the model's semantic label.
namespace building_evidence {
using Point = std::array<double,3>;
using Triangle = std::array<Point,3>;
inline double Dot(const Point& a,const Point& b) { return a[0]*b[0]+a[1]*b[1]+a[2]*b[2]; }
struct Result { bool accepted=false; int planes=0; double wallFraction=0; };
inline Result Check(const std::vector<Triangle>& input) {
    struct Face { Triangle p; Point n,c; double area; bool used=false; };
    std::vector<Face> faces; double total=0;
    for(const auto& p:input) {
        bool finite=true; for(const auto& v:p) for(double x:v) finite &= std::isfinite(x);
        if(!finite) continue;
        Point a,b,n,c;
        for(int k=0;k<3;++k) { a[k]=p[1][k]-p[0][k]; b[k]=p[2][k]-p[0][k]; c[k]=(p[0][k]+p[1][k]+p[2][k])/3; }
        n={a[1]*b[2]-a[2]*b[1],a[2]*b[0]-a[0]*b[2],a[0]*b[1]-a[1]*b[0]};
        double length=std::sqrt(Dot(n,n)); if(length<=.02) continue;
        for(double& x:n) x/=length;
        faces.push_back({p,n,c,length/2}); total+=length/2;
    }
    std::vector<size_t> seeds;
    for(size_t i=0;i<faces.size();++i) if(std::abs(faces[i].n[2])<.25) seeds.push_back(i);
    std::stable_sort(seeds.begin(),seeds.end(),[&](size_t a,size_t b){return faces[a].area>faces[b].area;});
    if(seeds.size()>64) seeds.resize(64);
    Result result; double support=0; bool rectangular=false; std::vector<Point> directions;
    for(size_t seed:seeds) {
        if(faces[seed].used) continue;
        Point n=faces[seed].n; n[2]=0; double length=std::sqrt(Dot(n,n)); for(double& x:n) x/=length;
        Point u={-n[1],n[0],0}; double offset=Dot(faces[seed].c,n),area=0;
        double lo=std::numeric_limits<double>::infinity(),hi=-lo,zlo=lo,zhi=-lo;
        std::vector<size_t> hit;
        for(size_t i=0;i<faces.size();++i) {
            const auto& f=faces[i]; if(f.used || std::abs(f.n[2])>=.25 || std::abs(Dot(f.n,n))<=.97) continue;
            bool near=true; for(const auto& p:f.p) near &= std::abs(Dot(p,n)-offset)<1.2;
            if(!near) continue;
            hit.push_back(i); area+=f.area;
            for(const auto& p:f.p) { double x=Dot(p,u); lo=std::min(lo,x);hi=std::max(hi,x);zlo=std::min(zlo,p[2]);zhi=std::max(zhi,p[2]); }
        }
        if(hit.size()<2 || hi-lo<3 || zhi-zlo<3 || area<12 || area<.55*(hi-lo)*(zhi-zlo)) continue;
        for(size_t i:hit) faces[i].used=true;
        support+=area; ++result.planes;
        directions.push_back(n); rectangular |= area>=.85*(hi-lo)*(zhi-zlo) && area>=.60*total;
    }
    result.wallFraction=total>0?support/total:0;
    bool corner=false; for(const auto& a:directions) for(const auto& b:directions) corner |= std::abs(Dot(a,b))<.9;
    result.accepted=(corner && result.wallFraction>=(result.planes>=3?.20:.50)) || rectangular;
    return result;
}
}
