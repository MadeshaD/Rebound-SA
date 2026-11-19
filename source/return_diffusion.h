#pragma once
#include <array>
#include <cmath>

// These short, fixed Schroeder allpasses spread an attack in time. Each has
// unit magnitude at every frequency. Use identical coefficients on L and R so
// a single reflection keeps its bearing. The environmental buses run at 32 kHz.
template<unsigned Delay> class ReturnAllpass {
    std::array<float,Delay> history{};
    unsigned cursor{};
public:
    float sample(float input,float gain){
        const float output=history[cursor]+gain*input;
        float feedback=input-gain*output;
        history[cursor]=std::abs(feedback)>1.e-20f?feedback:0.f;
        if(++cursor==Delay)cursor=0;
        return output;
    }
};

struct ReturnDiffusion {
    ReturnAllpass<47> first;
    ReturnAllpass<101> second;
    float echo(float x){return second.sample(first.sample(x,.71f),.74f);}
};
