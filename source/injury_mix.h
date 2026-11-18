#pragma once
#include <array>
#include "low_health.h"

namespace injury {
constexpr unsigned sdkFrames=128;
struct Decay {float low,mid,high;};
inline Decay decay(float depth){
    return {1.15f+4.05f*depth,.55f+2.05f*depth,.18f+.42f*depth};
}

// Reduce only the ordinary programme when it would run into a full-scale
// protected line. The soft knee stays outside normal listening levels.
inline float combine(float ordinary,float protectedVoice){
    const float room=std::max(0.f,1.f-std::copysign(1.f,ordinary)*protectedVoice);
    const float knee=.12f*room;
    const float over=std::abs(ordinary)-(room-knee);
    if(over>0.f){
        float limited=room-knee+(knee>0.f?over/(1.f+over/knee):0.f);
        ordinary=std::copysign(limited,ordinary);
    }
    return ordinary+protectedVoice;
}

// One instance belongs to the game output. Input already contains all audible
// spatial effects. Player speech has a gentler tone curve and a reduced send to this same tail.
class Program {
    unsigned rate{},cursor{},tailFrames{};
    Tone tone[8], speechTone[8];
    Ring ring;
    std::array<float,sdkFrames> input{},returned{};
    float depth{},fade=1,follow{},dc{},body{},body2{},tailLow{},tailHigh{},env{},modeX{},modeY{};
    float hp{},lp{},tailPole{},highPole{},rotX{},rotY{},attack{},release{};
    bool active{},blockExcited{};
public:
    void configure(unsigned hz){
        rate=hz;follow=pole(6.f,hz);hp=pole(34.f,hz);lp=pole(790.f,hz);
        tailPole=pole(530.f,hz);highPole=pole(45.f,hz);
        const float angle=6.283185307f*102.f/hz,damping=std::exp(-1.f/(.085f*hz));
        rotX=damping*std::cos(angle);rotY=damping*std::sin(angle);
        attack=pole(95.f,hz);release=pole(4.f,hz);
    }
    // A recognized CJ line feeds the same listener reverb at a lower level.
    static constexpr float speechDepth=.45f,speechSend=.28f;
    bool running()const{return active;}
    template<class Reverb> void reset(Reverb& sdk){
        if(active)sdk.clear();
        active=blockExcited=false;cursor=tailFrames=0;depth=0;fade=1;ring.clear();
        dc=body=body2=tailLow=tailHigh=env=modeX=modeY=0;
        input.fill(0);returned.fill(0);for(auto& t:tone)t={};for(auto& t:speechTone)t={};
    }
    template<class Reverb> void prepare(State s,unsigned channels,Reverb& sdk){
        if(s.bypass || s.fade<=0){if(active)reset(sdk);fade=s.fade;return;}
        if(!active && !s.amount && s.fade==1.f)return;
        if(!active){
            fade=s.fade;
            for(unsigned c=0;c<channels;c++){tone[c].prepare(0,rate,true);speechTone[c].prepare(0,rate,true);}
        }
        active=true;
        for(unsigned c=0;c<channels;c++){tone[c].prepare(s.amount,rate);speechTone[c].prepare(s.amount*speechDepth,rate);}
        if(!s.amount && s.fade==1 && depth<.0001f && ring.gain<.000001f)reset(sdk);
    }
    template<class Reverb> void sample(float* game,const float* speech,unsigned channels,int lfe,
                                      State state,float master,Reverb& sdk){
        if(!active || state.bypass || state.fade<=0){
            float gain=state.bypass?1.f:state.fade;
            for(unsigned c=0;c<channels;c++)game[c]=(game[c]+speech[c])*gain;
            return;
        }
        depth+=follow*(state.amount-depth);
        fade+=std::min(1.f,3.f*follow)*(state.fade-fade);
        float mid=0,sum=0,peak=0;
        for(unsigned c=0;c<channels;c++){
            float weight=int(c)==lfe?.22f:(c==0?1.f:c==1?.82f:.65f);
            float send=game[c]+speechSend*speech[c];
            mid+=weight*send;sum+=weight;peak=std::max(peak,std::abs(send));
        }
        mid/=sum;
        dc+=hp*(mid-dc);body+=lp*(mid-dc-body);body2+=lp*(body-body2);
        float delta=(peak-env)*(peak>env?attack:release);env+=delta;
        float next=rotX*modeX-rotY*modeY+.7f*delta;
        modeY=rotY*modeX+rotX*modeY;modeX=next;
        // A changing programme envelope excites the low mode, including short HUD beeps.
        float drive=(body2+.55f*modeX)*std::sqrt(std::max(0.f,depth));
        input[cursor]=drive;
        blockExcited|=std::abs(drive)>1.e-7f;
        float wet=returned[cursor];
        if(++cursor==sdkFrames){
            // An empty injury bus stays digitally silent even when the SDK uses
            // a noise floor internally. Once excited, render the complete tail.
            if(blockExcited || tailFrames){
                auto times=decay(depth);
                if(blockExcited)tailFrames=unsigned((.5f+2.f*times.low)*rate);
                sdk.apply(input.data(),returned.data(),times);
                tailFrames-=std::min(tailFrames,sdkFrames);
                if(!tailFrames)sdk.clear();
            }else returned.fill(0);
            cursor=0;blockExcited=false;
        }
        tailLow+=tailPole*(wet-tailLow);tailHigh+=highPole*(tailLow-tailHigh);
        float raw=(tailLow-tailHigh)*(1.45f+2.6f*depth)+.24f*drive;
        // Reserve headroom for loud transients, without limiting the normal game mix.
        float rumble=raw/(1.f+std::abs(raw)/.52f);
        if(state.amount==0.f)rumble*=std::min(1.f,depth*12.f);
        float ears=ring.tick(depth,master,float(rate));
        for(unsigned c=0;c<channels;c++){
            float dry=tone[c].sample(game[c]);
            float extra=int(c)==lfe?.7f*rumble:(c<2?rumble+ears:.54f*rumble);
            const float scene=dry*(1.f-.14f*depth)+extra;
            const float voice=speechTone[c].sample(speech[c]);
            game[c]=fade*(depth>.001f?combine(scene,voice):scene+voice);
        }
    }
};
}
