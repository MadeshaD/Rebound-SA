#pragma once
#include "phonon.h"
#include <algorithm>
#include <array>
#include <atomic>
#include <cmath>
#include <type_traits>

namespace immersion {
inline std::atomic<float> listener{},source[64]{};
inline std::atomic<int> cutoff{420},loss{},modulation{12};
inline std::atomic<bool> drowning[64]{};

struct Settings {
    float wet{}, shared{};
    int cutoffHz{420}, lossDb{}, modulationPercent{12};
};
inline Settings settings(float wet,float shared=0.f){
    return {std::clamp(wet,0.f,1.f),std::clamp(shared,0.f,1.f),cutoff.load(),loss.load(),modulation.load()};
}
inline Settings channel(int slot){
    float ear=listener.load(),emitter=source[slot].load();
    return settings(std::max(ear,emitter)*(drowning[slot].load()?.25f:1.f),std::min(ear,emitter));
}
inline Settings stream(float ear,bool nativeUnderwaterAmbience){
    return settings(nativeUnderwaterAmbience?0.f:ear);
}
struct DirectApi {
    IPLContext context{};
    decltype(&iplDirectEffectCreate) create{};
    decltype(&iplDirectEffectApply) apply{};
    decltype(&iplDirectEffectReset) reset{};
    decltype(&iplDirectEffectRelease) release{};
};
inline constexpr unsigned block=64;

// Valve's direct effect has three transmission bands. Evaluate the requested
// low-pass curve in those bands. Its gain and the LFO belong to this medium,
// independently of the geometry occlusion and air absorption of a world voice.
inline IPLDirectEffectParams transmission(const Settings& s,float phase){
    const float cutoffHz=s.cutoffHz*(1.f+.5f*s.shared)*(1.f+.01f*s.modulationPercent*std::sin(phase));
    IPLDirectEffectParams p{};
    p.flags=static_cast<IPLDirectEffectFlags>(IPL_DIRECTEFFECTFLAGS_APPLYOCCLUSION|
             IPL_DIRECTEFFECTFLAGS_APPLYTRANSMISSION|IPL_DIRECTEFFECTFLAGS_APPLYDISTANCEATTENUATION);
    p.occlusion=0.f;
    p.transmissionType=IPL_TRANSMISSIONTYPE_FREQDEPENDENT;
    p.distanceAttenuation=std::pow(10.f,-.05f*s.lossDb*(1.f-.35f*s.shared));
    constexpr float centres[]={180.f,1500.f,6800.f};
    for(unsigned i=0;i<3;i++){
        float ratio=centres[i]/cutoffHz;
        float square=ratio*ratio;
        p.transmission[i]=1.f/std::sqrt(1.f+square*square);
    }
    return p;
}

class Transmission {
    const DirectApi* api{};
    // Separate mono effects keep independent filter histories for L/R.
    IPLDirectEffect effect[2][2]{};
    unsigned rate{},channels{},factor{};
    float mix{},phase{},response{},previous[2]{};
    bool fresh{true},used{};
    static constexpr unsigned maxFactor=4;
public:
    Transmission()=default;
    Transmission(const Transmission&)=delete;
    Transmission& operator=(const Transmission&)=delete;
    ~Transmission(){close();}
    bool valid()const{return effect[0][0]!=nullptr;}
    bool needsAudio(float wet)const{return valid() && (wet>0.f || used);}
    void noteDry(){fresh=false;}
    void close(){
        if(api)for(auto& pair:effect)for(auto& e:pair)if(e)api->release(&e);
        api=nullptr;rate=channels=factor=0;used=false;fresh=true;
    }
    bool open(const DirectApi& bindings,unsigned hz,unsigned count){
        if(valid() && rate==hz && channels==count)return true;
        close();
        if(!bindings.context || !bindings.create || !bindings.apply || !bindings.reset || !bindings.release ||
           hz<8000 || hz>192000 || count<1 || count>2)return false;
        api=&bindings;rate=hz;channels=count;
        factor=hz<24000?(32000+hz-1)/hz:1;
        IPLAudioSettings audio{int(rate*factor),int(block*factor)};
        IPLDirectEffectSettings format{1};
        for(unsigned c=0;c<count;c++){
            for(auto& stage:effect[c])
                if(api->create(api->context,&audio,&format,&stage)!=IPL_STATUS_SUCCESS){close();return false;}
        }
        response=-std::expm1(-1.f/(.025f*rate));clear();return true;
    }
    void clear(){
        if(used && api)for(auto& pair:effect)for(auto e:pair)if(e)api->reset(e);
        mix=phase=0;previous[0]=previous[1]=0;fresh=true;used=false;
    }
    // Exactly 64 complete frames in normalized floating point. The caller may
    // supply interleaved stereo. A dry block is bit-transparent on return.
    void process(float* samples,const Settings& s){
        if(!valid())return;
        if(fresh){mix=s.wet;fresh=false;}
        if(s.wet==0.f && mix<.0001f){
            if(used)clear();
            fresh=false;
            for(unsigned c=0;c<channels;c++)previous[c]=samples[(block-1)*channels+c];
            return;
        }
        auto p=transmission(s,phase);
        // A second, gentler SDK stage deepens the treble cut. Apply overall
        // attenuation once. Separate work buffers obey the SDK aliasing contract.
        auto second=p;second.distanceAttenuation=1.f;
        for(auto& gain:second.transmission)gain=std::sqrt(gain);
        const float speed=6.283185307f*.72f*block/rate;
        phase+=speed;if(phase>=6.283185307f)phase-=6.283185307f;
        float blend[block];
        for(auto& v:blend){mix+=response*(s.wet-mix);v=mix;}
        float input[block*maxFactor],middle[block*maxFactor],output[block*maxFactor];
        float* in=input;float* mid=middle;float* out=output;
        IPLAudioBuffer src{1,int(block*factor),&in},internal{1,int(block*factor),&mid},dst{1,int(block*factor),&out};
        for(unsigned c=0;c<channels;c++){
            for(unsigned i=0;i<block;i++){
                float x=samples[i*channels+c];
                if(factor==1)input[i]=x;
                else for(unsigned j=0;j<factor;j++)
                    input[i*factor+j]=previous[c]+(x-previous[c])*float(j+1)/factor;
                previous[c]=x;
            }
            // The SDK ramps EQ coefficients on a new effect. Prime on silence
            // so a station, a gunshot or a short cue begins at its requested tone.
            if(!used){
                float zeros[block*maxFactor]{},*zero=zeros;
                IPLAudioBuffer silent{1,int(block*factor),&zero};
                api->apply(effect[c][0],&p,&silent,&internal);
                api->apply(effect[c][1],&second,&silent,&dst);
            }
            api->apply(effect[c][0],&p,&src,&internal);
            api->apply(effect[c][1],&second,&internal,&dst);
            for(unsigned i=0;i<block;i++){
                auto& x=samples[i*channels+c];
                const float wet=output[(i+1)*factor-1];
                if(std::isfinite(wet))x+=blend[i]*(wet-x);
            }
        }
        used=true;
    }
};

// DirectSound supplies arbitrary packet sizes. Keep one fixed block of latency
// in both wet and dry playback so a dive never changes pitch or stereo timing.
class Stream {
    Transmission filter;
    std::array<float,block*2> in{},out{};
    unsigned cursor{};
    bool lastAmbient{};
public:
    bool open(const DirectApi& api,unsigned rate){return filter.open(api,rate,2);}
    void clear(){filter.clear();in.fill(0);out.fill(0);cursor=0;}
    template<class Sample> void process(Sample* data,unsigned frames,float wet,bool ambient,bool silence=false){
        if(ambient!=lastAmbient){clear();lastAmbient=ambient;}
        const auto controls=stream(wet,ambient);
        for(unsigned i=0;i<frames;i++){
            for(unsigned c=0;c<2;c++){
                const unsigned k=2*cursor+c;
                float x=silence?0.f:float(data[2*i+c]);
                if constexpr(std::is_integral_v<Sample>)x/=32768.f;
                in[k]=x;
                if constexpr(std::is_integral_v<Sample>)data[2*i+c]=static_cast<Sample>(std::clamp(std::lrint(out[k]*32768.f),-32768L,32767L));
                else data[2*i+c]=out[k];
            }
            if(++cursor==block){
                out=in;
                if(!ambient)filter.process(out.data(),controls);
                cursor=0;
            }
        }
    }
};
}
