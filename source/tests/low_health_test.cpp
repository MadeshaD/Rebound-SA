#include "../injury_mix.h"
#include "../stem_queue.h"
#include "../mix_pcm.h"
#include <cassert>
#include <cstdio>

using injury::State;
static constexpr unsigned rate=48000;
static constexpr double tau=6.283185307179586;

static void healthTransitions(){
    auto at=[](float hp,int max=100,int threshold=15,int strength=100){
        return injury::evaluate(hp,float(max),threshold,strength,true,false,false,0);
    };
    assert(at(16).amount==0 && at(15).amount>0 && at(0).amount==1);
    float previous=0;
    for(int hp=30;hp>=0;--hp){
        float depth=at(float(hp),200).amount;
        assert(depth>previous);previous=depth;
    }
    assert(at(31,200).amount==0 && at(50,100,50).amount>0);
    assert(at(11,100,5).amount==0 && at(51,100,70).amount==0);
    assert(at(4,100,15,0).bypass && at(0,100,15,50).amount==.5f);
    assert(injury::evaluate(NAN,100,15,100,true,false,false,0).bypass);
    injury::Life life;
    auto frame=[&](float hp,bool dead,float alpha,bool excluded=false,bool present=true){
        return life.update(present,hp,100,dead,alpha,15,100,true,excluded);};
    assert(frame(6,false,220).fade==1);
    assert(frame(4,false,0,true).bypass);
    assert(frame(1,true,0).amount==1);
    assert(std::abs(frame(0,true,128).fade-127.f/255)<1e-5f);
    assert(frame(0,false,255,false,false).fade==0);
    auto respawn=frame(100,false,255);assert(!respawn.amount && !respawn.fade);
    assert(frame(100,false,128).fade>0 && frame(100,false,0).fade==1);
    assert(!frame(100,false,150).amount && frame(100,false,150).fade==1);
}

// A deterministic delay gives this routing test a known additive output.
// The companion integration test exercises the actual Steam Audio effect.
struct TestTail {
    std::array<float,injury::sdkFrames> last{};
    void clear(){last.fill(0);}
    void apply(float* input,float* output,injury::Decay){
        for(unsigned i=0;i<last.size();i++){output[i]=last[i];last[i]=input[i]+.6f*last[i];}
    }
};
static void routing(){
    injury::Program a,b;a.configure(rate);b.configure(rate);
    TestTail ta,tb;
    for(unsigned i=0;i<rate/2;i++){
        State state{.9f,1.f,false};
        if(i%143==0){a.prepare(state,2,ta);b.prepare(state,2,tb);}
        float native=float(.05*std::sin(tau*180*i/rate)),world=float(.12*std::sin(tau*1800*i/rate));
        float sound[]={native+world,native-world},other[]={sound[0],sound[1]};
        float cj[]={float(.14*std::cos(tau*6000*i/rate)),float(.16*std::sin(tau*2000*i/rate))},none[2]{};
        a.sample(sound,cj,2,-1,state,0,ta);b.sample(other,none,2,-1,state,0,tb);
        for(int c=0;c<2;c++)assert(std::abs((sound[c]-other[c])-cj[c])<1.e-6f);
    }
    for(auto s:{State{},State{1,0,false},State{0,.45f,false},State{0,1,false}}){
        a.prepare(s,2,ta);
        float x[]={.1f,-.25f},cj[]={.5f,.25f};
        a.sample(x,cj,2,-1,s,0,ta);
        if(s.bypass||s.fade==1.f)assert(std::abs(x[0]-.6f)<1.e-6f);
        if(s.fade==0)assert(!x[0] && !x[1]);
        if(s.fade>0 && s.fade<1)assert(x[0]<=.6f*s.fade+.001f);
        a.reset(ta);
    }
    injury::State critical{1,1,false};
    a.prepare(critical,2,ta);
    for(unsigned i=0;i<rate/4;i++){
        float x[2]{},cj[]={i==0?.75f:0.f,-.13f};
        a.sample(x,cj,2,-1,critical,0,ta);
        assert(x[0]==cj[0] && x[1]==cj[1]);
    }
    injury::Tone clean,half,deep;
    clean.prepare(0,rate,true);half.prepare(.5f,rate,true);deep.prepare(1,rate,true);
    double normal=0,middle=0,low=0;
    for(unsigned i=0;i<rate;i++){
        float x=float(std::sin(tau*8000*i/rate));
        float y=clean.sample(x),z=half.sample(x),w=deep.sample(x);
        if(i>rate/2){normal+=y*y;middle+=z*z;low+=w*w;}
    }
    assert(middle<normal*.2 && low<middle*.02);
    for(float speech:{-1.f,-.8f,0.f,.8f,1.f}){
        assert(injury::combine(0,speech)==speech);
        for(int sample=-400;sample<=400;sample++){
            float sum=injury::combine(sample*.01f,speech);
            assert(std::isfinite(sum) && std::abs(sum)<=1.000001f);
        }
    }
    injury::Ring ring;
    for(unsigned i=0;i<rate;i++)assert(ring.tick(1,0,rate)==0);
    for(unsigned i=0;i<rate;i++)ring.tick(.5f,1,rate);
    float soft=ring.gain;
    for(unsigned i=0;i<rate;i++)ring.tick(1,1,rate);
    assert(ring.gain>3.9f*soft && ring.gain<=.01001f);
}
static void stemClock(){
    StemQueue q;
    Stems source[256],v{};
    for(unsigned block=0;block<150;block++){
        for(unsigned i=0;i<256;i++){
            float n=float(block*256+i);
            source[i]={{n,n+1},{-n,-n-1}};
        }
        assert(q.push(source,256));
        for(unsigned i=0;i<(block==0?510:512);i++){
            assert(q.pop(.5,v));
            float expected=float(block*256)+i*.5f-(block?1.f:0.f);
            assert(v.world[0]==expected && v.player[0]==-expected);
        }
    }
    q.discard();assert(!q.pop(1.,v) && q.size()==0);
    assert(q.push(source,256));
    for(unsigned i=0;i<64;i++)assert(q.pop(4.,v) && v.world[0]==source[4*i].world[0]);
    assert(!q.pop(4.,v));
}
static void sampleFormats(){
    for(unsigned bits:{8u,16u,24u,32u}){
        for(bool floating:{false,true}){
            if(floating && bits!=32)continue;
            mixpcm::Format f{48000,2,bits,floating};
            for(float x:{-1.f,-.5f,0.f,.25f,.999f}){
                unsigned char p[4]{};mixpcm::write(p,f,x);
                assert(std::abs(x-mixpcm::read(p,f))< 1.01f/float(1u<<(bits-1))+1e-7f);
            }
        }
    }
    mixpcm::Format padded{48000,6,32,false,3,24};
    unsigned char p[4]{};mixpcm::write(p,padded,.12345678f);
    assert(!p[0] && std::abs(mixpcm::read(p,padded)-.12345678f)<2.e-7f);
}
int main(){healthTransitions();routing();stemClock();sampleFormats();std::puts("Health, routing, format and stem clock checks passed.");}
