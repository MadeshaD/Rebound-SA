#pragma once
#include <algorithm>
#include <cmath>

// Steam Audio blends visible and transmitted pressure. Add a verified detour
// to the blocked fraction before the SDK processes the original PCM stream.
inline float blockedTransfer(float material,float detour,int band){
    constexpr float edge[]={1.f,.92f,.67f};
    return std::min(1.f,std::hypot(material,detour*edge[band]));
}
inline float returnGain(float listenerRoof,float emitterRoof,float distance,float boostDb,float strength,float access){
    float mismatch=std::clamp(listenerRoof*(1.f-emitterRoof),0.f,1.f);
    float distanceMix=std::clamp((distance-12.f)/88.f,0.f,1.f);
    // A roof over the listener cannot make a far outdoor source excite the
    // tunnel as strongly as an emitter inside it.
    float coupling=1.f/(1.f+mismatch*(1.5f+.10f*distance));
    // A nearby visible source can excite the listener room through its entrance.
    float proximity=std::clamp((65.f-distance)/50.f,0.f,1.f);
    float entrance=mismatch*access*proximity*proximity*(3.f-2.f*proximity);
    float db=boostDb*distanceMix*std::min(1.f,strength);
    float outdoorGain=std::pow(10.f,db/20.f);
    // The listener tunnel already has its own early and late gain.
    float indoorGain=.55f;
    float base=coupling*((1.f-listenerRoof)*outdoorGain+listenerRoof*indoorGain);
    return base+entrance*std::max(0.f,.85f-base);
}

// Bounded amplitude estimate for a nearby reflecting surface.
inline float surfaceReturn(float metres,float range,float absorption){
    if(metres>=range-.05f)return 0.f;
    float reflectivity=std::sqrt(1.f-std::clamp(absorption,0.f,1.f));
    return reflectivity*(1.f-metres/range)/(1.f+metres/5.f);
}
inline float tunnelWeight(float wall,float ceiling,float emitterCover,float listenerCover){
    return std::clamp(emitterCover*listenerCover*(.28f+.58f*wall+.14f*ceiling),0.f,1.f);
}

// Restore part of the game's distance loss on the diffuse return only.
inline float tunnelRecoveryDb(float distance,float connection,float boost,float emittedDb,float voiceDb){
    float distanceMix=std::clamp((distance-12.f)/88.f,0.f,1.f);
    float loss=std::max(0.f,std::min(0.f,emittedDb)-voiceDb);
    return connection*distanceMix*std::min(boost,.65f*loss);
}
