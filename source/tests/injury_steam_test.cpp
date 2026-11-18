#include "../injury_steam.h"
#include <cassert>
#include <cstdio>
#include <cstdlib>
#include <vector>

// Runs against Valve's actual native SDK. No DirectSound or game is required.
struct RealTail {
    IPLContext context{};
    IPLReflectionEffect effect{};
    explicit RealTail(int rate){
        IPLContextSettings contextSettings{};contextSettings.version=STEAMAUDIO_VERSION;
        IPLAudioSettings audio{rate,injury::sdkFrames};
        IPLReflectionEffectSettings reflection{IPL_REFLECTIONEFFECTTYPE_PARAMETRIC,0,1};
        if(iplContextCreate(&contextSettings,&context)!=IPL_STATUS_SUCCESS ||
           iplReflectionEffectCreate(context,&audio,&reflection,&effect)!=IPL_STATUS_SUCCESS)std::abort();
    }
    void apply(float* in,float* out,injury::Decay d){injury::applySteamBlock(effect,iplReflectionEffectApply,in,out,d);}
    void clear(){iplReflectionEffectReset(effect);}
    ~RealTail(){iplReflectionEffectRelease(&effect);iplContextRelease(&context);}
};
constexpr double tau=6.283185307179586;
struct Result {double earlyDb,lateDb,t40;};
inline float burst(unsigned i,unsigned rate){
    double t=double(i)/rate;
    return float(.13*std::sin(tau*125*t)+.065*std::sin(tau*212*t)+.035*std::sin(tau*367*t));
}
inline Result measure(const std::vector<float>& output,unsigned rate){
    auto db=[&](double from,double to){
        unsigned a=unsigned(from*rate),b=unsigned(to*rate);
        double energy=0;for(unsigned i=a;i<b;i++)energy+=double(output[i])*output[i];
        return 10.*std::log10(std::max(1.e-20,energy/(b-a)));
    };
    double total=0;for(float x:output){assert(std::isfinite(x));total+=double(x)*x;}
    assert(total>1e-8);
    unsigned crossing=0;double remaining=0;
    for(unsigned i=unsigned(output.size());i--;){
        remaining+=double(output[i])*output[i];
        if(remaining>=1.e-4*total){crossing=i;break;}
    }
    return {db(.05,.35),db(.7,1.5),double(crossing)/rate};
}
Result probe(unsigned rate,float amount,bool checkSpeech=false){
    RealTail sdk(rate);
    injury::Program program;program.configure(rate);
    injury::State state{amount,1,false};
    const unsigned warm=rate/3,duration=rate/4,end=warm+duration;
    std::vector<float> output(rate*8);
    for(unsigned i=0;i<end+output.size();i++){
        if(i%173==0)program.prepare(state,2,sdk);
        float x=i>=warm && i<end?burst(i-warm,rate):0;
        float mix[2]={x,x},cj[2]{};
        if(checkSpeech){cj[0]=cj[1]=x;mix[0]=mix[1]=0;}
        program.sample(mix,cj,2,-1,state,0,sdk);
        if(i>=end)output[i-end]=mix[0];
    }
    return measure(output,rate);
}
int main(){
    for(unsigned rate:{32000u,44100u,48000u,96000u}){
        auto a=probe(rate,.25f),b=probe(rate,.6f),c=probe(rate,1);
        for(auto t:{a,b,c})std::printf("%u Hz: early %.2f dBFS, late %.2f dBFS, T-40 %.3f s\n",rate,t.earlyDb,t.lateDb,t.t40);
        assert(c.earlyDb>a.earlyDb+6 && c.lateDb>b.lateDb && b.lateDb>a.lateDb+3 && c.t40>b.t40 && b.t40>a.t40);
        assert(c.t40>1.5 && c.lateDb>-75);
    }
    const auto world=probe(48000,.9375f),cj=probe(48000,.9375f,true);
    assert(cj.lateDb<world.lateDb-6 && cj.lateDb>world.lateDb-20 && cj.t40>1.8);
    std::printf("1 HP late tail: ordinary %.2f dBFS, CJ %.2f dBFS\n",world.lateDb,cj.lateDb);
    return 0;
}
