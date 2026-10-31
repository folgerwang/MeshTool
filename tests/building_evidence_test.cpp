#include "building_evidence.h"
#include <iostream>
#include <fstream>
#include <cmath>
using namespace building_evidence;
int main(int argc,char** argv) {
    if(argc==2) { // Replay captured triangles for native/Python parity checks.
        std::ifstream file(argv[1],std::ios::binary);
        std::vector<Triangle> p; Triangle t;
        while(file.read(reinterpret_cast<char*>(t.data()),sizeof(t))) p.push_back(t);
        auto r=Check(p);std::cout<<r.accepted<<" "<<r.planes<<" "<<r.wallFraction<<" "<<r.coherentWall<<" "<<r.coherentRoof<<"\n";return 0;
    }
    std::vector<Triangle> wall={Triangle{Point{0,0,0},Point{12,0,0},Point{12,0,20}},
                               Triangle{Point{0,0,0},Point{12,0,20},Point{0,0,20}}};
    if(!Check(wall).accepted) return 1;
    if(Check({wall[0]}).accepted) return 2;
    auto ground=wall;for(auto& t:ground)for(auto& p:t) std::swap(p[1],p[2]);
    if(Check(ground).accepted) return 3;
    if(Check({}).accepted) return 4;
    auto slope=ground;for(auto& t:slope)for(auto& p:t)p[2]=p[0]*.8;
    if(Check(slope).accepted) return 5;
    // A foliage-like cone of large facets is not a coherent wall.
    std::vector<Triangle> cone;
    for(int i=0;i<16;++i) {
        const double a=i*3.14159265358979/8,b=(i+1)*3.14159265358979/8;
        cone.push_back(Triangle{Point{0,0,18},Point{5*std::cos(a),5*std::sin(a),0},Point{5*std::cos(b),5*std::sin(b),0}});
    }
    if(Check(cone).accepted) return 6;
    // Seen from above: a smooth raised roof on low walls is a building; the
    // same roof without walls (a sloped ground plane) is not.
    std::vector<Triangle> shed={Triangle{Point{0,0,0},Point{6,0,0},Point{6,0,2.5}},Triangle{Point{0,0,0},Point{6,0,2.5},Point{0,0,2.5}}};
    Birdview roof{120,90};
    if(!Check(shed,&roof).accepted) return 7;
    if(Check(slope,&roof).accepted) return 8;
    Birdview crown{120,10};
    if(Check(shed,&crown).accepted) return 9;
    // A flat raised block is smooth; a rough crown of the same height is not.
    const int W=40,H=40;std::vector<float> top(W*H),above(W*H);
    for(int y=0;y<H;++y)for(int x=0;x<W;++x){
        const bool block=x<20;
        top[y*W+x]=block?10.f:10.f+float(((x*7919+y*104729)%97)/97.0*1.5);
        above[y*W+x]=8.f;
    }
    auto smooth=SmoothRaised(top,above,W,H,.5);
    if(!smooth[20*W+10] || smooth[20*W+30]) return 10;
    std::cout<<"Building evidence checks passed\n";
}
