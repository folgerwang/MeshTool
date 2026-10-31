#pragma once
#include <array>
#include <vector>
#include <algorithm>
#include <cmath>
#include <cstdint>
#include <limits>
#include <unordered_map>

// Keep thresholds aligned with tools/building_refine/building_evidence.py.
// Geometry evidence is independent of the model's semantic label.
namespace building_evidence {
using Point = std::array<double,3>;
using Triangle = std::array<Point,3>;
inline double Dot(const Point& a,const Point& b) { return a[0]*b[0]+a[1]*b[1]+a[2]*b[2]; }
struct Result {
    bool accepted=false; int planes=0; double wallFraction=0;
    double coherentWall=0, coherentRoof=0, smoothRoof=0;
    const char* route="none";
};

// The object seen from above (segmenter orthophoto): its footprint and the
// part of it that is a smooth surface standing above the ground model, in m2.
struct Birdview { double footprint=0, smooth=0; };

// Pixels of a smooth raised surface seen from above. A 2.5 m window fits a
// plane to the visible height; flat and pitched roofs fit within 10 cm, tree
// crowns do not. top is non-finite where nothing was captured.
inline std::vector<uint8_t> SmoothRaised(const std::vector<float>& top,const std::vector<float>& aboveGround,
                                         int W,int H,double res) {
    const int r=std::max(1,int(std::lround(1.25/res)));
    std::vector<uint8_t> out(size_t(W)*size_t(H),0);
    for(int y=0;y<H;++y) for(int x=0;x<W;++x) {
        const size_t i=size_t(y)*W+x;
        if(!std::isfinite(top[i]) || aboveGround[i]<=2.5f) continue;
        const int x0=std::max(0,x-r),x1=std::min(W,x+r+1),y0=std::max(0,y-r),y1=std::min(H,y+r+1);
        // Plane-fit moments relative to the centre pixel stay well conditioned.
        double n=0,sx=0,sy=0,sz=0,sxx=0,syy=0,sxy=0,sxz=0,syz=0,szz=0;
        for(int yy=y0;yy<y1;++yy) for(int xx=x0;xx<x1;++xx) {
            const float t=top[size_t(yy)*W+xx]; if(!std::isfinite(t)) continue;
            const double X=xx-x,Y=yy-y,Z=double(t)-top[i];
            n+=1;sx+=X;sy+=Y;sz+=Z;sxx+=X*X;syy+=Y*Y;sxy+=X*Y;sxz+=X*Z;syz+=Y*Z;szz+=Z*Z;
        }
        if(n<.9*(x1-x0)*(y1-y0)) continue;
        const double mx=sx/n,my=sy/n,mz=sz/n;
        const double cxx=sxx/n-mx*mx,cyy=syy/n-my*my,cxy=sxy/n-mx*my;
        const double cxz=sxz/n-mx*mz,cyz=syz/n-my*mz,czz=szz/n-mz*mz;
        const double det=cxx*cyy-cxy*cxy+1e-9,b=(cxz*cyy-cyz*cxy)/det,c=(cyz*cxx-cxz*cxy)/det;
        out[i]=std::sqrt(std::max(0.0,czz-b*cxz-c*cyz))<.10;
    }
    return out;
}

namespace detail {
// Resultant length of unit vectors over each point's 3x3x3 voxel block.
inline std::vector<double> NeighbourhoodResultant(const std::vector<Point>& points,
        const std::vector<Point>& vectors,double size) {
    auto key=[](int64_t x,int64_t y,int64_t z){ return ((x+(1<<20))<<42)|((y+(1<<20))<<21)|(z+(1<<20)); };
    struct Acc { double v[3]={0,0,0}; double n=0; };
    std::unordered_map<int64_t,Acc> cells; cells.reserve(points.size());
    std::vector<std::array<int64_t,3>> cell(points.size());
    for(size_t i=0;i<points.size();++i) {
        for(int k=0;k<3;++k) cell[i][k]=int64_t(std::floor(points[i][k]/size));
        Acc& a=cells[key(cell[i][0],cell[i][1],cell[i][2])];
        for(int k=0;k<3;++k) a.v[k]+=vectors[i][k];
        a.n+=1;
    }
    std::unordered_map<int64_t,double> memo; memo.reserve(cells.size());
    std::vector<double> out(points.size());
    for(size_t i=0;i<points.size();++i) {
        const int64_t own=key(cell[i][0],cell[i][1],cell[i][2]);
        auto hit=memo.find(own);
        if(hit==memo.end()) {
            double v[3]={0,0,0},n=0;
            for(int dx=-1;dx<=1;++dx) for(int dy=-1;dy<=1;++dy) for(int dz=-1;dz<=1;++dz) {
                auto c=cells.find(key(cell[i][0]+dx,cell[i][1]+dy,cell[i][2]+dz)); if(c==cells.end()) continue;
                for(int k=0;k<3;++k) v[k]+=c->second.v[k];
                n+=c->second.n;
            }
            hit=memo.emplace(own,std::sqrt(v[0]*v[0]+v[1]*v[1]+v[2]*v[2])/std::max(n,1e-12)).first;
        }
        out[i]=hit->second;
    }
    return out;
}
}

inline Result Check(const std::vector<Triangle>& input,const Birdview* birdview=nullptr) {
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
    const bool planar=(corner && result.wallFraction>=(result.planes>=3?.20:.50)) || rectangular;

    // Locally coherent walls and roofs: fine, wrinkled capture meshes rarely
    // form one dense plane per wall, but around every surface point a facade
    // or roof keeps one orientation and foliage does not. Points are spread
    // over each triangle (about one per 0.25 m2) so a large triangle covers
    // every voxel it crosses; opposite wall windings agree through the
    // doubled horizontal angle.
    std::vector<Point> wallPts,roofPts,wallVec,roofVec;
    std::vector<double> wallW,roofW; double roofTotal=0,walls=0;
    for(const auto& f:faces) {
        const bool vertical=std::abs(f.n[2])<.25;
        Point up=f.n; if(up[2]<0) for(double& x:up) x=-x;
        if(vertical) walls+=f.area;
        else if(up[2]>=.25) roofTotal+=f.area;
        else continue;
        const int count=int(std::clamp(std::ceil(f.area/.25),1.0,64.0));
        const double share=f.area/count,angle=2*std::atan2(f.n[1],f.n[0]);
        for(int k=0;k<count;++k) {
            double u=std::fmod(k*.7548776662+.5,1.0),v=std::fmod(k*.5698402910+.5,1.0);
            if(u+v>1) { u=1-u; v=1-v; }
            Point q; for(int d=0;d<3;++d) q[d]=f.p[0][d]+u*(f.p[1][d]-f.p[0][d])+v*(f.p[2][d]-f.p[0][d]);
            if(vertical) { wallPts.push_back(q); wallVec.push_back({std::cos(angle),std::sin(angle),0}); wallW.push_back(share); }
            else { roofPts.push_back(q); roofVec.push_back(up); roofW.push_back(share); }
        }
    }
    double height=0;
    if(!wallPts.empty()) {
        auto r=detail::NeighbourhoodResultant(wallPts,wallVec,1.5);
        double lo=std::numeric_limits<double>::infinity(),hi=-lo;
        for(size_t i=0;i<r.size();++i) if(r[i]>.90) { result.coherentWall+=wallW[i]; lo=std::min(lo,wallPts[i][2]); hi=std::max(hi,wallPts[i][2]); }
        if(hi>lo) height=hi-lo;
    }
    if(!roofPts.empty()) {
        auto r=detail::NeighbourhoodResultant(roofPts,roofVec,2.0);
        for(size_t i=0;i<r.size();++i) if(r[i]>.97) result.coherentRoof+=roofW[i];
    }
    const double w=result.coherentWall,rf=result.coherentRoof;
    const bool coherent=(w>=150 && w>=.15*total && height>=6) ||
                        (rf>=100 && rf>=.30*roofTotal && w>=std::max(20.0,.05*rf)) ||
                        (w+rf>=600 && w+rf>=.05*total && w>=.05*(w+rf));
    // Seen from above: a smooth raised roof over enough of the footprint,
    // standing on some vertical surface (a sloped ground plane has none).
    bool roofSeen=false;
    if(birdview) {
        result.smoothRoof=birdview->smooth;
        roofSeen=birdview->smooth>=40 && birdview->smooth>=.20*birdview->footprint && walls>=std::max(10.0,.10*birdview->smooth);
    }
    result.accepted=planar || coherent || roofSeen;
    result.route=planar?"facade planes":coherent?"coherent walls/roofs":roofSeen?"smooth roof from above":"none";
    return result;
}
}
