#pragma once
#include <algorithm>
#include <cmath>
inline float radialPitch(float listenerTowardSource,float sourceAway){
    constexpr float c=343.f;
    if(!std::isfinite(listenerTowardSource+sourceAway) ||
       std::max(std::abs(listenerTowardSource),std::abs(sourceAway))>200.f)return 1.f;
    return (c+listenerTowardSource)/(c+sourceAway);
}
