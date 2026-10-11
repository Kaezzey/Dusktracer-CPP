#pragma once
#include "vec3.h"
#include <array>
#include <algorithm>
#include <cmath>

// A homogeneous spherical interface admits a one-dimensional specular manifold.
// phi(theta)=theta-2(asin(k*sin(theta))-asin(k*sin(theta)/eta)).
// Split at its two possible folds to find every transmitted branch, rather than
// using a single Newton seed that misses the other side of the caustic.
namespace sun_sphere {
struct lens { point3 center; double radius=1, eta=1.5; };
struct map {
    double k,eta,limit;
    map(double ratio,double ior):k(ratio),eta(ior),limit(std::asin(1/ratio)*(1-1e-7)) {}
    double angle(double theta) const {
        double b=std::clamp(k*std::sin(theta),-1.0,1.0);
        return theta-2*(std::asin(b)-std::asin(b/eta));
    }
    double derivative(double theta) const {
        double b=k*std::sin(theta);
        return 1-2*k*std::cos(theta)*(1/std::sqrt(std::max(1e-15,1-b*b))-
            1/(eta*std::sqrt(std::max(1e-15,1-b*b/(eta*eta)))));
    }
    double jacobian(double theta) const {
        double a=derivative(theta);
        return std::abs(theta)<1e-7 ? a*a : std::abs(std::sin(angle(theta))/std::sin(theta)*a);
    }
    double transmission(double theta) const {
        double b=k*std::sin(theta), ci=std::sqrt(std::max(0.0,1-b*b)),ct=std::sqrt(std::max(0.0,1-b*b/(eta*eta)));
        double rp=(ci-eta*ct)/(ci+eta*ct),rs=(eta*ci-ct)/(eta*ci+ct);
        double t=1-.5*(rp*rp+rs*rs);
        return t*t;
    }
    int roots(double target,std::array<double,3>& result) const {
        double split=0;
        if(derivative(0)>0 && derivative(limit)<0) {
            double lo=0,hi=limit;
            for(int i=0;i<28;++i) { double mid=(lo+hi)*.5; if(derivative(mid)>0) lo=mid; else hi=mid; }
            split=(lo+hi)*.5;
        }
        double ends[4]={-limit,-split,split,limit};
        int count=0;
        for(int segment=0;segment<3;++segment) {
            double lo=ends[segment],hi=ends[segment+1],a=angle(lo)-target,b=angle(hi)-target;
            if(hi-lo<1e-12 || a*b>0) continue;
            for(int i=0;i<28;++i) { double mid=(lo+hi)*.5,c=angle(mid)-target; if((a<0)==(c<0)) {lo=mid;a=c;} else hi=mid; }
            double theta=(lo+hi)*.5;
            if(!count || std::abs(theta-result[count-1])>1e-7) result[count++]=theta;
        }
        return count;
    }
};
struct connection { vec3 direction,inside,outgoing; point3 entry,exit; double jacobian,transmission; };
inline connection connect(const lens& sphere,const point3& origin,const vec3& axial,const vec3& tangent,double theta) {
    map mapping((sphere.center-origin).length()/sphere.radius,sphere.eta);
    connection c;
    c.direction=std::cos(theta)*axial+std::sin(theta)*tangent;
    vec3 delta=sphere.center-origin;
    double along=dot(delta,c.direction);
    double t=along-std::sqrt(std::max(0.0,sphere.radius*sphere.radius-delta.length_squared()+along*along));
    c.entry=origin+t*c.direction;
    vec3 normal=(c.entry-sphere.center)/sphere.radius;
    c.inside=refract(c.direction,normal,1/sphere.eta);
    c.exit=c.entry-2*dot(c.entry-sphere.center,c.inside)*c.inside;
    c.outgoing=refract(c.inside,-(c.exit-sphere.center)/sphere.radius,sphere.eta);
    c.jacobian=mapping.jacobian(theta); c.transmission=mapping.transmission(theta);
    return c;
}
}
