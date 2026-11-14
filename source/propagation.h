#pragma once
#include <algorithm>
#include <cmath>
inline float radialPitch(float listenerTowardSource,float sourceAway){
    constexpr float c=343.f;
    if(!std::isfinite(listenerTowardSource+sourceAway) ||
       std::max(std::abs(listenerTowardSource),std::abs(sourceAway))>200.f)return 1.f;
    return (c+listenerTowardSource)/(c+sourceAway);
}
inline float wallBand(float transmission,float thickness,float lowStrength,int band){
    // The SDK value is the surface reference. Only add loss for a measured slab.
    float base=std::clamp(transmission,.0001f,1.f);
    float slope=band==0?.12f:(band==1?.28f:.55f);
    float exponent=(band==0?lowStrength:1.f)*(1.f+slope*thickness);
    return std::pow(base,exponent);
}
