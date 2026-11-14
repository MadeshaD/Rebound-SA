#pragma once
#include <atomic>
#include <algorithm>
#include <cmath>
namespace immersion {
inline std::atomic<float> listener{},source[64]{};
inline std::atomic<int> cutoff{420},loss{0},modulation{12};
inline std::atomic<bool> drowning[64]{};
inline float amount(int slot){
    float ear=listener.load(),src=slot>=0?source[slot].load():0.f;
    return std::max(ear,src)*(slot>=0 && drowning[slot]?.25f:1.f);
}
inline float sharedWater(int slot){
    return slot>=0?std::min(listener.load(),source[slot].load()):0.f;
}
struct Filter {
    float lo{},lo2{},phase{},pole{},gain{1},mix{},target{},step{},phaseStep{},base{},depth{};
    unsigned segment{};
    void prepare(float wet,float rate,float shared=0.f){
        target=wet;step=-std::expm1(-1.f/(.025f*rate));
        phaseStep=6.2831853f*.72f/rate;base=6.2831853f*cutoff.load()*(1.f+.5f*shared)/rate;
        depth=.01f*modulation.load();
        gain=std::pow(10.f,-.05f*loss.load()*(1.f-.35f*shared));segment=0;
    }
    float sample(float x){
        mix+=step*(target-mix);
        if(mix<.00001f && target==0){lo=lo2=x;return x;}
        if(!segment){
            pole=-std::expm1(-base*(1+depth*std::sin(phase)));
            phase=std::fmod(phase+32*phaseStep,6.2831853f);segment=32;
        }
        --segment;lo+=pole*(x-lo);lo2+=pole*(lo-lo2);
        return x+mix*(gain*lo2-x);
    }
};
}
