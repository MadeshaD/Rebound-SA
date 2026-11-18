#include "../underwater.h"
#include <cassert>
#include <cstdint>
#include <cstdio>
#include <vector>

namespace {
constexpr double tau=6.283185307179586;
constexpr unsigned rate=48000;
constexpr unsigned block=immersion::block;
using Stereo=std::array<float,block*2>;

float sine(unsigned sample,unsigned hz,double frequency){
    return float(.24*std::sin(tau*frequency*sample/hz));
}

double response(const immersion::DirectApi& api,unsigned hz,double freq,immersion::Settings s){
    immersion::Transmission filter;
    assert(filter.open(api,hz,2));
    const unsigned count=hz/block;
    double dry=0,processed=0,leak=0;
    for(unsigned k=0;k<count;k++){
        Stereo frames{};
        for(unsigned j=0;j<block;j++){
            const float value=sine(k*block+j,hz,freq);
            frames[2*j]=value;
            if(k>count/2)dry+=value*value;
        }
        filter.process(frames.data(),s);
        for(unsigned j=0;j<block;j++){
            assert(std::isfinite(frames[2*j]) && std::isfinite(frames[2*j+1]));
            if(k>count/2){
                processed+=frames[2*j]*frames[2*j];
                leak+=frames[2*j+1]*frames[2*j+1];
            }
        }
    }
    assert(leak/dry<1e-10);
    return 10.*std::log10(processed/dry);
}

void musicAndAmbience(const immersion::DirectApi& api){
    immersion::Stream stream,chunked;
    assert(stream.open(api,rate) && chunked.open(api,rate));
    const unsigned count=6601;
    std::vector<float> music(2*count);
    for(unsigned i=0;i<count;i++){music[2*i]=sine(i,rate,8000);music[2*i+1]=sine(i,rate,125);}
    auto one=music,many=music;
    stream.process(one.data(),count,1,false);
    constexpr unsigned sizes[]={1,17,64,72,511,39,192};
    for(unsigned i=0,part=0;i<count;part++){
        unsigned n=std::min(sizes[part%7],count-i);
        chunked.process(many.data()+2*i,n,1,false);i+=n;
    }
    for(unsigned i=0;i<2*count;i++)assert(std::abs(many[i]-one[i])<2e-6f);
    for(unsigned i=0;i<2*block;i++)assert(one[i]==0);
    double dry=0,wet=0;
    for(unsigned i=2*block;i<count;i++){wet+=one[2*i]*one[2*i];dry+=music[2*(i-block)]*music[2*(i-block)];}
    assert(wet<.004*dry);
    // The one exempt track starts clean, even after part of a wet packet.
    std::vector<int16_t> ambient(2*count);
    for(unsigned i=0;i<ambient.size();i++)ambient[i]=int16_t(int(i%601)-300);
    auto original=ambient;
    stream.process(ambient.data(),count,1,true);
    for(unsigned i=0;i<2*count;i++)assert(ambient[i]==(i<2*block?0:original[i-2*block]));
    auto switched=music;
    stream.process(switched.data(),count,1,false);
    double firstWet=0,firstDry=0;
    for(unsigned i=0;i<block;i++){
        assert(switched[2*i]==0);firstWet+=switched[2*(i+block)]*switched[2*(i+block)];
        firstDry+=music[2*i]*music[2*i];
    }
    assert(firstWet<.025*firstDry);
    stream.clear();
    auto dryMusic=original;
    stream.process(dryMusic.data(),count,0,false);
    for(unsigned i=0;i<2*count;i++)assert(dryMusic[i]==(i<2*block?0:original[i-2*block]));
    stream.clear();
    std::vector<float> silent(2*count,1.f);
    stream.process(silent.data(),count,0,false,true);
    for(float v:silent)assert(v==0.f);
}

void transitions(const immersion::DirectApi& api){
    immersion::Transmission steady,modulated;
    assert(steady.open(api,rate,1) && modulated.open(api,rate,1));
    std::array<float,block> normal{},waving{};
    double difference=0,energy=0;
    for(unsigned k=0;k<1500;k++){
        for(unsigned i=0;i<block;i++)normal[i]=sine(k*block+i,rate,1200);
        waving=normal;
        steady.process(normal.data(),{1,0,420,0,0});
        modulated.process(waving.data(),{1,0,420,0,30});
        if(k>200)for(unsigned i=0;i<block;i++){
            difference+=std::pow(waving[i]-normal[i],2);energy+=normal[i]*normal[i];
        }
    }
    assert(difference>.002*energy);
    // Surfacing returns the exact native samples after the short crossfade.
    for(unsigned k=0;k<250;k++){
        for(unsigned i=0;i<block;i++)normal[i]=sine(k*block+i,rate,1200);
        auto original=normal;steady.process(normal.data(),{0,0,420,0,0});
        if(k==249)assert(normal==original);
    }
    assert(!steady.needsAudio(0));
}

void sources(){
    immersion::listener=0;immersion::source[7]=.8f;
    auto s=immersion::channel(7);assert(s.wet==.8f && s.shared==0);
    immersion::listener=1;s=immersion::channel(7);assert(s.wet==1 && s.shared==.8f);
    immersion::drowning[7]=true;assert(immersion::channel(7).wet==.25f);
    assert(immersion::stream(1,true).wet==0 && immersion::stream(1,false).wet==1);
}
}

int main(){
    IPLContext context{};IPLContextSettings settings{};settings.version=STEAMAUDIO_VERSION;
    assert(iplContextCreate(&settings,&context)==IPL_STATUS_SUCCESS);
    immersion::DirectApi sdk{context,iplDirectEffectCreate,iplDirectEffectApply,iplDirectEffectReset,iplDirectEffectRelease};
    const immersion::Settings water{1,0,420,0,0};
    for(unsigned hz:{8000u,11025u,16000u,22050u,24000u,32000u,44100u,48000u,96000u,192000u}){
        double bass=response(sdk,hz,125,water),mid=response(sdk,hz,2000,water);
        assert(bass>-7 && bass<=0 && mid<bass-12);
    }
    double bass=response(sdk,rate,125,water),treble=response(sdk,rate,8000,water);
    assert(treble<bass-30);
    auto loss=water;loss.lossDb=12;
    assert(std::abs(response(sdk,rate,125,loss)-bass+12)<.3);
    assert(std::abs(response(sdk,rate,8000,loss)-treble+12)<.3);
    auto brighter=water;brighter.cutoffHz=1800;
    assert(response(sdk,rate,2000,brighter)>response(sdk,rate,2000,water)+9);
    auto softer=water;softer.wet=.25f;assert(response(sdk,rate,8000,softer)>treble+12);
    sources();musicAndAmbience(sdk);transitions(sdk);
    iplContextRelease(&context);
    std::printf("Water at 48 kHz, cutoff 420 Hz: 125 Hz %.2f dB, 8 kHz %.2f dB. Stereo, track changes, formats, modulation and surfacing passed.\n",bass,treble);
}
