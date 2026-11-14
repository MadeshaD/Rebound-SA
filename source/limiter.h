#pragma once
#include <algorithm>
#include <array>
#include <cmath>

// One block of lookahead. Both stereo channels share the same gain.
// Reserve headroom for the dry signal, which the game mixes independently.
template<int Frames> struct WetLimiter {
    using Block=std::array<float,Frames*2>;
    Block pending{};
    float previousLimit=1, gain=1;
    bool process(const Block& next,short* out) {
        float peak=0.25f;
        for (float value:next) peak=std::max(peak,std::abs(value));
        float nextLimit=0.25f/peak;
        float goal=std::min(previousLimit,nextLimit);
        if (goal>gain) goal=gain+(goal-gain)*0.045f;
        float start=gain;
        for (int i=0;i<Frames;i++) {
            gain=start+(goal-start)*float(i+1)/Frames;
            for (int ch=0;ch<2;ch++) out[2*i+ch]=short(pending[2*i+ch]*gain*32767.f);
        }
        previousLimit=nextLimit;
        pending=next;
        return nextLimit<1.f;
    }
};
