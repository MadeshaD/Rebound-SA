#pragma once
#include <algorithm>
#include <cmath>

namespace injury {
struct State {
    float amount{};
    float fade{1.f};
    bool bypass{true};
};

inline State evaluate(float hp,float maximum,int threshold,int strength,bool enabled,
                      bool excluded,bool dead,float screenAlpha) {
    if(!enabled || !strength || excluded || !std::isfinite(hp) ||
       !std::isfinite(maximum) || maximum<=0.f)return {};
    const float limit=maximum*std::clamp(threshold,10,50)*.01f;
    if(!dead && hp>limit)return {0.f,1.f,false};
    const float depth=dead?1.f:std::clamp((limit-hp+1.f)/(limit+1.f),0.f,1.f);
    const float fade=dead && std::isfinite(screenAlpha)?1.f-std::clamp(screenAlpha/255.f,0.f,1.f):1.f;
    return {depth*std::clamp(strength,0,100)*.01f,fade,false};
}

struct Life {
    bool dying{},returning{};
    State update(bool playerPresent,float hp,float maxHp,bool wasted,float alpha,
                 int threshold,int strength,bool enabled,bool excluded){
        if(!enabled || strength==0){dying=returning=false;return {};}
        if(excluded)return {};
        if(!std::isfinite(alpha))alpha=0.f;
        if(playerPresent && !wasted && (!std::isfinite(hp) || !std::isfinite(maxHp) || maxHp<=0))return {};
        const bool dead=(playerPresent && hp<=0.f) || wasted;
        if(dead){dying=true;returning=false;}
        if(dying && playerPresent && !dead){dying=false;returning=alpha>0.f;}
        if(dying)return evaluate(0,100,threshold,strength,true,false,true,alpha);
        if(!playerPresent){returning=false;return {};}
        State out=evaluate(hp,maxHp,threshold,strength,true,false,false,alpha);
        if(returning){out.fade=1.f-std::clamp(alpha/255.f,0.f,1.f);returning=alpha>0.f;}
        return out;
    }
};

inline float pole(float hz,float rate){return -std::expm1(-6.283185307f*hz/rate);}
struct Tone {
    float first{},second{},cut{1},mix{},mixGoal{},cutGoal{1},slew{},configuredRate{};
    bool initialized{};
    void prepare(float amount,float rate,bool immediate=false){
        const float target=std::clamp(amount,0.f,1.f);
        if(initialized && !immediate && mixGoal==target && configuredRate==rate)return;
        configuredRate=rate;mixGoal=target;
        const float hz=std::min(16000.f,.43f*rate)*std::pow(1.f/40.f,mixGoal);
        cutGoal=pole(hz,rate);
        slew=pole(10.f,rate);
        if(immediate || !initialized){mix=mixGoal;cut=cutGoal;initialized=true;}
    }
    float sample(float x){
        mix+=slew*(mixGoal-mix);
        cut+=slew*(cutGoal-cut);
        first+=cut*(x-first);second+=cut*(first-second);
        // Retain a little attack during the shallow end of the transition.
        const float blend=std::min(1.f,mix*3.f);
        return x+blend*(second-x);
    }
};

// One oscillator for the listener. Its phase and gain are continuous across blocks.
struct Ring {
    double phase{};
    float gain{};
    void clear(){phase=0;gain=0;}
    float tick(float amount,float master,float rate){
        const float goal=.010f*amount*amount*master;
        gain+=std::min(1.f,1.f/(.035f*rate))*(goal-gain);
        phase+=6.283185307179586*3180./rate;
        if(phase>=6.283185307179586)phase-=6.283185307179586;
        return gain*float(std::sin(phase));
    }
};

}
