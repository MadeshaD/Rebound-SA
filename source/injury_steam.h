#pragma once
#include "injury_mix.h"
#include "phonon.h"

namespace injury {
template<class Apply>
inline void applySteamBlock(IPLReflectionEffect effect,Apply apply,float* input,float* output,Decay times){
    IPLReflectionEffectParams params{};
    params.type=IPL_REFLECTIONEFFECTTYPE_PARAMETRIC;
    params.numChannels=1;
    params.reverbTimes[0]=times.low;
    params.reverbTimes[1]=times.mid;
    params.reverbTimes[2]=times.high;
    IPLAudioBuffer src{1,sdkFrames,&input},dst{1,sdkFrames,&output};
    apply(effect,&params,&src,&dst,nullptr);
}
}
