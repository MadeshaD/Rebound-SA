#pragma once
#include <algorithm>
#include <cmath>

// Approximate absorption at 250 Hz, 1, 4 and 8 kHz in ordinary air
// (20 C, 50% RH, sea-level pressure; ISO 9613-1).
inline constexpr float airDbPerMetre[]={.00131f,.00466f,.02967f,.10529f};
class AirPath {
    struct Shelf {float t{},x{},y{},a{},b{},c{};bool active{};};
    Shelf sections[3]{};
    float base=1, metres=-1;
public:
    void configure(float rate){
        constexpr float transitions[]={500,2000,5657};
        for(int k=0;k<3;k++){
            auto& s=sections[k];s={};s.active=transitions[k]<rate*.46f;
            if(s.active)s.t=std::tan(3.14159265f*transitions[k]/rate);
        }
        metres=-1;update(0);
    }
    void clear(){for(auto& s:sections)s.x=s.y=0;}
    void update(float distance){
        distance=std::isfinite(distance)?std::clamp(distance,0.f,2000.f):0.f;
        if(std::abs(distance-metres)<.05f)return;
        metres=distance;
        base=std::exp(-.11512925f*distance*airDbPerMetre[0]);
        for(int k=0;k<3;k++){
            auto& s=sections[k];if(!s.active)continue;
            float ratio=std::exp(-.11512925f*distance*(airDbPerMetre[k+1]-airDbPerMetre[k]));
            float t=s.t*std::sqrt(ratio),den=1+t;
            s.a=(ratio+t)/den;s.b=(t-ratio)/den;s.c=(1-t)/den;
        }
    }
    float process(float x){
        x*=base;
        for(auto& s:sections)if(s.active){
            float y=s.a*x+s.b*s.x+s.c*s.y;s.x=x;s.y=y;x=y;
        }
        return x;
    }
};
