#pragma once
#include <algorithm>
#include <cmath>

// Two first-order low-pass responses and a dry path. When the bands
// have equal gain, the output is exactly the scaled input.
class DirectTone {
    float slow=0, fast=0, slowStep=0, fastStep=0;
public:
    void configure(float rate) {
        auto step=[rate](float hz){return -std::expm1(-6.2831853f*hz/rate);};
        slowStep=step(500.f);
        fastStep=step(3500.f);
        clear();
    }
    void clear(){slow=fast=0;}
    float sample(float x,const float gain[3]){
        slow+=slowStep*(x-slow);
        fast+=fastStep*(x-fast);
        return gain[2]*x+(gain[1]-gain[2])*fast+(gain[0]-gain[1])*slow;
    }
};
