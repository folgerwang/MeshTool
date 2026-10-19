#include "building_evidence.h"
#include <iostream>
#include <fstream>
using namespace building_evidence;
int main(int argc,char** argv) {
    if(argc==2) { // Replay captured triangles for native/Python parity checks.
        std::ifstream file(argv[1],std::ios::binary);
        std::vector<Triangle> p; Triangle t;
        while(file.read(reinterpret_cast<char*>(t.data()),sizeof(t))) p.push_back(t);
        auto r=Check(p);std::cout<<r.accepted<<" "<<r.planes<<" "<<r.wallFraction<<"\n";return 0;
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
    std::cout<<"Building evidence checks passed\n";
}
