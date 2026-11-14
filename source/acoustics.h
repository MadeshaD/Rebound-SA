#pragma once
#include <algorithm>
#include <cmath>

struct Vec {float x,y,z;};
inline float magnitude(Vec v) { return std::sqrt(v.x*v.x+v.y*v.y+v.z*v.z); }

// Walk a broken ray via the edge of a hit. Cross the hit plane between two offset points to handle thin collision faces.
template<class Trace>
float obstruction(Vec a,Vec b,Trace trace) {
    auto direction=[](Vec a, Vec b){return Vec{b.x-a.x,b.y-a.y,b.z-a.z};};
    Vec d=direction(a,b);
    float len=magnitude(d);
    if (len<1.5f) return 0;
    Vec dir{d.x/len,d.y/len,d.z/len};
    float hit=trace(a,dir,len);
    if (hit>=len-0.30f) return 0;
    Vec centre{a.x+dir.x*hit,a.y+dir.y*hit,a.z+dir.z*hit};
    float horizontal=std::hypot(dir.x,dir.y);
    Vec side=horizontal>.01f?Vec{-dir.y/horizontal,dir.x/horizontal,0}:Vec{1,0,0};
    auto clear=[&](Vec from,Vec to){
        Vec step=direction(from,to);float dist=magnitude(step);
        if (dist<.01f) return true;
        return trace(from,{step.x/dist,step.y/dist,step.z/dist},dist)>=dist-.05f;
    };
    auto around=[&](float sideways,float upwards){
        Vec bend{centre.x+side.x*sideways,centre.y+side.y*sideways,centre.z+upwards};
        float back=std::min(.7f,hit*.6f),ahead=std::min(.7f,(len-hit)*.6f);
        Vec entry{bend.x-dir.x*back,bend.y-dir.y*back,bend.z-dir.z*back};
        Vec exit{bend.x+dir.x*ahead,bend.y+dir.y*ahead,bend.z+dir.z*ahead};
        return clear(a,entry) && clear(entry,exit) && clear(exit,b);
    };
    int sides=int(around(-1.65f,0))+int(around(1.65f,0));
    if (sides==2) return 0;
    if (sides==1) return .15f;
    return around(0,1.4f)?.35f:1.f;
}

// High pass, broad -5 dB bell at 550 Hz, and adjustable air-loss low pass.
// Bell coefficients use 32 kHz, Q=0.75, transposed direct form II.
struct ReflectionTone {
    float bass{},s1{},s2{},treble{};
    float tick(float x,float lp) {
        bass+=.02137f*(x-bass);
        x-=bass;
        constexpr float w=6.2831853f*550/32000;
        static const float a=std::pow(10.f,-5.f/40.f), alpha=std::sin(w)/1.5f,
            scale=1/(1+alpha/a),b0=(1+alpha*a)*scale,b1=-2*std::cos(w)*scale,
            b2=(1-alpha*a)*scale,a2=(1-alpha/a)*scale;
        float y=b0*x+s1;
        s1=b1*(x-y)+s2;s2=b2*x-a2*y;
        treble+=lp*(y-treble);
        if (std::abs(treble)<1e-20f) treble=0;
        if (std::abs(s1)<1e-20f) s1=0;
        if (std::abs(s2)<1e-20f) s2=0;
        if (std::abs(bass)<1e-20f) bass=0;
        return treble;
    }
};
