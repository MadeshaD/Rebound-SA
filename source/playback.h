#pragma once
#include <algorithm>
#include <cmath>

// Time-align the looping reflection renderer to the hardware play cursor.
// Compare circular differences so crossing either end of a loop is continuous.
inline double phaseDifference(double target,double cursor,double length){
    return std::remainder(target-cursor,length);
}

inline float fieldDistanceGain(float distance,float radius){
    float fade=std::clamp((radius-distance)/(.30f*radius),0.f,1.f);
    fade*=fade*(3.f-2.f*fade);
    // Recover some of the dry channel's distance attenuation on long ranges.
    float reach=std::clamp((radius-80.f)/240.f,0.f,1.5f);
    return fade*(1.f+reach*std::clamp((distance-16.f)/100.f,0.f,1.f));
}
