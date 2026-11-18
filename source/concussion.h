#pragma once
#include "low_health.h"
#include <cstdint>

namespace injury {
struct Concussion {
    float radius=18.f, strength=1.f, fullDamage=50.f, recovery=8.f, ringing=2.5f;
    bool enabled=true;
    float peak{};
    uint32_t onset{};
    uintptr_t owner{};
    float value(uint32_t now)const{
        const float seconds=float(now-onset)*.001f;
        return std::max(0.f,peak-seconds/recovery);
    }
    void clear(){peak=0;owner=0;}
    void hit(uintptr_t ped,uint32_t now,float loss,float metres){
        if(!enabled || !ped || !(loss>0.f) || !(metres>=0.f && metres<radius))return;
        const float proximity=1.f-metres/radius;
        const float hit=std::sqrt(std::min(1.f,loss/fullDamage))*proximity;
        const float previous=owner==ped?value(now):0.f;
        peak=std::max(previous,hit);onset=now;owner=ped;
    }
    State mix(State base,float intensity)const{
        if(base.bypass || intensity<=0.f)return base;
        const float amount=intensity*strength;
        base.amount=std::max(base.amount,amount);
        base.blastRing=amount*amount*ringing;
        return base;
    }
};
}
