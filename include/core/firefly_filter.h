#pragma once
#include <algorithm>
#include <array>
#include <cmath>
#include <vector>

// Reconstruction of isolated INDIRECT outliers, not a transport/sample clamp.
// Broad caustic peaks have bright neighbours and pass through. Direct lighting,
// emitters and camera-visible specular chains are kept outside this filter.
namespace dusk_fireflies {
inline float luminance(const float* rgb) { return .2126f*rgb[0]+.7152f*rgb[1]+.0722f*rgb[2]; }
inline float limit(std::array<float,24> neighbours,int count,float value) {
    if(count<3) return INFINITY;
    std::sort(neighbours.begin(),neighbours.begin()+count);
    float supported=neighbours[(count*3)/4];
    float reference=std::max(supported,4*neighbours[count/2]);
    return value>4*reference+.05f ? supported : INFINITY;
}
inline void reconstruct(std::vector<float>& image,const std::vector<float>& indirect,int width,int height,float strength) {
    if(strength<=0 || indirect.size()!=image.size()) return;
    strength=std::clamp(strength,0.f,1.f);
    for(int y=0;y<height;++y) for(int x=0;x<width;++x) {
        size_t p=(size_t(y)*width+x)*3;
        float value=luminance(&indirect[p]);
        if(value<=.05f) continue;
        std::array<float,24> neighbours{}; int count=0;
        for(int dy=-2;dy<=2;++dy) for(int dx=-2;dx<=2;++dx) {
            if((dx==0 && dy==0) || x+dx<0 || x+dx>=width || y+dy<0 || y+dy>=height) continue;
            neighbours[count++]=luminance(&indirect[(size_t(y+dy)*width+x+dx)*3]);
        }
        float ceiling=limit(neighbours,count,value);
        if(value>ceiling) {
            float removed=strength*(1-ceiling/value);
            for(int c=0;c<3;++c) image[p+c]=std::max(0.f,image[p+c]-indirect[p+c]*removed);
        }
    }
}
}
