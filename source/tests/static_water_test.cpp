// Exercise the short-buffer fallback against Valve's real DSP with a split,
// failure-injectable PCM store. The production class only uses these methods.
#include "../underwater.h"
#include <cassert>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <memory>
#include <vector>

using DWORD=uint32_t;
constexpr DWORD DSBLOCK_ENTIREBUFFER=2,WAVE_FORMAT_PCM=1;
constexpr bool FAILED(int hr){return hr<0;}
constexpr bool SUCCEEDED(int hr){return hr>=0;}
struct DSBCAPS {DWORD dwSize{},dwBufferBytes{};};
struct WAVEFORMATEX {unsigned nChannels{},wFormatTag{},wBitsPerSample{},nSamplesPerSec{};};
namespace {
unsigned locks{},renderCalls{};
IPLAudioEffectState IPLCALL checkedApply(IPLDirectEffect effect,IPLDirectEffectParams* p,IPLAudioBuffer* input,IPLAudioBuffer* output){
    assert(locks==0);++renderCalls;return iplDirectEffectApply(effect,p,input,output);
}
}
struct IDirectSoundBuffer {
    std::vector<short> pcm;
    unsigned refs=1,lockCalls{};
    bool failNext{};
    explicit IDirectSoundBuffer(size_t n):pcm(n){}
    void AddRef(){++refs;}
    void Release(){assert(refs>1);--refs;}
    int GetCaps(DSBCAPS* c){c->dwBufferBytes=DWORD(pcm.size()*2);return 0;}
    int GetFormat(WAVEFORMATEX* f,DWORD,void*){*f={1,1,16,48000};return 0;}
    int Lock(DWORD,DWORD,void** a,DWORD* na,void** b,DWORD* nb,DWORD){
        ++lockCalls;
        if(failNext){failNext=false;return -1;}
        assert(locks==0);++locks;
        auto split=pcm.size()/3;*a=pcm.data();*b=pcm.data()+split;
        *na=DWORD(2*split);*nb=DWORD(2*(pcm.size()-split));return 0;
    }
    int Unlock(void*,DWORD,void*,DWORD){assert(locks==1);--locks;return 0;}
};
namespace directfx {bool ready{};immersion::DirectApi api;}
#include "../static_water.h"

int main(){
    IPLContextSettings cfg{};cfg.version=STEAMAUDIO_VERSION;
    IPLContext context{};assert(iplContextCreate(&cfg,&context)==IPL_STATUS_SUCCESS);
    directfx::api={context,iplDirectEffectCreate,checkedApply,iplDirectEffectReset,iplDirectEffectRelease};
    directfx::ready=true;
    IDirectSoundBuffer buffer(4157);
    for(size_t i=0;i<buffer.pcm.size();i++)buffer.pcm[i]=short(15000.*std::sin(6.28318530718*i/6.));
    const auto dry=buffer.pcm;
    const immersion::Settings off{},under{1,0,420,0,0};
    auto partial=under;partial.wet=.375f;
    StaticWater state;
    state.update(&buffer,1,off);assert(!buffer.lockCalls);
    buffer.failNext=true;state.update(&buffer,1,under);
    assert(buffer.pcm==dry && buffer.refs==1 && !renderCalls);
    state.update(&buffer,1,under);const auto dark=buffer.pcm;
    assert(buffer.refs==2 && dark!=dry && renderCalls);
    auto count=renderCalls;
    state.update(&buffer,1,partial);
    for(size_t i=0;i<dry.size();i++)assert(buffer.pcm[i]==(5*dry[i]+3*dark[i])/8);
    assert(renderCalls==count);
    unsigned already=buffer.lockCalls;state.update(&buffer,1,partial);assert(buffer.lockCalls==already);
    buffer.failNext=true;state.update(&buffer,1,off);assert(buffer.refs==2);
    state.update(&buffer,1,off);assert(buffer.refs==1 && buffer.pcm==dry);
    state.update(&buffer,1,under);
    auto higher=under;higher.cutoffHz=2300;
    count=renderCalls;state.update(&buffer,1,higher);
    assert(renderCalls>count && buffer.pcm!=dark && buffer.pcm!=dry);
    // GTA reused the physical channel and filled it with a different sample.
    std::fill(buffer.pcm.begin(),buffer.pcm.end(),1200);const auto other=buffer.pcm;
    state.update(&buffer,2,under);state.update(&buffer,2,off);
    assert(buffer.pcm==other && buffer.refs==1 && !locks);
    // Restart restores a surviving native buffer and leaves a rebound bank alone.
    state.update(&buffer,2,under);assert(state.active());
    buffer.failNext=true;state.restore(&buffer,2);assert(state.active());
    state.restore(&buffer,2);assert(!state.active() && buffer.pcm==other);
    state.update(&buffer,2,under);
    std::fill(buffer.pcm.begin(),buffer.pcm.end(),700);const auto rebound=buffer.pcm;
    state.restore(&buffer,3);assert(!state.active() && buffer.pcm==rebound);
    iplContextRelease(&context);
    std::puts("Split PCM locks, exact restoration, retry, bank reuse and cached partial immersion passed.");
}
