#include "../injury_mix.h"
#include <cassert>
#include <cstdio>

// Isolate the early delay. The existing Steam Audio integration test measures
// the late tail separately using the real SDK.
struct SilentTail {
    void apply(float*,float* out,injury::Decay){std::fill_n(out,injury::sdkFrames,0.f);}
    void clear(){}
};
double earlyEnergy(float amount,bool cj){
    constexpr unsigned rate=48000, start=rate/2;
    injury::Program mix;mix.configure(rate);
    SilentTail tail;
    injury::State state{amount,1,false};
    double energy=0;
    for(unsigned i=0;i<start+rate/6;i++){
        if(i%128==0)mix.prepare(state,2,tail);
        const float burst=(i>=start && i<start+480)?float(.18*std::sin((i-start)*.12)):0.f;
        float game[]={cj?0.f:burst,cj?0.f:burst},voice[]={cj?burst:0.f,cj?burst:0.f};
        mix.sample(game,voice,2,-1,state,0,tail);
        if(i>start+rate/40)energy+=game[0]*game[0]+game[1]*game[1];
    }
    return energy;
}
int main(){
    const double scene=earlyEnergy(1,false),cj=earlyEnergy(1,true);
    const double ratio=10*std::log10(cj/scene);
    assert(scene>earlyEnergy(.25f,false)*9 && cj>0 && ratio<-9 && ratio>-13);
    assert(earlyEnergy(0,false)==0);
    std::printf("CJ early delay relative to world: %.2f dB\n",ratio);
}
