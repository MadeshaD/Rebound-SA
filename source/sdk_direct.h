#pragma once
#include <algorithm>
#include <cmath>
#include "phonon.h"

inline float sdkGain(float value){
    return std::isfinite(value)?std::clamp(value,0.f,1.f):1.f;
}

inline IPLDirectEffectParams sdkDirectParams(float visibility,const float transmission[3],
                                              const float air[3],unsigned rate){
    IPLDirectEffectParams params{};
    params.occlusion=sdkGain(visibility);
    int flags=IPL_DIRECTEFFECTFLAGS_APPLYOCCLUSION|IPL_DIRECTEFFECTFLAGS_APPLYTRANSMISSION;
    params.transmissionType=IPL_TRANSMISSIONTYPE_FREQDEPENDENT;
    for(int band=0;band<3;band++){
        params.transmission[band]=sdkGain(transmission[band]);
        params.airAbsorption[band]=sdkGain(air[band]);
    }
    // Keep the SDK equalizer out of low-rate legacy voice buffers. The
    // SDK still mixes visibility and wall transmission on these voices.
    if(rate<24000){
        params.transmissionType=IPL_TRANSMISSIONTYPE_FREQINDEPENDENT;
        params.distanceAttenuation=(params.airAbsorption[0]+params.airAbsorption[1]+params.airAbsorption[2])/3.f;
        flags|=IPL_DIRECTEFFECTFLAGS_APPLYDISTANCEATTENUATION;
    }else{
        flags|=IPL_DIRECTEFFECTFLAGS_APPLYAIRABSORPTION;
    }
    params.flags=static_cast<IPLDirectEffectFlags>(flags);
    return params;
}
