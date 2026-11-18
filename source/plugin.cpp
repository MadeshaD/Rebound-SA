#ifndef _WIN32_WINNT
#define _WIN32_WINNT 0x0A00
#endif
#define WIN32_LEAN_AND_MEAN
#define NOMINMAX
#include <windows.h>
#include <mmsystem.h>
#include <dsound.h>
#include <dshow.h>
#include <mmreg.h>
#include <mediaobj.h>
#include <mediaerr.h>
#include <algorithm>
#include <atomic>
#include <cmath>
#include <cstring>
#include <cstdint>
#include <cwchar>
#include <memory>
#include <iterator>
#include <new>
#include "room.h"
#include "acoustics.h"
#include "space.h"
#include "voice_response.h"
#include "steam_return.h"
#include <string>
#include <cstdio>
#include "limiter.h"
#include "playback.h"

#include "direct_tone.h"
#include "underwater.h"
#include "air_path.h"
#include "concussion.h"

namespace healthAudio {
std::atomic<bool> enabled{true}, cjVoice[64]{};
std::atomic<int> threshold{15},strength{100};
std::atomic<float> master{};
std::atomic<uint64_t> stateWord{};
std::atomic<unsigned> stateVersion{};
std::atomic<DWORD> updated{};
// These are process-lifetime flags in the supported game. Checking them at
// the audio boundary also covers menu sounds before the next gameplay tick.
bool excluded(){
    auto flag=[](uintptr_t addr){return *reinterpret_cast<volatile const BYTE*>(addr)!=0;};
    return flag(0xBA67A4) || flag(0xB7CB48) || flag(0xB7CB49) || flag(0xB5F851);
}
injury::State snapshot(){
    const uint64_t word=stateWord.load();
    if(!word || GetTickCount()-updated.load()>1500 || excluded())return {};
    return {(word&65535)/65535.f,((word>>16)&32767)/32767.f,false,float(word>>32)/16384.f};
}
void publish(injury::State s){
    updated=GetTickCount();
    uint64_t word=s.bypass?0:(uint64_t(s.blastRing*16384.f+.5f)<<32)|0x80000000u|(uint32_t(s.fade*32767.f+.5f)<<16)|uint32_t(s.amount*65535.f+.5f);
    if(bool(stateWord.exchange(word))!=bool(word))++stateVersion;
}
}
#include "direct_dmo.h"
#include "speech_buffer.h"
#include "radio_buffer.h"
#include "static_water.h"
#include "endpoint.h"
#include "speech_playback.h"

extern "C" __declspec(dllexport) HRESULT WINAPI DllGetClassObject(REFCLSID cls,REFIID id,void** out){
    if(cls!=directfx::cls)return CLASS_E_CLASSNOTAVAILABLE;
    static directfx::Factory factory;return factory.QueryInterface(id,out);
}

namespace {
std::atomic<bool> logging{false};
std::atomic<bool> sdkReverbReady{},fieldReady{},hrtfReady{};
std::atomic<unsigned> queueUnderruns{},limitedBlocks{},outputError{};
std::wstring logPath;
bool stadiumActive{};
int currentArea{};
std::atomic<int> sourceRoom[64]{},farReturn[64]{};
struct Hit {Vec point; float pad; Vec normal; char padding[8]; unsigned char material; char end[7];};
static_assert(sizeof(Hit)==44 && sizeof(void*)==4);
template<typename T> T read(uintptr_t addr) {
    T result; memcpy(&result,reinterpret_cast<void*>(addr),sizeof(result)); return result;
}
#include "spatial.h"
#include "propagation.h"
constexpr int frames=256;
#include "early_geometry.h"
#include "field.h"
struct Voice {
    EarlyPaths lastPaths{};
    NearWallDelay nearWall[2]{};
    int wallStretch{};
    IDirectSoundBuffer* buffer{};
    DWORD samples{},bankRate{};
    AirEq airEq;
    float airLow{},envelope{};
    int lastEq=0;
    DWORD nextEq{};
    double cursor{};
    unsigned offPhase{};
    float fieldLevel{},ambientGain{},lastAmbient{};
    bool looping{}, synced{};
    std::array<float,4> lastBus{};
    float gain{};
    int channel=-1;
    float pan{},roomPan{},shade{},roomGain{};
    float bounceCoverage{};
    std::unique_ptr<short[]> pcm;
    DWORD pcmSamples{}, prefix{}, lastPosition{};
    std::unique_ptr<SpeechPlayback> player;
    bool isPlayer{};
    bool intro{};
};
Voice voices[64];
std::array<std::array<float,64>,2> retiredField{};
std::array<std::array<float,4>,2> retiredBus{}; // Protected by voiceLock.
SRWLOCK voiceLock=SRWLOCK_INIT;
std::atomic<int> enclosure{-1};
std::atomic<bool> ready{false};
std::atomic<float> lateralReturn[2]{},lateralRange[2]{{30.f},{30.f}};
std::atomic<DWORD> lastFrame{0};
std::atomic<float> wet{1.f},quietFloorGain{.12589254f},speechMix{.35f},farGainDb{24.f};
std::atomic<bool> speech[64]{},ambientVoice[64]{};
std::atomic<int> wallEq[64]{};
std::wstring iniPath;
int reloadKey=VK_F8;
std::atomic<float> reflectedHigh{1.f};
void (__cdecl *priorFrame)();

using Trace=bool (__cdecl*)(const Vec&,const Vec&,Hit&,void*&,
                           bool,bool,bool,bool,bool,bool,bool,bool);
#include "garage.h"
bool directOcclusion=true;
struct EmitterGeometry {
    uintptr_t owner{};
    Vec position{};
    bool door{};
};
thread_local const EmitterGeometry* tracingEmitter{};
struct TraceScope {
    explicit TraceScope(const EmitterGeometry& source){tracingEmitter=&source;}
    ~TraceScope(){tracingEmitter=nullptr;}
};
bool castRay(Vec a,Vec b,Hit& hit,void*& entity){
    auto cast=reinterpret_cast<Trace>(0x56BA00);
    if(!cast(a,b,hit,entity,true,false,false,true,true,false,false,false))return false;
    if(!tracingEmitter || !entity)return true;
    const auto& source=*tracingEmitter;
    bool self=uintptr_t(entity)==source.owner;
    if(!self && source.door && hit.material==50){
        Vec delta{hit.point.x-source.position.x,hit.point.y-source.position.y,hit.point.z-source.position.z};
        self=magnitude(delta)<2.5f;
    }
    if(!self)return true;
    // Retry this segment without the emitting object. Respect another mod's active exclusion.
    auto& ignored=*reinterpret_cast<void**>(0xB7CD68);
    if(ignored)return true;
    ignored=entity;entity=nullptr;
    bool found=cast(a,b,hit,entity,true,false,false,true,true,false,false,false);
    ignored=nullptr;
    return found;
}
float distance(Vec origin,Vec ray,float range,unsigned char* material=nullptr) {
    Vec end{origin.x+ray.x*range,origin.y+ray.y*range,origin.z+ray.z*range};
    Hit hit{};void* entity=nullptr;
    if (!castRay(origin,end,hit,entity)) return range;
    if (material) *material=hit.material;
    return std::clamp((hit.point.x-origin.x)*ray.x+(hit.point.y-origin.y)*ray.y+(hit.point.z-origin.z)*ray.z,0.f,range);
}
#include "stadium.h"
int stadiumModel=-1;
bool stadiumEntity(uintptr_t entity,int area){
    if(!entity)return false;
    const unsigned model=read<unsigned short>(entity+0x22);
    if(read<unsigned char>(entity+0x2f)!=area || !stadium::matches(model,area))return false;
    stadiumModel=int(model);
    return true;
}
bool inStadium(int area,DWORD now){
    static int previousArea=-1;
    static DWORD checked{},lastHit{};
    static Vec lastPosition{};
    if(!stadium::candidateArea(area)){previousArea=area;lastHit=0;stadiumModel=-1;return false;}
    Vec position{};
    reinterpret_cast<void(__cdecl*)(Vec*,int)>(0x56E010)(&position,-1);
    bool moved=magnitude(difference(position,lastPosition))>30.f;
    if(area!=previousArea || moved){checked=now-250;lastHit=0;stadiumModel=-1;}
    previousArea=area;
    if(now-checked>=250){
        checked=now;lastPosition=position;
        position.z+=1.f;
        for(Vec direction:{Vec{0,0,-1},{0,0,1},{1,0,0},{-1,0,0},{0,1,0},{0,-1,0}}){
            Hit hit{};void* entity{};
            // Classification only: ignore physical props, vehicles and peds.
            // Acoustic traces elsewhere still use the complete supported scene.
            auto trace=reinterpret_cast<Trace>(0x56BA00);
            if(trace(position,advance(position,direction,180.f),hit,entity,
                     true,false,false,false,true,false,false,false) &&
               stadiumEntity(uintptr_t(entity),area)){lastHit=now;break;}
        }
    }
    return lastHit && now-lastHit<1500;
}
Vec listener{};
std::atomic<int> decayMs{450};
float roofWeight=0,listenerSurface=0,listenerCeiling=0;
std::atomic<float> outdoorAudible[64]{};
std::atomic<float> surfaceWeight[64]{},wallPanOffset[64]{},tunnelConnection[64]{};
std::atomic<int> tunnelBoostDb{12};
std::atomic<float> entranceAccess[64]{};
std::atomic<int> emitterRoofPercent[64]{};
bool interiors=false;
float probeRadius=120.f,sourceRadius=400.f;
std::atomic<float> largeRoomAmount{0},corridorAmount{0},enclosedAmount{0};
bool waterEnabled=true;
bool waterGeometry=false;
float depthAt(Vec p){
    float surface{};
    if(!waterGeometry || !std::isfinite(p.x+p.y+p.z) || read<int>(0xB72914)!=0)return 0;
    bool hit=reinterpret_cast<bool(__cdecl*)(float,float,float,float*,bool,Vec*)>(0x6EB690)(p.x,p.y,p.z,&surface,true,nullptr);
    return hit?std::clamp((surface-p.z-.12f)/.6f,0.f,1.f):0.f;
}
constexpr uintptr_t hardware=0xB5F8B8;
bool radioHook{};
uintptr_t playerPed{};
std::wstring pluginDirectory(){return iniPath.substr(0,iniPath.find_last_of(L"/\\")+1);}
HRESULT WINAPI createStreamBuffer(IDirectSound* ds,const DSBUFFERDESC* desc,IDirectSoundBuffer** out,IUnknown* outer){
    HRESULT h=ds->CreateSoundBuffer(desc,out,outer);
    if(SUCCEEDED(h) && out && *out && !outer && desc->lpwfxFormat && desc->lpwfxFormat->nChannels==2){
        directfx::start(pluginDirectory());
        if(directfx::ready)RadioBuffer::wrap(out,desc->lpwfxFormat->nSamplesPerSec);
    }
    return h;
}
std::atomic<DWORD> gameThread{0};

std::atomic<int> coverPercent{},diffusionPercent{},spanMetres{10};

std::atomic<int> materialPercent{};
#include "live_direct.h"
#include "doppler.h"
bool alternativePitch=false;
std::atomic<float> wetEq[2][3];
std::atomic<bool> wallEcho{true},lateReverb{true};
std::atomic<float> cityReturn{},openSky{},streetGain{},streetBoost{1.58f},rangeScale{2.f};
std::atomic<float> speechRangeScale{1.f};
bool isPedSpeech(short bank,unsigned event){
    return (bank>=20 && bank<=25) || event==0x34 || event==0x35;
}
// GTA computes distance attenuation before assigning hardware voices.
void __fastcall calculateWorldVolume(uintptr_t sound,void*){
    auto& distanceScale=*reinterpret_cast<float*>(sound+0x18);
    float original=distanceScale;
    float scale=isPedSpeech(read<short>(sound),read<unsigned>(sound+0xc))
              ? speechRangeScale.load() : rangeScale.load();
    bool extend=!(read<unsigned short>(sound+0x56)&65) && original>0 && std::isfinite(original*scale) && scale!=1.f;
    if(extend)distanceScale=original*scale;
    reinterpret_cast<void(__thiscall*)(uintptr_t)>(0x4efa10)(sound);
    if(extend)distanceScale=original;
}
std::atomic<int> echoStretch{100},echoDelayMs{0},preDelayMs{0},decayPercent{100};
std::atomic<int> lowDecayPercent{100},highDecayPercent{100};
struct SurfacePlane {Hit hit{};void* entity{};};
struct WallReturns {
    Arrival side[2]{};
    void include(Arrival path){
        auto& current=side[path.pan>0.f];
        if(path.gain>current.gain)current=path;
    }
    float peak()const{return std::max(side[0].gain,side[1].gain);}
};
WallReturns firstBounce(Vec source,float direct,Vec right,int& budget,const SurfacePlane* candidates,unsigned count){
    WallReturns best{};
    for(unsigned i=0;i<count;i++){
        const auto& plane=candidates[i];
        if(!plane.entity || budget<2)continue;
        Vec point{};
        if(!bouncePoint(listener,source,plane.hit.point,unit(plane.hit.normal),point))continue;
        Vec incoming=unit(difference(point,listener));
        float leg=magnitude(difference(point,listener));
        float total=leg+magnitude(difference(source,point));
        if(total-direct>140.f)continue;
        // Hit the actual face, not merely its infinite supporting plane.
        Hit face{};void* entity{};--budget;
        if(!castRay(listener,advance(point,incoming,.12f),face,entity) ||
           magnitude(difference(face.point,point))>.09f || std::abs(dot3(unit(face.normal),unit(plane.hit.normal)))<.97f)continue;
        Hit obstruction{};void* blocker{};--budget;
        Vec toward=unit(difference(source,point));
        if(castRay(advance(point,toward,.06f),source,obstruction,blocker))continue;
        float reflectivity=1.f-live::surface(face.material)->absorption[1];
        auto reflected=arrival(total,direct,.78f*reflectivity*(direct+6.f)/(total+6.f),dot3(right,incoming));
        reflected.bearing=incoming;
        best.include(reflected);
    }
    return best;
}
struct NearbySpace { Vec point{}; float strength{},time{},openAir{},covered{}; bool valid{}; };
NearbySpace neighbours[32]{};
std::atomic<float> neighbouringDecay{0.f};
float entranceLeak=1.f;
void scanNeighbour(unsigned index,Vec dir,float reach){
    auto& n=neighbours[index]; n={};
    if(distance(listener,dir,reach)<reach-.05f)return;
    Vec point=advance(listener,dir,reach),side{-dir.y,dir.x,0};
    float h=distance(point,{0,0,1},60);
    float a=distance(point,side,45),b=distance(point,advance({},side,-1),45);
    float ahead=distance(point,dir,55);
    float behind=distance(point,advance({},dir,-1),55);
    float width=a+b;
    float walls=std::clamp((78.f-width)/50.f,0.f,1.f);
    float covered=std::clamp((60.f-h)/12.f,0.f,1.f);
    // Verify that the cover persists along the passage instead of inferring
    // its depth from a horizontal ray escaping through the end of an arch.
    float depth=0;
    for(int sign:{-1,1}){
        float span=sign>0?ahead:behind;
        for(float step:{6.f,18.f}){
            if(span<step)break;
            Vec farther=advance(point,dir,sign*step);
            if(distance(farther,{0,0,1},60)>=59.8f)break;
            depth+=step==6.f?6.f:12.f;
        }
    }
    float corridor=covered*std::clamp(depth/30.f,0.f,1.f)*(.32f+.68f*walls);
    float alley=(1.f-covered)*walls*std::clamp((ahead+behind)/48.f,0.f,1.f);
    n={point,corridor+.47f*alley,.55f+3.8f*corridor+1.0f*alley,
       (1.f-covered)*(1.f-walls),covered,true};
}
int measure() {
    reinterpret_cast<void (__cdecl*)(Vec*,int)>(0x56E010)(&listener,-1);
    listener.z+=0.65f;
    if (!std::isfinite(listener.x+listener.y+listener.z)) return -1;
    float height=distance(listener,{0,0,1},probeRadius);

    constexpr Vec axes[]={{1,0,0},{.92388f,.38268f,0},{.70711f,.70711f,0},
        {.38268f,.92388f,0},{0,1,0},{-.38268f,.92388f,0},{-.70711f,.70711f,0},{-.92388f,.38268f,0}};
    // One isolated ray may hit a pillar. Use the middle of three neighbouring
    // directions, retaining broad walls and reusing the same 16 collision tests.
    float rays[16],rayAbsorption[16];
    float weight=0,absorption=0,midAbsorption=0;
    for (int i=0;i<8;i++) {
        Vec dir=axes[i];
        for (int sign : {1,-1}) {
            unsigned char material=0;
            float range=distance(listener,{sign*dir.x,sign*dir.y,0},probeRadius,&material);
            rays[i+(sign<0?8:0)]=range;
            rayAbsorption[i+(sign<0?8:0)]=live::surface(material)->absorption[1];
            float influence=1-range/probeRadius;
            absorption+=influence*live::surface(material)->absorption[2];
            midAbsorption+=influence*live::surface(material)->absorption[1];
            weight+=influence;
        }
    }
    materialPercent=weight>.001f ? int(100*absorption/weight) : 0;
    auto wallAt=[&](int i) {
        float a=rays[(i+15)%16],b=rays[i],c=rays[(i+1)%16];
        return std::max(std::min(a,b),std::min(std::max(a,b),c));
    };
    float width=2*probeRadius,length=0,nearest=probeRadius,opposite=probeRadius;
    int across=0;
    for (int i=0;i<8;i++) {
        float r=wallAt(i),l=wallAt(i+8);
        if (l+r<width) {width=l+r;across=i;}
        length=std::max(length,rays[i]+rays[i+8]);
        if (std::min(l,r)<nearest) {nearest=std::min(l,r);opposite=std::max(l,r);}
    }
    // Project actual wall bearings onto the listener's stereo axis. Use the
    // median range to keep an isolated pole from becoming a whole side wall.
    Vec right=unit(read<Vec>(0xB6F028+0x974));
    float strength[2]{},sideRange[2]{probeRadius,probeRadius};
    float sideEnergy[2]{},weightedRange[2]{};
    unsigned shortRays=0;
    bool longBothWays=false;
    float boundedSpan=width;
    for(int i=0;i<16;i++){
        int oppositeIndex=(i+8)%16;
        if(rays[i]<14.f)++shortRays;
        longBothWays|=rays[i]>12.f && rays[oppositeIndex]>12.f;
        if(rays[i]<probeRadius-.2f && rays[oppositeIndex]<probeRadius-.2f)
            boundedSpan=std::max(boundedSpan,rays[i]+rays[oppositeIndex]);
        float range=std::max(rays[i],wallAt(i));
        float amplitude=surfaceReturn(range,probeRadius,rayAbsorption[i]);
        float pan=std::clamp(dot3(axes[i%8],right)*(i<8?1.f:-1.f),-1.f,1.f);
        for(int ch=0;ch<2;ch++){
            float candidate=amplitude*std::sqrt(.5f*(1.f+(ch?pan:-pan)));
            strength[ch]=std::max(strength[ch],candidate);
            float weight=candidate*candidate;
            sideEnergy[ch]+=weight;
            weightedRange[ch]+=weight*range;
        }
    }
    for(int ch=0;ch<2;ch++){
        if(sideEnergy[ch]>0.f)sideRange[ch]=weightedRange[ch]/sideEnergy[ch];
        lateralReturn[ch]=strength[ch];
        lateralRange[ch]=sideRange[ch];
    }
    // A ray leaving through a doorway gives a lower bound on free space
    // outside, not the length of a garage surrounded by nearby walls.
    if(height<12.f && !longBothWays){
        float spans[8];
        for(int i=0;i<8;i++)spans[i]=rays[i]+rays[i+8];
        std::sort(spans,spans+8);
        length=std::min(length,1.35f*spans[6]);
    }
    if(height<6.f && shortRays>=13 && !longBothWays)length=std::min(length,boundedSpan);
    int hits=0;
    for (float range:rays) if (range<probeRadius-.2f) ++hits;
    // Probe the same facades above the listener. A low fence should
    // contribute less late energy than a tall wall at the same distance.
    float facadeHeight=0;
    float streetBody=0.f;
    if(height>=probeRadius-.2f && width<100.f){
        streetBody=streetFacades(listener,axes,rays,across,width,probeRadius,facadeHeight,
                                 [](Vec origin,Vec dir,float reach){return distance(origin,dir,reach);});
    }
    static Vec lastScan{};
    static unsigned phase=0;
    if(magnitude(difference(listener,lastScan))>12.f)std::fill_n(neighbours,32,NearbySpace{});
    lastScan=listener;
    constexpr float reaches[]={5,12,24,40};
    for(unsigned j=0;j<8;j++){
        unsigned k=phase+4*j, bearing=k%8;
        Vec direction=axes[(2*bearing)%8];
        if(bearing>=4)direction=advance({},direction,-1);
        scanNeighbour(k,direction,reaches[k/8]);
    }
    phase=(phase+1)%4;
    float opening=0,roofSum=0,count=0,previewTime=0;
    for(const auto& n:neighbours)if(n.valid){
        float range=magnitude(difference(n.point,listener));
        if(range>48.f)continue;
        float nearness=1.f/(1.f+.07f*range);
        opening=std::max(opening,n.openAir*nearness);
        previewTime=std::max(previewTime,n.time*n.strength*std::min(1.f,1.7f*nearness));
        if(range<27.f){roofSum+=n.covered*nearness;count+=nearness;}
    }
    float continuity=count>0?roofSum/count:1.f;
    entranceLeak=1.f-opening*(.48f+.32f*(1.f-continuity));
    neighbouringDecay=previewTime;
    unsigned pairs=0;
    for(int i=0;i<8;++i)if(rays[i]<probeRadius-.2f && rays[i+8]<probeRadius-.2f)++pairs;
    bool sky=height>=probeRadius-.2f;
    openSky=sky?1.f:0.f;
    // Require a broad vertical facade above head height, and space across the street.
    float urban=0.f;
    if(sky && width>14.f){
        int best=0;
        for(int i=1;i<16;i++)if(wallAt(i)<wallAt(best))best=i;
        float base=wallAt(best);
        if(base<45.f){
            Vec elevated=listener;elevated.z+=5.f;
            Vec dir=axes[best%8];if(best>=8)dir=advance({},dir,-1);
            float upper=distance(elevated,dir,probeRadius);
            urban=std::clamp(1.f-std::abs(upper-base)/(3.f+.2f*base),0.f,1.f)
                 *std::clamp((width-14.f)/12.f,0.f,1.f)*std::clamp((45.f-base)/15.f,0.f,1.f);
        }
    }
    streetGain=std::max(urban,streetBody);
    cityReturn=streetBody;
    float hall=largeEnclosure(height,probeRadius,width,length,pairs/8.f);
    largeRoomAmount=hall;
    float closed=std::clamp((probeRadius-height)/(.2f*probeRadius),0.f,1.f)
                 *std::min(1.f,pairs/4.f);
    auto space=assessSpace(height,width,length,nearest,hits/16.f,facadeHeight,hall,closed,
                           weight>.001f?midAbsorption/weight:.14f);
    if(closed>0){
        float retain=1.f-closed*(1.f-entranceLeak);
        space.returnLevel*=retain;space.diffusion*=retain;space.tunnel*=retain;
        space.decay=.35f+(space.decay-.35f)*retain;
        space.corridor*=retain;
    }
    space.diffusion+=.32f*streetBody;
    space.returnLevel=std::min(1.f,space.returnLevel+.28f*streetBody);
    space.decay+=.85f*streetBody;
    corridorAmount=space.corridor;
    enclosedAmount=space.enclosure;
    if(live::started){
        IPLAirAbsorptionModel model{};float bands[IPL_NUM_BANDS]{};
        live::AirAbsorptionCalculate(live::context,{0,0,0},{nearest+opposite,0,0},&model,bands);
        reflectedHigh=std::clamp(bands[2],0.f,1.f);
    }else reflectedHigh=1.f;
    float averageAbsorption=weight>.001f?midAbsorption/weight:.14f;
    listenerSurface=surfaceReturn(nearest,probeRadius,averageAbsorption);
    listenerCeiling=surfaceReturn(height,probeRadius,averageAbsorption);
    roofWeight=space.tunnel;
    coverPercent=int(100*space.tunnel);
    diffusionPercent=int(100*space.diffusion);
    spanMetres=int(length);
    decayMs=int(1000*space.decay);
    return int(1000*space.returnLevel);
}

constexpr uintptr_t manager=0xB62CB0;
bool isPlayerSpeech(uintptr_t sound){
    return playerPed && (read<uintptr_t>(sound+4)==playerPed+0x294 ||
        (read<uintptr_t>(sound+8)==playerPed && isPedSpeech(read<short>(sound),read<unsigned>(sound+0xc))));
}
int worldChannel(IDirectSoundBuffer** output){
    int base=read<short>(manager+2), count=read<unsigned short>(manager);
    uintptr_t map=read<uintptr_t>(manager+0x87F4);
    if(!map || base<0 || count<1 || base+count>64)return -1;
    for(int i=0;i<count;i++){
        auto channel=read<uintptr_t>(hardware+0xE64+4*(base+i));
        if(channel+8!=reinterpret_cast<uintptr_t>(output))continue;
        int id=read<short>(map+2*i);
        return id>=0 && id<300 && !(read<unsigned short>(manager+4+0x74*id+0x56)&65)?base+i:-1;
    }
    return -1;
}

HRESULT createNativeBuffer(IDirectSound* ds,const DSBUFFERDESC* original,IDirectSoundBuffer** out,IUnknown* outer){
    int slot=worldChannel(out);
    int physical=-1;
    for(int j=0;j<std::min(64,int(read<unsigned short>(hardware+0x8e)));j++){
        if(read<uintptr_t>(hardware+0xe64+4*j)+8==uintptr_t(out)){
            physical=j;
            directfx::attached[j]=false;directfx::waterAttached[j]=false;
            immersion::drowning[j]=false;immersion::source[j]=0;break;
        }
    }
    if(!(original->dwFlags&DSBCAPS_CTRL3D) || slot<0){
        const auto* fmt=original->lpwfxFormat;
        if(physical>=0 && directfx::ready && fmt && fmt->nChannels==1 && fmt->nAvgBytesPerSec &&
           original->dwBufferBytes>=fmt->nAvgBytesPerSec*.150){
            DSBUFFERDESC desc=*original;
            desc.dwFlags=(desc.dwFlags&~DSBCAPS_LOCHARDWARE)|DSBCAPS_LOCSOFTWARE|DSBCAPS_CTRLFX;
            auto h=ds->CreateSoundBuffer(&desc,out,outer);
            if(SUCCEEDED(h)){
                if(directfx::install(*out,directfx::waterOnlyBase+physical,fmt->nSamplesPerSec)){
                    directfx::waterAttached[physical]=true;return h;
                }
                (*out)->Release();*out=nullptr;
            }
        }
        return ds->CreateSoundBuffer(original,out,outer);
    }
    DSBUFFERDESC desc=*original;
    desc.dwFlags=(desc.dwFlags&~DSBCAPS_LOCHARDWARE)|DSBCAPS_LOCSOFTWARE;
    if(headphones)desc.guid3DAlgorithm=DS3DALG_HRTF_FULL;
    const auto* fmt=desc.lpwfxFormat;
    if(fmt && desc.dwBufferBytes>=fmt->nAvgBytesPerSec*.150){
        desc.dwFlags|=DSBCAPS_CTRLFX;
        HRESULT hr=ds->CreateSoundBuffer(&desc,out,outer);
        if(SUCCEEDED(hr)){
            bool sdk=directfx::install(*out,slot,fmt->nSamplesPerSec);
            if(sdk || addAirEq(*out)){
                directfx::attached[slot]=sdk;directfx::waterAttached[slot]=sdk;
                return hr;
            }
            (*out)->Release();*out=nullptr;
        }
        desc.dwFlags&=~DSBCAPS_CTRLFX;
    }
    if(headphones){
        HRESULT hr=ds->CreateSoundBuffer(&desc,out,outer);
        if(SUCCEEDED(hr))return hr;
    }
    return ds->CreateSoundBuffer(original,out,outer);
}

HRESULT WINAPI createSpatial(IDirectSound* ds,const DSBUFFERDESC* desc,IDirectSoundBuffer** out,IUnknown* outer){
    HRESULT h=createNativeBuffer(ds,desc,out,outer);
    if(FAILED(h) || !out || !*out || !(desc->dwFlags&DSBCAPS_CTRLVOLUME))return h;
    int base=read<short>(manager+2),count=read<unsigned short>(manager);
    auto map=read<uintptr_t>(manager+0x87F4);
    if(!map || base<0 || count<1 || base+count>64)return h;
    for(int i=0;i<count;i++){
        if(read<uintptr_t>(hardware+0xe64+4*(base+i))+8!=uintptr_t(out))continue;
        int id=read<short>(map+2*i);
        bool cj=id>=0 && id<300 && isPlayerSpeech(manager+4+0x74*id);
        healthAudio::cjVoice[base+i]=cj;
        if(cj)SpeechBuffer::wrap(out);
        break;
    }
    return h;
}
// Read the registered emitter's forward vector only for oriented sound events.
float facingLoss(uintptr_t sound,Vec pos){
    unsigned event=read<unsigned>(sound+0xc);
    if(event!=0x34 && event!=0x35 && event!=0x5f)return 0;
    uintptr_t entity=read<uintptr_t>(sound+8);
    if(!entity)return 0;
    uintptr_t matrix{};Vec forward{};
    if(!ReadProcessMemory(GetCurrentProcess(),reinterpret_cast<void*>(entity+0x14),&matrix,sizeof(matrix),nullptr)
        || !matrix || !ReadProcessMemory(GetCurrentProcess(),reinterpret_cast<void*>(matrix+0x10),&forward,sizeof(forward),nullptr))return 0;
    Vec toward{listener.x-pos.x,listener.y-pos.y,listener.z-pos.z};
    float len=magnitude(toward)*magnitude(forward);
    if(!(len>.01f && std::isfinite(len)))return 0;
    float cosine=std::clamp((toward.x*forward.x+toward.y*forward.y+toward.z*forward.z)/len,-1.f,1.f);
    return (event==0x5f?4.f:3.f)*(1-cosine);
}
struct Source {
    EmitterGeometry geometry{};
    float roomSend{}, entrance{}, surface{}, connection{};
    int id=-1;
    DWORD checked{};
    bool panel{};
    unsigned event{};
    float route{},send{},targetLoss{},loss{},pan{},shadow{};
    Vec pos{}, lastVisible{}, entry{}, exit{};
    bool haveVisible{}, haveRoute{};
    DWORD seen{};
};
constexpr unsigned entryExitDoor=0xa3;
std::atomic<bool> engineVoice[64]{};
// CVehicle::m_vehicleAudio and its twelve engine CAESound pointers (SA 1.0).
// Game-thread only. Each pool supplies an allocation bitmap indexed by slot.
bool livePhysical(uintptr_t address){
    if(address<0x10000)return false;
    struct Pool {uintptr_t global;unsigned stride;};
    constexpr Pool pools[]={{0xB74494,0xA18},{0xB74490,0x7C4},{0xB7449C,0x19C}};
    for(auto layout:pools){
        auto pool=read<uintptr_t>(layout.global);
        if(!pool)continue;
        auto base=read<uintptr_t>(pool), flags=read<uintptr_t>(pool+4);
        int count=read<int>(pool+8);
        if(!base || !flags || count<=0 || address<base)continue;
        auto offset=address-base;
        unsigned index=offset/layout.stride;
        if(index<unsigned(count) && offset%layout.stride==0 && !(read<BYTE>(flags+index)&0x80))return true;
    }
    return false;
}
bool vehicleEngineSound(uintptr_t audio,uintptr_t sound){
    if(!audio)return false;
    uintptr_t entity=read<uintptr_t>(audio+4);
    if(!entity || audio!=entity+0x138 || !livePhysical(entity) || (read<BYTE>(entity+0x36)&7)!=2)return false;
    for(unsigned i=0;i<12;i++)
        if(read<uintptr_t>(audio+0xe4+8*i+4)==sound)return true;
    return false;
}
std::atomic<int> losses[64]{};
Source sources[64]; // Sound manager thread only.
std::atomic<int> sends[64]{},pans[64]{},shadows[64]{};
constexpr uintptr_t volumeCalls[]={0x4F0813,0x4F0907,0x4F09AE};

constexpr uintptr_t positionCalls[]={0x4F085A,0x4F0A9F};

// The original voice stays on the game device. Supply an apparent direction
// to its normal 3D position update, preserving the original radial distance.
void __fastcall sourcePosition(uintptr_t hw,void*,int base,int index,const Vec* original,int flags){
    using SetPosition=void (__thiscall*)(uintptr_t,int,int,const Vec*,int);
    const int slot=short(base)+short(index);
    Vec routed{},panelPosition{};
    if(original && hw==hardware && slot>=0 && slot<64 && GetCurrentThreadId()==gameThread){
        const Source& src=sources[slot];
        if(src.id>=0 && src.panel && directOcclusion && enclosure>=0){
            reinterpret_cast<void (__cdecl*)(Vec*,const Vec*)>(0x4D80B0)(&panelPosition,&src.pos);
            original=&panelPosition;
        }
        if(directOcclusion && src.id>=0 && src.route>0 && enclosure>=0 && GetTickCount()-src.checked<500){
            Vec portal=src.entry;
            reinterpret_cast<void (__cdecl*)(Vec*,const Vec*)>(0x4D80B0)(&routed,&portal);
            float len=magnitude(routed),radius=magnitude(*original);
            if(len>.1f && std::isfinite(len+radius)){
                const float weight=src.shadow;
                Vec blend{(1-weight)*original->x+weight*radius*routed.x/len,
                          (1-weight)*original->y+weight*radius*routed.y/len,
                          (1-weight)*original->z+weight*radius*routed.z/len};
                float norm=magnitude(blend);
                if(norm>.01f){routed={blend.x*radius/norm,blend.y*radius/norm,blend.z*radius/norm};original=&routed;}
            }
        }
    }
    reinterpret_cast<SetPosition>(0x4D8920)(hw,base,index,original,flags);
}

void __fastcall sourceVolume(uintptr_t hw,void*,int base,int index,float db,int flags) {
    using SetVolume=void (__thiscall*)(uintptr_t,int,int,float,int);
    int physical=short(base)+short(index);
    if (hw==hardware && physical>=0 && physical<64 && index>=0 &&
        index<read<unsigned short>(manager) && GetCurrentThreadId()==gameThread) {
        auto& src=sources[physical];
        auto map=read<uintptr_t>(manager+0x87F4);
        int id=map ? read<short>(map+2*index) : -1;
        uintptr_t sound=manager+4+0x74*unsigned(std::max(id,0));
        bool personal=false;
        const bool cj=id>=0 && id<300 && isPlayerSpeech(sound);
        healthAudio::cjVoice[physical]=cj;
        if(cj && read<uintptr_t>(sound+4)==playerPed+0x294){
            short context=read<short>(playerPed+0x294+0xb0);
            personal=context==357 || context==341;
        }
        immersion::drowning[physical]=personal;
        static DWORD sampled[64]{};
        static uintptr_t previous[64]{};
        uintptr_t owner=id>=0 && id<300 && !(read<unsigned short>(sound+0x56)&65)?read<uintptr_t>(sound+8):0;
        if(!owner && id>=0 && id<300 && !(read<unsigned short>(sound+0x56)&65)){
            auto audio=read<uintptr_t>(sound+4);if(audio)owner=read<uintptr_t>(audio+4);
        }
        
        if(!waterEnabled || !livePhysical(owner) || !(read<BYTE>(owner+0x41)&1)){
            immersion::source[physical]=0;previous[physical]=0;
        }else if(owner!=previous[physical] || GetTickCount()-sampled[physical]>=100){
            immersion::source[physical]=depthAt(read<Vec>(sound+0x24));
            sampled[physical]=GetTickCount();previous[physical]=owner;
        }
        if (enclosure.load()<0 || id<0 || id>=300 || (read<unsigned short>(sound+0x56)&65)) {
            src={};
            directfx::controls[physical].metres=0;
            directfx::controls[physical].visibility=1.f;
            if(directfx::attached[physical])for(int k=0;k<3;k++){
                directfx::controls[physical].air[k]=1.f;directfx::controls[physical].transmission[k]=1.f;
            }
        } else {
            DWORD now=GetTickCount();
            bool starting=src.id!=id || uintptr_t(__builtin_return_address(0))==volumeCalls[0]+5;
            if (starting) { src={};
                entranceAccess[physical]=0;
                outdoorAudible[physical]=0;wallPanOffset[physical]=0;tunnelConnection[physical]=0;
                emitterRoofPercent[physical]=0;
                directfx::controls[physical].visibility=1.f;
                for(int k=0;k<3;k++){directfx::controls[physical].transmission[k]=1.f;directfx::controls[physical].air[k]=1.f;}
                AcquireSRWLockExclusive(&sceneLock);sceneView.paths[physical]={};ReleaseSRWLockExclusive(&sceneLock);
                wallEq[physical]=0;farReturn[physical]=0;src.id=id;src.checked=now-180;src.send=roofWeight>.5f?.15f:1.f; }
            src.pos=read<Vec>(sound+0x24);
            src.pos.z+=0.32f;
            const auto* panel=activePanel(read<uintptr_t>(sound+4),now);
            src.panel=panel!=nullptr;
            if(panel)src.pos=panel->nearest(listener);
            airDistance[physical]=int(10*std::clamp(magnitude({src.pos.x-listener.x,src.pos.y-listener.y,src.pos.z-listener.z}),0.f,1000.f));
            directfx::controls[physical].metres=.1f*airDistance[physical].load();
            unsigned event=read<unsigned>(sound+0xc);
            int bank=read<short>(sound);
            src.event=event;
            uintptr_t owner=read<uintptr_t>(sound+8);
            uintptr_t audioOwner=read<uintptr_t>(sound+4);
            engineVoice[physical]=vehicleEngineSound(audioOwner,sound);
            if(event==entryExitDoor){
                src.panel=false;
                src.loss=src.targetLoss=src.route=src.shadow=0;
                wallEq[physical]=0;
                auto& ctrl=directfx::controls[physical];
                ctrl.visibility=1.f;
                for(auto& band:ctrl.transmission)band=1.f;
            }
            if(!owner && audioOwner)owner=read<uintptr_t>(audioOwner+4);
            src.geometry={panel?panel->entity:owner,src.pos,bank==31 || (event>=0x9f && event<=entryExitDoor) || event==0x13e || event==0x13f};
            if(starting && event!=entryExitDoor && directOcclusion && enclosure>=0){
                // The first playback volume can precede the next budgeted spatial update.
                static DWORD lastTick{};static unsigned checks{};
                DWORD tick=read<DWORD>(0xB7CB84);
                if(tick!=lastTick){lastTick=tick;checks=0;}
                float distanceToListener=magnitude(difference(src.pos,listener));
                if(checks<8 && distanceToListener>.1f && distanceToListener<sourceRadius){
                    ++checks;
                    TraceScope scope(src.geometry);
                    Hit hit{};void* obstacle{};
                    if(castRay(listener,src.pos,hit,obstacle)){
                        float visibility=0.f;
                        Vec side=unit(Vec{listener.y-src.pos.y,src.pos.x-listener.x,0});
                        if(magnitude(side)>.5f){
                            for(float offset:{-.9f,.9f}){
                                Vec bend=advance(hit.point,side,offset);
                                Hit other{};void* object{};
                                if(!castRay(listener,bend,other,object) && !castRay(bend,src.pos,other,object))
                                    visibility+=.42f;
                            }
                        }
                        auto& control=directfx::controls[physical];
                        control.visibility=visibility;
                        float average=0;
                        for(int k=0;k<3;k++){
                            float pass=live::surface(hit.material)->transmission[k];
                            control.transmission[k]=pass;average+=pass/3.f;
                        }
                        float gain=visibility+(1.f-visibility)*average;
                        src.loss=src.targetLoss=-20.f*std::log10(std::max(.001f,gain));
                    }
                }
            }
            if(starting){
                // A horn in the player's car shares the measured listener space.
                // Seed both sends now: a tap may end before the emitter is probed.
                float metres=magnitude(difference(src.pos,listener));
                float nearby=std::clamp(1.f-metres/10.f,0.f,1.f);
                float sharedRoof=nearby*roofWeight;
                float transfer=std::pow(10.f,-src.targetLoss/40.f);
                src.surface=tunnelWeight(listenerSurface,listenerCeiling,sharedRoof,enclosedAmount.load());
                surfaceWeight[physical]=src.surface;
                src.roomSend=sharedRoof*transfer;
                src.send=((1.f-nearby)*src.send+nearby*(1.f-.45f*sharedRoof))*transfer;
                emitterRoofPercent[physical]=int(100.f*sharedRoof);
            }
            ambientVoice[physical]=(bank>=7 && bank<=17) || bank==30 || bank==40;
            speech[physical]=isPedSpeech(bank,event);
            float intrinsic=read<float>(sound+0x14);
            emissionDb[physical]=std::isfinite(intrinsic)?int(100*std::clamp(intrinsic,-80.f,12.f)):0;
            db-=facingLoss(sound,src.pos);
            float dt=std::min<DWORD>(now-src.seen,100)*.001f;
            src.seen=now;
            src.loss+=(1.f-std::exp(-dt/(src.targetLoss>src.loss?.16f:.075f)))*(src.targetLoss-src.loss);
            if(directOcclusion && !directfx::attached[physical]){
                float applied=std::max(-100.f,db-src.loss);
                losses[physical]=int(100*std::max(0.f,db-applied));
                db=applied;
            }else losses[physical]=0;
        }
        spatialVoice[physical]=src.id>=0;
        sourceRoom[physical]=int(src.roomSend*1000);
        if(src.id<0){losses[physical]=0;engineVoice[physical]=false;wallEq[physical]=0;}
        pans[physical]=int(1000*src.pan);
        shadows[physical]=int(1000*src.shadow);
        sends[physical].store(int(src.send*1000));
    }
    reinterpret_cast<SetVolume>(0x4D8870)(hw,base,index,db,flags);
}

// Collision queries run once the main game update has returned. Audio
// callbacks only copy the current position and consume the cached result.
void updateSources(DWORD now) {
    static unsigned next=0;
    // Recent listener positions supply actual points inside passages of arbitrary orientation.
    static Vec trail[16]{};
    static unsigned head=0,count=0;
    auto separation=[](Vec a,Vec b){return magnitude({a.x-b.x,a.y-b.y,a.z-b.z});};
    float moved=count?separation(listener,trail[(head+15)%16]):1000.f;
    if(moved>45)count=0;
    if(!count || moved>=1.5f){trail[head]=listener;head=(head+1)%16;count=std::min(16u,count+1);}
    int budget=72;
    int bounceBudget=24;
    
    for (unsigned n=0;n<64 && budget>=54;n++) {
        Source& src=sources[next];
        unsigned slot=next;
        next=(next+1)%64;
        if (src.id<0 || now-src.seen>200 || now-src.checked<150) continue;
        float elapsed=.001f*std::min<DWORD>(now-src.checked,500);
        src.checked=now;
        TraceScope traceScope(src.geometry);
        Vec pos=src.pos;
        Vec d{pos.x-listener.x,pos.y-listener.y,pos.z-listener.z};
        float metres=magnitude(d);
        EarlyPaths paths{};
        WallReturns wallsFound{};
        src.route=0;
        farReturn[slot]=0;
        float visibility=1.f;
        float materialBands[3]={1.f,1.f,1.f};
        if (!(metres<sourceRadius)) {
            src.entrance=0;
            entranceAccess[slot]=0;
            outdoorAudible[slot]=0;wallPanOffset[slot]=0;src.connection=0;tunnelConnection[slot]=0;
            src.surface=0;surfaceWeight[slot]=0;
            emitterRoofPercent[slot]=0;
            src.send=0;src.roomSend=0;src.targetLoss=0;
            if(useField && metres<fieldRadius.load()){
                Vec direction=unit(d);
                if(distance(listener,direction,metres)>=metres-.06f){
                    float nearWall=std::min({distance(pos,{1,0,0},40),distance(pos,{-1,0,0},40),
                        distance(pos,{0,1,0},40),distance(pos,{0,-1,0},40)});
                    farReturn[slot]=int(1000*(.12f+.5f*(1.f-nearWall/40.f)));
                    AcquireSRWLockExclusive(&sceneLock);sceneView.directions[slot]=direction;ReleaseSRWLockExclusive(&sceneLock);
                }
            }
        }
        else {
            auto trace=[&](Vec o,Vec dir,float range){--budget;return distance(o,dir,range);};
            float transmitted=.035f, highRatio=1.f;
            if(src.event==entryExitDoor){src.shadow=0.f;}else if(live::started && metres>.05f){
                auto result=live::query(pos,metres>1.5f,budget);
                visibility=sdkGain(result.occlusion);
                src.shadow=1.f-visibility;
                transmitted=std::clamp((result.transmission[0]+result.transmission[1]+result.transmission[2])/3.f,.001f,1.f);
                std::copy_n(result.transmission,3,materialBands);
                float broadband=1-src.shadow+src.shadow*transmitted;
                highRatio=std::clamp((1-src.shadow+src.shadow*result.transmission[2])/broadband,.12f,1.f);
            }else if(metres>1.5f) {
                // Sample the projected source disc. Keep both ray endpoints on
                // their own side of thin occluders near the listener.
                float xy=std::hypot(d.x,d.y);
                Vec side=xy>.001f?Vec{-d.y/xy,d.x/xy,0}:Vec{1,0,0};
                Vec up{-side.y*d.z/metres,side.x*d.z/metres,xy/metres};
                int blocked=0;
                for(auto uv : {std::pair<float,float>{0,0},{1,0},{-1,0},{0,1},{0,-1}}){
                    Vec ray{d.x+.85f*(side.x*uv.first+up.x*uv.second),
                            d.y+.85f*(side.y*uv.first+up.y*uv.second),
                            d.z+.85f*up.z*uv.second};
                    float len=magnitude(ray);
                    blocked+=trace(listener,{ray.x/len,ray.y/len,ray.z/len},len)<len-.06f;
                }
                src.shadow=blocked*.2f;
            } else src.shadow=obstruction(listener,pos,trace);
            if(src.shadow==0.f){src.haveVisible=true;src.lastVisible=listener;}
            Vec right=read<Vec>(0xB6F028+0x974);
            src.pan=std::clamp((d.x*right.x+d.y*right.y+d.z*right.z)/std::max(metres,1.f),-1.f,1.f);
            // Measure confinement at the emitter, including open streets with facing walls.
            float wallReturn=0.f,ceilingReturn=0.f;
            Vec directionMoment{};float wallEnergy=0.f;
            SurfacePlane facades[8]{};unsigned facadeCount=0;
            auto surfaceProbe=[&](Vec direction,float range,float& strongest){
                SurfacePlane plane{};
                --budget;
                bool hit=castRay(pos,advance(pos,direction,range),plane.hit,plane.entity);
                float metres=hit?std::clamp(dot3(difference(plane.hit.point,pos),direction),0.f,range):range;
                unsigned char material=hit?plane.hit.material:0;
                float response=surfaceReturn(metres,range,live::surface(material)->absorption[1]);
                if(direction.z==0 && response>0)facades[facadeCount++]=plane;
                strongest=std::max(strongest,response);
                if(direction.z==0 && response>0){
                    float energy=response*response;
                    Vec point=advance(pos,direction,metres);
                    directionMoment=advance(directionMoment,unit(difference(point,listener)),energy);
                    wallEnergy+=energy;
                }
                return metres;
            };
            float ceiling=surfaceProbe({0,0,1},probeRadius,ceilingReturn);
            float roof=roofProximity(ceiling);
            emitterRoofPercent[slot]=int(100*roof);
            float east=surfaceProbe({1,0,0},40,wallReturn);
            float widthX=east+surfaceProbe({-1,0,0},40,wallReturn);
            float north=surfaceProbe({0,1,0},40,wallReturn);
            float widthY=north+surfaceProbe({0,-1,0},40,wallReturn);
            if(ceiling>=probeRadius-.2f){
                for(float x:{-.7071068f,.7071068f})for(float y:{-.7071068f,.7071068f})
                    surfaceProbe({x,y,0},40,wallReturn);
            }
            float walls=std::clamp((56.f-std::min(widthX,widthY))/44.f,0.f,1.f);
            if(ceiling<probeRadius-.2f){
                float shared=largeRoomAmount.load()*std::clamp(1.f-metres/probeRadius,0.f,1.f);
                roof=std::max(roof,shared);
                walls=std::max(walls,shared);
                emitterRoofPercent[slot]=int(100*roof);
            }
            float localRoom=walls*(.52f+.48f*roof)*(1.f-enclosedAmount.load()*(1.f-entranceLeak));
            // Some material-transmitted energy still excites the indirect field when the
            // budgeted corner search misses. The room path is an energy envelope.
            float transfer=(1.f-src.shadow)+src.shadow*std::sqrt(transmitted);
            float horizontal=std::hypot(d.x,d.y);
            src.targetLoss=-20.f*std::log10(std::max(.001f,1-src.shadow+src.shadow*transmitted));
            if(src.shadow>.05f && metres<.5f*sourceRadius && horizontal>.3f){
                Vec unit{d.x/metres,d.y/metres,d.z/metres};
                Vec side{-d.y/horizontal,d.x/horizontal,0};
                Hit nearFace{},farFace{};
                void* nearObject{};void* farObject{};
                budget-=2;
                bool hitNear=castRay(listener,pos,nearFace,nearObject);
                bool hitFar=castRay(pos,listener,farFace,farObject);
                float nearHit=hitNear?dot3(difference(nearFace.point,listener),unit):metres;
                float farHit=hitFar?dot3(difference(farFace.point,listener),unit):0.f;
                float entry=std::max(0.f,std::min(nearHit,farHit)-.45f);
                float exit=std::min(metres,std::max(nearHit,farHit)+.45f);
                bool narrowDetour=false;
                auto bend=[&](float at,float offset){
                    return Vec{listener.x+unit.x*at+side.x*offset,
                               listener.y+unit.y*at+side.y*offset,listener.z+unit.z*at};
                };
                auto clear=[&](Vec a,Vec b){
                    Vec delta{b.x-a.x,b.y-a.y,b.z-a.z};float len=magnitude(delta);
                    return len<.005f || trace(a,{delta.x/len,delta.y/len,delta.z/len},len)>=len-.025f;
                };
                // A T junction often has a shorter L-shaped route than a symmetric detour.
                // Use the shortest verified polyline, including the final leg to the emitter.
                float best=1e9f,bestPan=src.pan,bestExtra=0;
                auto length=[](Vec a,Vec b){return magnitude({a.x-b.x,a.y-b.y,a.z-b.z});};
                Vec chosenA{},chosenB{};
                auto consider=[&](Vec a,Vec b,bool narrow){
                    float first=length(a,listener);
                    float total=first+length(a,b)+length(b,pos);
                    if(budget<3 || first<.15f || total>=best || !clear(listener,a) || !clear(a,b) || !clear(b,pos))return;
                    chosenA=a;chosenB=b;best=total;bestExtra=std::max(0.f,total-metres);
                    bestPan=std::clamp(((a.x-listener.x)*right.x+(a.y-listener.y)*right.y+
                                        (a.z-listener.z)*right.z)/first,-1.f,1.f);
                    narrowDetour=narrow;
                };
                if(src.haveRoute)consider(src.entry,src.exit,false);
                Vec edge{};
                if(hitNear && hitFar && outsideCorner(nearFace.point,nearFace.normal,farFace.point,farFace.normal,listener,pos,edge))
                    consider(edge,edge,false);
                if(src.haveVisible)consider(src.lastVisible,src.lastVisible,false);
                // Narrow posts: search both nearby edges before spending rays on player history.
                for(float offset:{.55f,-.55f,1.2f,-1.2f}){
                    Vec a=bend(entry,offset),b=bend(exit,offset);
                    consider(a,b,exit-entry<3.5f);
                }
                if(src.haveVisible && length(src.lastVisible,pos)<150){
                    for(unsigned age : {2u,4u,7u,11u,15u}){
                        if(age>=count)break;
                        Vec waypoint=trail[(head+15-age)%16];
                        consider(waypoint,src.lastVisible,false);
                    }
                }
                // An opening under a raised panel or over a low barrier can be vertical.
                for(float dz : {-.8f,1.1f}){
                    Vec a=bend(entry,0),b=bend(exit,0);a.z+=dz;b.z+=dz;
                    consider(a,b,true);
                }
                float midZ=(listener.z+pos.z)*.5f;
                for(Vec corner : {Vec{pos.x,listener.y,midZ},Vec{listener.x,pos.y,midZ}})
                    consider(corner,corner,false);
                for(float offset : {1.f,-1.f,2.5f,-2.5f,6.f,-6.f,14.f,-14.f,26.f,-26.f}){
                    consider(bend(entry,offset),bend(exit,offset),std::abs(offset)<=2.5f && exit-entry<3.5f);
                    Vec corner=bend(.5f*(entry+exit),offset);consider(corner,corner,false);
                }
                if(best<1e9f){
                    src.haveRoute=true;src.entry=chosenA;src.exit=chosenB;
                    float reach=1.f/(1.f+.09f*bestExtra);
                    src.pan+=src.shadow*(bestPan-src.pan);
                    transfer+=(1.f-transfer)*reach;src.route=.9f*src.shadow*reach;
                }
                
                if(narrowDetour)src.targetLoss*=.3f;
            }
            if(src.route>0){
                // The direct and diffracted energies are represented by one native voice.
                float direct=std::pow(10.f,-src.targetLoss/20.f);
                src.targetLoss=-20.f*std::log10(std::min(1.f,std::hypot(direct,src.route)));
            }
            // Single outdoor facade: validate both legs even when the listener has no nearby walls.
            float openPath=(1.f-roof)*(1.f-enclosedAmount.load());
            if(openPath>.01f && facadeCount){
                auto reflected=firstBounce(pos,metres,right,bounceBudget,facades,facadeCount);
                // Allow a weaker scattered return from a visible patch when
                // the specular point is beyond the edge of a finite facade.
                if(reflected.peak()==0 && bounceBudget>0){
                    for(unsigned j=0;j<facadeCount && bounceBudget>0;j++){
                        auto& face=facades[j].hit;
                        Vec incoming=difference(face.point,listener);
                        float leg=magnitude(incoming), sourceLeg=magnitude(difference(face.point,pos));
                        if(leg<.1f || dot3(difference(pos,face.point),face.normal)*dot3(difference(listener,face.point),face.normal)<=0)continue;
                        --bounceBudget;
                        if(distance(listener,unit(incoming),leg)<leg-.12f)continue;
                        float gain=.36f*surfaceReturn(sourceLeg,40,live::surface(face.material)->absorption[1]);
                        auto patch=arrival(leg+sourceLeg,metres,gain,dot3(unit(incoming),right));
                        patch.bearing=unit(incoming);
                        reflected.include(patch);
                    }
                }
                for(auto path:reflected.side){
                    path.gain*=openPath*(.35f+.65f*wallReturn);
                    localRoom=std::max(localRoom,.42f*path.gain/.44f);
                    wallsFound.include(path);
                }
            }
            // Preserve a single wall's level while sharing it across both arrivals.
            float total=wallsFound.side[0].gain+wallsFound.side[1].gain;
            if(total>0.f){
                for(unsigned side=0;side<2;side++){
                    paths.wall[side]=wallsFound.side[side];
                    paths.wall[side].gain*=wallsFound.peak()/total;
                }
            }
            float coupling=0.f; Vec mouthDirection{};
            const NearbySpace* selected=nullptr;
            const NearbySpace* second=nullptr;float secondGain=0;
            float sourceLeg=0,earLeg=0;
            for(const auto& candidate:neighbours){
                if(candidate.strength<=0)continue;
                float a=magnitude(difference(candidate.point,pos)),b=magnitude(difference(candidate.point,listener));
                float strength=candidate.strength*(1.f-enclosedAmount.load()*entranceLeak)/
                               (1.f+.024f*(a+b));
                if(b>48.f || a>75.f)continue;
                if(strength>coupling){
                    second=selected;secondGain=coupling;
                    selected=&candidate;coupling=strength;sourceLeg=a;earLeg=b;
                }else if(strength>secondGain){second=&candidate;secondGain=strength;}
            }
            for(int attempt=0;selected && attempt<2;attempt++){
                sourceLeg=magnitude(difference(selected->point,pos));
                earLeg=magnitude(difference(selected->point,listener));
                mouthDirection=unit(difference(selected->point,listener));
                if(trace(listener,mouthDirection,earLeg)<earLeg-.08f ||
                   trace(pos,unit(difference(selected->point,pos)),sourceLeg)<sourceLeg-.08f){
                    selected=second;coupling=attempt==0?secondGain:0.f;continue;
                }
                paths.entryEcho=arrival(sourceLeg+earLeg+4.f,metres,.63f*coupling,dot3(right,mouthDirection));
                break;
            }
            Vec arrival=src.route>0 ? Vec{src.entry.x-listener.x,src.entry.y-listener.y,src.entry.z-listener.z} : d;
            AcquireSRWLockExclusive(&sceneLock);
            Vec sourceDirection=unit(arrival);
            float reflectionMix=wallReturn*std::clamp(1.f-src.shadow,0.f,1.f);
            Vec wallDirection=wallEnergy>.001f?advance({},directionMoment,1.f/wallEnergy):sourceDirection;
            Vec fieldDirection=advance(sourceDirection,difference(wallDirection,sourceDirection),reflectionMix);
            float mouthShare=coupling/(localRoom+coupling+.001f);
            sceneView.directions[slot]=advance(fieldDirection,difference(mouthDirection,fieldDirection),mouthShare);
            ReleaseSRWLockExclusive(&sceneLock);
            wallEq[slot]=directOcclusion?int(-2000.f*std::log10(highRatio)*(1-src.route)):0;
            auto follow=[elapsed](float old,float target){
                float tau=target<old?.32f:.09f;
                return old+(target-old)*(1.f-std::exp(-elapsed/tau));
            };
            src.entrance=follow(src.entrance,std::min(1.f,1.f-src.shadow+src.route));
            entranceAccess[slot]=src.entrance;
            float access=std::clamp(1.f-src.shadow+src.route,0.f,1.f);
            float connected=0.f;
            if(metres>12.f && roof*walls>.05f && access>.05f && enclosedAmount.load()>.05f){
                Vec middle=advance(listener,d,.5f);
                float midRoof=trace(middle,{0,0,1},probeRadius);
                connected=enclosedAmount.load()*roof*walls*access*
                    std::clamp((probeRadius-midRoof)/(.2f*probeRadius),0.f,1.f);
            }
            src.connection=follow(src.connection,connected);
            tunnelConnection[slot]=src.connection;
            outdoorAudible[slot]=openPath*wallReturn*float(wallsFound.peak()>0);
            float wallPan=wallEnergy>.001f?dot3(directionMoment,right)/wallEnergy:src.pan;
            float roomPan=src.pan+(wallPan-src.pan)*wallReturn*access;
            wallPanOffset[slot]=roomPan+mouthShare*(dot3(right,mouthDirection)-roomPan)-src.pan;
            src.surface=follow(src.surface,tunnelWeight(wallReturn,ceilingReturn,roof*walls,enclosedAmount.load()));
            surfaceWeight[slot]=src.surface;
            src.roomSend=follow(src.roomSend,std::max(localRoom*transfer,coupling));
            src.send=follow(src.send,transfer*(1.f-.45f*localRoom));

        }
        AcquireSRWLockExclusive(&sceneLock); sceneView.paths[slot]=paths; ReleaseSRWLockExclusive(&sceneLock);
        if(directfx::attached[slot]){
            auto& control=directfx::controls[slot];
            control.visibility=directOcclusion?visibility:1.f;
            for(int k=0;k<3;k++){
                float detour=(src.shadow>.001f)?src.route/src.shadow:0.f;
                control.transmission[k]=directOcclusion?blockedTransfer(sdkGain(materialBands[k]),detour,k):1.f;
                control.air[k]=1.f; // AirPath handles distance once, at the native sample rate.
            }
        }
        sourceRoom[slot]=int(src.roomSend*1000);
        sends[slot]=int(src.send*1000);
        pans[slot]=int(src.pan*1000);
        shadows[slot]=int(src.shadow*1000);
    }
}

StaticWater waterCopies[64];
void refreshStaticWater(){
    int count=std::min(64,int(read<unsigned short>(hardware+0x8e)));
    auto stream=read<uintptr_t>(hardware+0xe10);
    for(int slot=0;slot<count;slot++){
        auto channel=read<uintptr_t>(hardware+0xe64+4*slot);
        if(!channel || channel==stream || directfx::waterAttached[slot]){waterCopies[slot].forget();continue;}
        waterCopies[slot].update(read<IDirectSoundBuffer*>(channel+8),read<uintptr_t>(channel+0x84),immersion::channel(slot));
    }
}
// Copy the static sound bank sample while the game owns the channel.
// The mixer queries playback state but never locks the game's playback buffer.
HRESULT WINAPI play(IDirectSoundBuffer* b,uintptr_t channel,DWORD flags) {
    auto nativePlay=[&](){return b->Play(0,0,flags);};
    if(!ready.load())return nativePlay();
    WAVEFORMATEX fmt{}; DSBCAPS caps{}; caps.dwSize=sizeof(caps);
    if (FAILED(b->GetFormat(&fmt,sizeof(fmt),nullptr)) || fmt.wFormatTag!=WAVE_FORMAT_PCM ||
        fmt.nChannels!=1 || fmt.wBitsPerSample!=16 || FAILED(b->GetCaps(&caps)) || caps.dwBufferBytes<4) return nativePlay();
    int physical=-1;
    const int count=std::min(64,int(read<unsigned short>(hardware+0x8e)));
    for (int i=0;i<count;i++) {
        if (read<uintptr_t>(hardware+0xE64+4*i)==channel) {physical=i;break;}
    }
    if (physical<0) return nativePlay();
    if(GetCurrentThreadId()==gameThread && !directfx::waterAttached[physical])
        waterCopies[physical].update(b,read<uintptr_t>(channel+0x84),immersion::channel(physical));
    Voice fresh{};
    if(!directfx::attached[physical] && (caps.dwFlags&DSBCAPS_CTRLFX))fresh.airEq=getAirEq(b);
    fresh.looping=(flags&DSBPLAY_LOOPING)!=0;
    fresh.samples=caps.dwBufferBytes/2;
    fresh.bankRate=fmt.nSamplesPerSec;
    fresh.isPlayer=healthAudio::cjVoice[physical].load();
    const int gameBase=read<short>(manager+2),index=physical-gameBase;
    const auto gameMap=read<uintptr_t>(manager+0x87f4);
    if(GetCurrentThreadId()==gameThread && gameMap && gameBase>=0 && index>=0 && index<read<unsigned short>(manager)){
        int sound=read<short>(gameMap+2*index);
        fresh.isPlayer=sound>=0 && sound<300 && isPlayerSpeech(manager+4+0x74*sound);
    }
    DWORD bytes=read<DWORD>(channel+(fresh.looping ? 0x68 : 0x2c));
    DWORD prefix=fresh.looping ? read<DWORD>(channel+0x64) : 0;
    if (bytes>=2 && bytes<=8*1024*1024 && !((bytes|prefix)&1) &&
        prefix<bytes && prefix<=caps.dwBufferBytes) {
        fresh.pcmSamples=bytes/2;
        fresh.prefix=prefix/2;
        fresh.pcm.reset(new(std::nothrow) short[fresh.pcmSamples]);
        SIZE_T copied=0;
        auto src=read<const void*>(channel+0x84);
        if (fresh.pcm && (!ReadProcessMemory(GetCurrentProcess(),src,fresh.pcm.get(),bytes,&copied)
                          || copied!=bytes)) fresh.pcm.reset();
        DWORD position=0;
        if (SUCCEEDED(b->GetCurrentPosition(&position,nullptr))) {
            fresh.lastPosition=position/2;
            fresh.intro=prefix && read<BYTE>(channel+0x60) && position>=caps.dwBufferBytes-prefix;
        }
    }
    if(fresh.isPlayer && fresh.pcm){
        SpeechRoute* route{};
        b->QueryInterface(speechRouteId,(void**)&route);
        if(!route && GetCurrentThreadId()==gameThread && (caps.dwFlags&DSBCAPS_CTRLVOLUME) &&
           read<IDirectSoundBuffer*>(channel+8)==b){
            auto** storage=reinterpret_cast<IDirectSoundBuffer**>(channel+8);
            SpeechBuffer::wrap(storage);
            b=*storage;b->QueryInterface(speechRouteId,(void**)&route);
        }
        if(route){
            fresh.player.reset(new(std::nothrow)SpeechPlayback(route,physical,directfx::attached[physical]));
            if(!fresh.player)route->Release();
            else if(gameOutput::live() && gameOutput::hasSDK && healthAudio::snapshot().amount>0.f)
                fresh.player->setRoute(true);
        }
    }
    HRESULT hr=nativePlay();
    if(FAILED(hr))return hr;
    AcquireSRWLockExclusive(&voiceLock);
    // A paused loop can retain its buffer until this physical channel is reused.
    Voice& slot=voices[physical];
    for(auto& v:voices) if(v.buffer && (&v==&slot || v.buffer==b)){
        retiredField[v.isPlayer][v.channel]+=v.lastAmbient;
        for(unsigned k=0;k<v.lastBus.size();++k)retiredBus[v.isPlayer][k]+=v.lastBus[k];
        v.buffer->Release();v={};
    }
    if(fresh.pcm){
        b->AddRef();fresh.buffer=b;fresh.channel=physical;
        slot=std::move(fresh);
    }
    ReleaseSRWLockExclusive(&voiceLock);
    return hr;
}

struct MixBuses {
    float *mono,*side,*room,*fieldMono;
    void add(int i,const std::array<float,4>& v,float gain=1.f){
        
        mono[i]+=v[0]*gain;side[i]+=v[1]*gain;
        room[2*i]+=v[2]*gain;room[2*i+1]+=v[3]*gain;
    }
    void fade(std::array<float,4>& last){
        for(int i=0;i<128;i++)add(i,last,1.f-(i+1)/128.f);
        last={};
    }
};

immersion::Transmission emitterWater[64],listenerWater[2];

bool mixVoice(Voice& v,MixBuses buses,bool active,unsigned queuedFrames,EarlyPaths paths,ArrivalQueue& queue) {
    auto fadeVoice=[&](){
        buses.fade(v.lastBus);
        if(buses.fieldMono){
            for(int i=0;i<128;i++)buses.fieldMono[i]=v.lastAmbient*(1.f-(i+1)/128.f);
        }
        v.lastAmbient=0;
    };
    DWORD status,hz,bytePos;
    auto b=v.buffer;
    if(FAILED(b->GetStatus(&status))){fadeVoice();return false;}
    if(!(status&DSBSTATUS_PLAYING)){
        fadeVoice();v.synced=false;
        return v.looping; // GTA can resume the same buffer without invoking the hooked Play site.
    }
    LONG volume;
    if (!active || !spatialVoice[v.channel] || FAILED(b->GetVolume(&volume)) || (volume+losses[v.channel].load()<=-7000 && (std::max(tunnelConnection[v.channel].load(),outdoorAudible[v.channel].load())<.05f || volume<=-10000)) ||
        FAILED(b->GetFrequency(&hz)) || hz==0 || hz>200000 ||
        FAILED(b->GetCurrentPosition(&bytePos,nullptr))) { fadeVoice();v.synced=false; return true; }
    double step=double(hz)/Room::rate;
    double actual=(bytePos/2)%v.samples;
    if (actual<v.lastPosition) v.intro=false;
    v.lastPosition=DWORD(actual);
    // A looping DirectSound buffer can contain several copies of the same
    // sound-bank loop. Resetting at the buffer boundary can skip part of a cycle.
    const DWORD loopFrames=v.pcmSamples-v.prefix;
    bool resync=!v.synced || (!v.looping && std::abs(actual-v.cursor)>hz*.09);
    double target=actual;
    if(v.looping){
        if(v.intro && actual>=v.samples-v.prefix)target=actual-(v.samples-v.prefix);
        else target=v.prefix+std::fmod(actual,double(loopFrames));
        target+=queuedFrames*step;
        if(target>=v.pcmSamples)target=v.prefix+std::fmod(target-v.prefix,double(loopFrames));
        if(!resync && target>=v.prefix && v.cursor>=v.prefix){
            double error=phaseDifference(target,v.cursor,loopFrames);
            // Ignore normal DirectSound cursor jitter. Confirm a large error over several blocks.
            if(std::abs(error)>hz*.038){
                if(++v.offPhase>=4){resync=true;}
            }else{
                v.offPhase=0;
                if(std::abs(error)>hz*.010)step*=1.+std::clamp(error/hz*.1,-.002,.002);
            }
        }
    }
    if(resync){v.cursor=target;v.synced=true;v.offPhase=0;}
    if(!v.looping && v.cursor>=v.pcmSamples){fadeVoice();return true;}
    DWORD start=DWORD(v.cursor);
    double frac=v.cursor-start;
    auto sample=[&](DWORD idx)->float {
        DWORD pos=start+idx;
        if(!v.looping)return pos<v.pcmSamples?v.pcm[pos]:0.f;
        if(pos<v.prefix)return v.pcm[pos];
        DWORD offset=(pos-v.prefix)%loopFrames;
        float value=v.pcm[v.prefix+offset];
        // Remove an endpoint step without changing the loop period or pitch.
        DWORD ramp=std::min<DWORD>(loopFrames/4,std::max<DWORD>(1,hz/500));
        if(offset<ramp){
            float t=1.f-float(offset)/ramp;
            value+=(float(v.pcm[v.pcmSamples-1])-v.pcm[v.prefix])*t*t;
        }
        return value;
    };
    float sourceGain=std::pow(10.f,std::min(0.f,float(volume+losses[v.channel].load()))/2000.f)/32768.f;
    float metres=.1f*airDistance[v.channel].load();
    float airDb=std::min(30.f,airDbPerMetre[3]*metres);
    int eqStep=std::min(60,int((airDb+.01f*wallEq[v.channel].load())*2+.5f));
    DWORD eqTime=GetTickCount();
    if(v.airEq && eqStep!=v.lastEq && (!v.nextEq || LONG(eqTime-v.nextEq)>=0)){
        eqStep=v.lastEq+std::clamp(eqStep-v.lastEq,-3,3);
        v.nextEq=eqTime+32;
        DSFXParamEq params{8000.f,24.f,-.5f*eqStep};
        if(SUCCEEDED(v.airEq->SetAllParameters(&params)))v.lastEq=eqStep;
    }
    float highBand=std::pow(10.f,-std::min(15.f,.09f*metres)/20.f);
    float emitted=std::pow(10.f,emissionDb[v.channel].load()/2000.f);
    float distanceMix=std::clamp((metres-12.f)/88.f,0.f,1.f);
    sourceGain*=returnGain(.01f*coverPercent.load(),.01f*emitterRoofPercent[v.channel].load(),metres,farGainDb.load(),emitted,entranceAccess[v.channel].load());
    // Recover return energy only when both endpoints have overhead cover.
    const float unboostedGain=sourceGain;
    sourceGain*=1.f+1.55f*corridorAmount.load()*.01f*emitterRoofPercent[v.channel].load();
    sourceGain*=std::pow(10.f,.05f*tunnelBoostDb.load()*surfaceWeight[v.channel].load());
    // Engine loops already sustain the room. Limit the extra tunnel gain for them.
    if(engineVoice[v.channel])sourceGain=std::min(sourceGain,unboostedGain*1.12f);
    if(speech[v.channel])sourceGain*=speechMix.load();
    float quietFloor=quietFloorGain.load();
    float gain=(sends[v.channel].load()*0.001f)*sourceGain;
    float recover=tunnelRecoveryDb(metres,tunnelConnection[v.channel].load(),farGainDb.load(),
                                   emissionDb[v.channel].load()*.01f,(volume+losses[v.channel].load())*.01f);
    float localGain=sourceGain*sourceRoom[v.channel].load()*.001f*std::pow(10.f,.05f*recover);
    // Distant traffic and ambient banks can excite a quiet directional bed even on open streets.
    float ambientSend=buses.fieldMono && ambientVoice[v.channel] ?
        .12f*distanceMix*sourceGain*sends[v.channel].load()*.001f : 0.f;
    if(buses.fieldMono)ambientSend+=sourceGain*.001f*farReturn[v.channel].load();
    float fieldTarget=buses.fieldMono?fieldDistanceGain(metres,float(fieldRadius.load())):0.f;
    // The output block already crossfades on synchronization. Start the room
    // send at its measured level so its slower tracking filter preserves attacks.
    if(resync)v.roomGain=localGain;
    // Keep the measured delay fixed between geometry updates. Smoothing a
    // moving read offset would pitch-shift sustained notes such as horns.
    const float coverageBefore=v.bounceCoverage;
    constexpr float follow=.064493f; // 8 ms block, 120 ms time constant
    const bool measured=paths.wall[0].gain>0 || paths.wall[1].gain>0;
    v.bounceCoverage+=follow*(float(measured)-v.bounceCoverage);
    const int stretch=echoStretch.load();
    for(unsigned side=0;side<2;side++){
        auto& path=paths.wall[side];
        const auto& old=v.lastPaths.wall[side];
        float target=path.gain;
        if(target==0.f)path=old;
        if(old.gain>0.f)path.pan=old.pan+follow*(path.pan-old.pan);
        path.gain=resync?target:old.gain+follow*(target-old.gain);
        if(target==0.f && path.gain<.0001f)path={};
        v.nearWall[side].update(path,v.looping,resync || stretch!=v.wallStretch,stretch);
    }
    v.wallStretch=stretch;
    // Per-channel state belongs to the worker. A new sound starts with clean history.
    auto& sub=emitterWater[v.channel];
    const auto medium=immersion::settings(immersion::source[v.channel].load()*(1.f-immersion::listener.load()));
    if(resync)sub.clear();
    auto resample=[&](int i){
        const double p=frac+i*step;
        const DWORD j=DWORD(p);
        const float x=sample(j);
        return x+float(p-j)*(sample(j+1)-x);
    };
    float sourcePCM[frames];
    const bool throughWater=sub.needsAudio(medium.wet);
    if(throughWater){
        for(int i=0;i<frames;i++)sourcePCM[i]=resample(i)/32768.f;
        for(unsigned i=0;i<frames;i+=immersion::block)sub.process(sourcePCM+i,medium);
    }else sub.noteDry();
    auto before=v.lastBus;
    float panTarget=pans[v.channel].load()*.001f;
    const float wallOffset=wallPanOffset[v.channel].load();
    float edgeFilter=.87f-.66f*shadows[v.channel].load()*.001f;
    for (int i=0;i<frames;i++) {
        float raw=throughWater?32768.f*sourcePCM[i]:resample(i);
        v.airLow+=.48f*(raw-v.airLow);
        raw=v.airLow+highBand*(raw-v.airLow);
        float intensity=std::abs(raw)*emitted/32768.f;
        v.envelope+=(intensity-v.envelope)*(intensity>v.envelope?.024f:.00038f);
        raw*=quietFloor+(1.f-quietFloor)*std::min(1.f,v.envelope*3.f);
        v.roomGain+=.0006f*(localGain-v.roomGain);
        v.gain+=(gain-v.gain)*0.025f;
        float value=v.gain*raw;
        v.shade+=edgeFilter*(value-v.shade);
        v.pan+=.001f*(panTarget-v.pan);
        float roomSample=raw*v.roomGain;
        v.roomPan+=.001f*(std::clamp(v.pan+wallOffset,-1.f,1.f)-v.roomPan);
        v.lastBus={v.shade,v.shade*v.pan,
                   roomSample*(1-.85f*v.roomPan),roomSample*(1+.85f*v.roomPan)};
        if(resync && i<128)for(unsigned k=0;k<before.size();++k)
            v.lastBus[k]=before[k]+(v.lastBus[k]-before[k])*((i+1)/128.f);
        float t=float(i+1)/frames;
        float earlySample=raw*sourceGain;
        for(unsigned side=0;side<2;side++){
            queue.add(i,v.lastPaths.wall[side],earlySample*(1.f-t));
            queue.add(i,paths.wall[side],earlySample*t);
        }
        queue.add(i,v.lastPaths.entryEcho,earlySample*(1.f-t));
        queue.add(i,paths.entryEcho,earlySample*t);
        float replaced=coverageBefore+t*(v.bounceCoverage-coverageBefore);
        auto bus=v.lastBus;bus[0]*=1.f-replaced;bus[1]*=1.f-replaced;
        buses.add(i,bus);
        if(buses.fieldMono){
            v.fieldLevel+=.001f*(fieldTarget-v.fieldLevel);
            v.ambientGain+=.001f*(ambientSend-v.ambientGain);
            float value=v.fieldLevel*(.5f*(v.lastBus[2]+v.lastBus[3])+v.ambientGain*raw);
            if(resync && i<128)value=v.lastAmbient+(value-v.lastAmbient)*(i+1)/128.f;
            buses.fieldMono[i]=value;
            if(i==frames-1)v.lastAmbient=value;
        }
    }
    v.lastPaths=paths;
    v.cursor+=frames*step;
    if(v.looping && v.cursor>=v.pcmSamples){
        v.cursor=v.prefix+std::fmod(v.cursor-v.prefix,double(loopFrames));
    }
    return true;
}

template<bool Player>
void render(short* out,SteamReturn& steam,SteamReturn (&emit)[2], SoundField& field,unsigned queuedFrames,float* protectedDry,bool protect,bool retire=false) {
    static Room room,longRoom{true};
    static WetLimiter<frames> limiter;
    static ArrivalQueue queue;
    WetLimiter<frames>::Block wetBlock{};
    float roomIn[frames*2]{};
    float input[frames]{},stereoSide[frames]{};
    int state=retire || GetTickCount()-lastFrame.load()>1500 ? -1 : enclosure.load();
    static float early[2][16384]{},wallGain[2]{},farMix[2]{},axialMix=0,covered=0,diffuse=0;
    static ReflectionTone tone[2][2];
    static DirectTone equalizer[2][2];
    static bool eqReady=false;
    if(!eqReady){for(auto& bus:equalizer)for(auto& eq:bus)eq.configure(Room::rate);eqReady=true;}
    const float hall=largeRoomAmount.load();
    float settings[2][3];static float smooth[2][3]={{1,1,1},{1,1,1}};
    for(int b=0;b<2;b++)for(int k=0;k<3;k++){
        smooth[b][k]+=.15f*(wetEq[b][k].load()-smooth[b][k]);
        settings[b][k]=smooth[b][k];
    }
    float echo[2*frames]{};
    static unsigned head;
    float metres[]={lateralRange[0].load(),lateralRange[1].load()};
    float farTarget[2],gains[2];
    for(int ch=0;ch<2;ch++) {
        farTarget[ch]=std::clamp((metres[ch]-3)/22,0.f,1.f);
        gains[ch]=1.20f*lateralReturn[ch].load();
    }
    static float level;
    float blockWet=wet.load();
    float target=std::max(state,0)*0.001f*blockWet;
    SceneView view;
    AcquireSRWLockShared(&sceneLock);view=sceneView;ReleaseSRWLockShared(&sceneLock);
    static float blend=0.f;
    bool enabled=useField.load(), spatial=state>=0 && field.available && (enabled || blend>0.f);
    static bool previousSpatial=false;
    if(!spatial && previousSpatial)field.resetDecoder(field.dec);
    previousSpatial=spatial;
    if(spatial)std::memset(field.mix,0,sizeof(field.mix));
    MixBuses buses{input,stereoSide,roomIn,nullptr};
    static float directional[64][frames];
    const int stretch=echoStretch.load();
    bool mixed[64]{};
    AcquireSRWLockExclusive(&voiceLock);
    buses.fade(retiredBus[Player]);
    auto outgoing=retiredField[Player];
    retiredField[Player].fill(0.f);
    for(auto& v:voices){
        if(!v.buffer || v.isPlayer!=Player)continue;
        if constexpr(Player)if(v.player){
            v.player->setRoute(protect);
            if(v.player->diverted)v.player->mix(v.buffer,v.pcm.get(),v.pcmSamples,v.prefix,v.samples,
                v.bankRate,v.looping,v.intro,queuedFrames,pans[v.channel].load()*.001f,protectedDry);
        }
        int slot=v.channel;
        float* mono=directional[slot];
        if(spatial){std::fill_n(mono,frames,0.f);buses.fieldMono=mono;}
        for(auto& bounce:view.paths[slot].wall){
        if(bounce.gain>0 && magnitude(bounce.bearing)>.5f){
            auto dir=audioVector(bounce.bearing),right=view.camera.right;
            bounce.pan=std::clamp(dir.x*right.x+dir.y*right.y+dir.z*right.z,-1.f,1.f);
        }
        bounce.delay=(bounce.delay*stretch+50)/100;
        }
        view.paths[slot].entryEcho.delay=(view.paths[slot].entryEcho.delay*stretch+50)/100;
        bool keep=mixVoice(v,buses,state>=0,queuedFrames,view.paths[slot],queue);
        mixed[slot]=spatial;
        if(!keep){v.buffer->Release();v={};}
    }
    ReleaseSRWLockExclusive(&voiceLock);
    float arrivals[frames*2];queue.drain(arrivals,frames);
    if(spatial){
        for(int slot=0;slot<64;slot++){
            if(outgoing[slot]!=0){
                for(int k=0;k<4;k++){
                    float last=field.previous[slot][k]*outgoing[slot];
                    for(int i=0;i<128;i++)field.mix[k][i]+=last*(1.f-(i+1)/128.f);
                }
            }
            auto* mono=directional[slot];
            if(mixed[slot] && std::any_of(mono,mono+frames,[](float x){return x!=0.f;}))
                field.add(slot,mono,view.directions[slot]);
        }
        field.spatialize(roomIn,view,enabled?1.f:0.f,blend);
    }
    static float decayEnvelope=450.f, bassReturn[2]{};
    static bool silent=false;
    static float low[2][2]{},hallBass[2]{};
    static float lateLow[2]{};
    static Room emitterFallback[2];
    static int lastStretch=100,lastEchoDelay=0,lastPreDelay=0;
    static float delayed[2][2][8192]{};
    static unsigned delayHead=0;
    static float taps[2][8192]{};static unsigned tapHead=0;
    if (state<0) {
        if(!silent){
            listenerWater[Player].clear();
            std::memset(low,0,sizeof low);
            std::fill_n(hallBass,2,0.f);
            std::memset(delayed,0,sizeof delayed);
            std::memset(taps,0,sizeof taps);
            std::memset(queue.stereo,0,sizeof queue.stereo);
            if(steam.effect)steam.reset(steam.effect);
            for(auto& e:emit)if(e.effect)e.reset(e.effect);
            for(auto& r:emitterFallback)r.clear();
            for(auto& ch:early)std::fill_n(ch,16384,0.f);
            for(auto& bus:tone)for(auto& f:bus)f={};
            for(auto& bus:equalizer)for(auto& f:bus)f.clear();
            room.clear();longRoom.clear();limiter={};
            covered=diffuse=blend=level=0;
            std::fill_n(lateLow,2,0.f);
            std::fill_n(bassReturn,2,0.f);
            decayEnvelope=450.f;silent=true;
        }
        std::fill_n(out,frames*2,0);return;
    }
    silent=false;
    float diffuseTarget=diffusionPercent.load()*.01f;
    float coverTarget=coverPercent.load()*.01f;
    float axialTarget=std::clamp((spanMetres.load()-24)/82.f,0.f,1.f);
    static float soft=0;
    soft+=.04f*(materialPercent.load()*.01f-soft);
    float high=reflectedHigh.load()*(1.f-.58f*soft);
    // One update per 8 ms block. Let energy stored under the roof ring out at the exit.
    float wantedDecay=float(decayMs.load());
    decayEnvelope+=(wantedDecay-decayEnvelope)*(wantedDecay>decayEnvelope?.035f:(wantedDecay<800.f && enclosedAmount.load()>.75f?.035f:.005f));
    const float duration=.01f*decayPercent.load();
    const float lowTime=.01f*lowDecayPercent.load(),highTime=.01f*highDecayPercent.load();
    const int echoOffset=32*echoDelayMs.load(),lateOffset=32*preDelayMs.load();
    int decay=int(decayEnvelope);
    room.setDecay(int(duration*std::min(decay,1050)));
    if(!steam.effect)longRoom.setDecay(int(duration*decay));
    static float enclosedMix=0,corridorMix=0;
    enclosedMix+=.045f*(enclosedAmount.load()-enclosedMix);
    corridorMix+=.045f*(corridorAmount.load()-corridorMix);
    float emitterSeconds=duration*(std::max(1.65f,neighbouringDecay.load())*(1.f-enclosedMix)+enclosedMix*decayEnvelope*.001f);
    float sizeTarget=std::clamp((decay-430)/1250.f,0.f,1.f);
    float lateInput[frames]{}, lateOutput[frames]{};
    auto stretchedTap=[&](const float* line,unsigned at,unsigned mask,unsigned n,int i){
        auto read=[&](int percent){return line[(at-(n*percent+50)/100)&mask];};
        float current=read(stretch);
        if(stretch==lastStretch)return current;
        float old=read(lastStretch);
        return old+(current-old)*float(i+1)/frames;
    };
    static float sizeMix=0,skyBlend=0,urbanGain=1,city=0;
    city+=.045f*(cityReturn.load()-city);
    skyBlend+=.1f*(openSky.load()-skyBlend);
    urbanGain+=.05f*(1.f+streetGain.load()*(streetBoost.load()-1.f)-urbanGain);
    for (int i=0;i<frames;i++) {
        level+=(target-level)*0.00009f;
        float send=input[i]*level;
        diffuse+=.00009f*(diffuseTarget-diffuse);
        covered+=.00009f*(coverTarget-covered);
        axialMix+=.00009f*(axialTarget-axialMix);
        early[0][head]=send-.75f*level*stereoSide[i];
        early[1][head]=send+.75f*level*stereoSide[i];
        float wall[2];
        for (int ch=0;ch<2;ch++) {
            // Fixed read offsets keep each return at the original pitch. Only
            // the amplitudes change as the room estimate changes.
            wallGain[ch]+=.00010f*(gains[ch]-wallGain[ch]);
            farMix[ch]+=.00008f*(farTarget[ch]-farMix[ch]);
            auto tap=[&](unsigned n) { return stretchedTap(early[ch],head,16383,n,i); };
            const unsigned spread=ch*47;
            float close=.62f*tap(349+spread)+.38f*tap(719+spread);
            float distant=.58f*tap(1777+spread)+.42f*tap(3083+spread);
            float gain=wallGain[ch];
            float mix=farMix[ch];
            wall[ch]=gain*(close+(distant-close)*mix);
            float shortAxis=tap(1597+ch*103),longAxis=tap(3863+ch*193);
            float axis=shortAxis+(longAxis-shortAxis)*axialMix;
            wall[ch]+=.48f*covered*axis;
            wall[ch]+=corridorMix*(.40f*axis+.25f*tap(4871+ch*107));
        }
        head=(head+1)&16383;
        float excite=diffuse*(.90f*send+.15f*(wall[0]+wall[1]));
        sizeMix+=0.00022f*(sizeTarget-sizeMix);
        float l,r,longL,longR;
        room.tick(excite*(1-sizeMix),l,r);
        if (steam.effect) {
            lateInput[i]=excite*(.4f+.6f*sizeMix);
            l*=.6f;r*=.6f;
            longL=longR=0;
        } else longRoom.tick(excite*sizeMix,longL,longR);
        l+=longL;r+=longR;
        // Reinforce the bass already present in the late return.
        bassReturn[0]+=.035f*(l-bassReturn[0]);
        bassReturn[1]+=.035f*(r-bassReturn[1]);
        l+=.50f*covered*bassReturn[0];
        r+=.50f*covered*bassReturn[1];
        echo[2*i]=(1.f-skyBlend+.42f*city)*wall[0]+blockWet*arrivals[2*i];echo[2*i+1]=(1.f-skyBlend+.42f*city)*wall[1]+blockWet*arrivals[2*i+1];
        wetBlock[i*2]=l;
        wetBlock[i*2+1]=r;
    }
    if (steam.effect) steam.process(lateInput,lateOutput,frames,duration*decayEnvelope*.001f,covered,soft,lowTime,highTime);
    for (int i=0;i<frames;i++) {
        for (int ch=0;ch<2;ch++) {
            auto& sample=wetBlock[2*i+ch];
            sample+=lateOutput[i];
            hallBass[ch]+=.032f*(sample-hallBass[ch]);
            sample+=(hall*.80f+.60f*city)*hallBass[ch];
            for(int b=0;b<2;b++){
                auto& x=b==0?echo[2*i+ch]:sample;
                x=tone[b][ch].tick(x,1.f);
                low[b][ch]+=.48f*(x-low[b][ch]);
                x=low[b][ch]+high*(x-low[b][ch]);
            }
        }
    }
    for(int ch=0;ch<2;ch++){
        float excitation[frames],returned[frames]{};
        for(int i=0;i<frames;i++){
            unsigned at=(tapHead+i)&8191;
            float earlyReturn=.34f*stretchedTap(taps[ch],at,8191,735,i)+.24f*stretchedTap(taps[ch],at,8191,1821,i);
            taps[ch][at]=roomIn[2*i+ch]*blockWet;
            excitation[i]=.55f*taps[ch][(at-384)&8191];
            echo[2*i+ch]+=(1.f-skyBlend+.42f*city)*earlyReturn;
        }
        if(emit[ch].effect)emit[ch].process(excitation,returned,frames,emitterSeconds,.8f,soft,lowTime,highTime);
        else{
            emitterFallback[ch].setDecay(int(1000.f*emitterSeconds));
            for(int i=0;i<frames;i++){float l,r;emitterFallback[ch].tick(excitation[i],l,r);returned[i]=.5f*(l+r);}
        }
        for(int i=0;i<frames;i++){
            lateLow[ch]+=.32f*(returned[i]-lateLow[ch]);
            wetBlock[2*i+ch]+=lateLow[ch]+.32f*(returned[i]-lateLow[ch]);
        }
    }
    static float echoEnabled=1.f,reverbEnabled=1.f;
    const float reverbTarget=lateReverb.load()?1.f:0.f;
    const float echoTarget=wallEcho.load()?1.f:0.f;
    for(int i=0;i<frames;i++){
      reverbEnabled+=std::clamp(reverbTarget-reverbEnabled,-1.f/256,1.f/256);
      echoEnabled+=std::clamp(echoTarget-echoEnabled,-1.f/256,1.f/256);
      for(int ch=0;ch<2;ch++){
        int at=2*i+ch;
        const unsigned pos=(delayHead+i)&8191;
        auto delayedSample=[&](int bus,float sample,int offset,int oldOffset){
            auto& line=delayed[bus][ch];
            line[pos]=sample;
            float value=line[(pos-offset)&8191];
            if(offset==oldOffset)return value;
            float old=line[(pos-oldOffset)&8191];
            return old+(value-old)*float(i+1)/frames;
        };
        float earlySample=delayedSample(0,echo[at],echoOffset,lastEchoDelay);
        float lateSample=delayedSample(1,wetBlock[at],lateOffset,lastPreDelay);
        wetBlock[at]=echoEnabled*equalizer[0][ch].sample(earlySample,settings[0])+reverbEnabled*urbanGain*equalizer[1][ch].sample(lateSample,settings[1]);
    }
    }
    lastStretch=stretch;lastEchoDelay=echoOffset;lastPreDelay=lateOffset;
    delayHead=(delayHead+frames)&8191;
    tapHead=(tapHead+frames)&8191;
    const auto medium=immersion::settings(immersion::listener.load());
    for(unsigned i=0;i<frames;i+=immersion::block)
        listenerWater[Player].process(wetBlock.data()+2*i,medium);
    if(limiter.process(wetBlock,out) && logging.load())++limitedBlocks;
}

DWORD WINAPI worker(void*) {
    SteamReturn steam[2],emit[2][2];
    SoundField field[2];
    if(steam[0].start((pluginDirectory()+L"phonon.dll").c_str(),Room::rate,frames)){
        steam[1].startSibling(steam[0],Room::rate,frames);
        for(int group=0;group<2;group++)if(steam[group].effect){
            field[group].start(steam[group]);
            for(auto& emitter:emit[group])emitter.startSibling(steam[0],Room::rate,frames);
        }
    }
    sdkReverbReady=steam[0].effect!=nullptr;
    fieldReady=field[0].available;hrtfReady=field[0].hrtf!=nullptr;
    if(directfx::ready){
        for(auto& water:emitterWater)water.open(directfx::api,Room::rate,1);
        for(auto& water:listenerWater)water.open(directfx::api,Room::rate,2);
    }
    constexpr int blocks=8;
    WAVEFORMATEX format{WAVE_FORMAT_PCM,2,Room::rate,Room::rate*4,4,16,0};
    HANDLE event=CreateEventW(nullptr,FALSE,FALSE,nullptr);
    HWAVEOUT output{};
    WAVEHDR headers[blocks]{};short data[blocks][frames*2]{};
    int prepared=0,index=0;
    DWORD lastOpen=GetTickCount()-1000;
    auto closeFallback=[&](){
        if(!output)return;
        waveOutReset(output);
        for(int i=0;i<prepared;i++)waveOutUnprepareHeader(output,&headers[i],sizeof(WAVEHDR));
        waveOutClose(output);output=nullptr;prepared=0;index=0;
    };
    auto openFallback=[&](){
        if(!event)return false;
        auto error=waveOutOpen(&output,WAVE_MAPPER,&format,reinterpret_cast<DWORD_PTR>(event),0,CALLBACK_EVENT);
        if(error!=MMSYSERR_NOERROR){output=nullptr;outputError=error;return false;}
        for(;prepared<blocks;prepared++){
            auto& hdr=headers[prepared];
            hdr={};hdr.lpData=reinterpret_cast<char*>(data[prepared]);hdr.dwBufferLength=sizeof(data[0]);
            error=waveOutPrepareHeader(output,&hdr,sizeof(hdr));
            if(error!=MMSYSERR_NOERROR){outputError=error;closeFallback();return false;}
        }
        return true;
    };
    gameOutput::auxiliaryThread=GetCurrentThreadId();
    SetThreadPriority(GetCurrentThread(),THREAD_PRIORITY_ABOVE_NORMAL);
    ready=true;
    bool usedFallback=false,speechRendering=false;
    unsigned tail=0;
    for(;;){
        bool shared=gameOutput::live();
        unsigned queued{};
        if(shared){
            if(output){closeFallback();usedFallback=false;}
            queued=gameOutput::stems.size();
            if(queued>=gameOutput::fillGoal.load()){
                WaitForSingleObject(gameOutput::demand,16);continue;
            }
        }else{
            if(!output){
                AcquireSRWLockExclusive(&voiceLock);
                for(auto& voice:voices)if(voice.player)voice.player->setRoute(false);
                ReleaseSRWLockExclusive(&voiceLock);
                DWORD now=GetTickCount();
                if(now-lastOpen<1000){Sleep(20);continue;}
                lastOpen=now;
                if(!openFallback()){Sleep(20);continue;}
            }
            if(headers[index].dwFlags&WHDR_INQUEUE){WaitForSingleObject(event,16);continue;}
            for(auto& hdr:headers)if(hdr.dwFlags&WHDR_INQUEUE)queued+=frames;
            if(logging && usedFallback && !queued)++queueUnderruns;
        }
        short world[2*frames]{},playerWet[2*frames]{};
        float playerDry[2*frames]{};
        const auto state=healthAudio::snapshot();
        const bool protect=shared && gameOutput::hasSDK && !state.bypass && (state.amount>0.f || state.fade<1.f);
        render<false>(world,steam[0],emit[0],field[0],queued,nullptr,false);
        AcquireSRWLockShared(&voiceLock);
        bool speechActive=std::any_of(std::begin(voices),std::end(voices),[](const Voice& v){return v.buffer && v.isPlayer;});
        ReleaseSRWLockShared(&voiceLock);
        if(speechActive){
            float ms=std::max(1600.f,decayMs.load()*.0001f*decayPercent.load()*lowDecayPercent.load()+550.f);
            tail=unsigned(std::min(ms,45000.f)*32);
        }
        if(speechActive || tail || speechRendering){
            const bool clear=!speechActive && !tail;
            render<true>(playerWet,steam[1],emit[1],field[1],queued,playerDry,protect,clear);
            speechRendering=!clear;
            if(!speechActive)tail-=std::min<unsigned>(tail,frames);
        }
        if(shared){
            Stems packet[frames];
            for(int i=0;i<frames;i++)for(int c=0;c<2;c++){
                packet[i].world[c]=world[2*i+c]/32768.f;
                packet[i].player[c]=playerWet[2*i+c]/32768.f+playerDry[2*i+c];
            }
            gameOutput::stems.push(packet,frames);
        }else{
            for(int i=0;i<frames*2;i++)
                data[index][i]=short(std::clamp(int(world[i])+int(playerWet[i]),-32768,32767));
            auto error=waveOutWrite(output,&headers[index],sizeof(WAVEHDR));
            if(error!=MMSYSERR_NOERROR){outputError=error;closeFallback();usedFallback=false;continue;}
            usedFallback=true;index=(index+1)%blocks;
        }
    }

}

injury::Concussion concussion;
bool explosionHooks{};
unsigned blastHits{};
struct ExplosionDamage {
    uintptr_t player{};
    float nestedLoss{};
};
ExplosionDamage* activeExplosion{}; // Accessed only on the game thread.
void __cdecl explosionDamage(const Vec& point,float radius,float visible,void* victim,void* creator,bool bomb,float damage){
    using Trigger=void(__cdecl*)(const Vec&,float,float,void*,void*,bool,float);
    const auto original=reinterpret_cast<Trigger>(0x56B790);
    auto player=[](){return reinterpret_cast<uintptr_t(__cdecl*)(int)>(0x56E210)(-1);};
    if(GetCurrentThreadId()!=gameThread.load() || !concussion.enabled || !concussion.strength || healthAudio::excluded()){
        original(point,radius,visible,victim,creator,bomb,damage);return;
    }
    ExplosionDamage event{player()};
    if(!event.player){original(point,radius,visible,victim,creator,bomb,damage);return;}
    const auto vitality=[&](){return read<float>(event.player+0x540)+read<float>(event.player+0x548);};
    const float before=vitality();
    Vec position{};reinterpret_cast<void(__cdecl*)(Vec*,int)>(0x56E010)(&position,-1);
    const float metres=magnitude(difference(position,point));
    auto* parent=activeExplosion;
    activeExplosion=&event;
    original(point,radius,visible,victim,creator,bomb,damage);
    activeExplosion=parent;
    if(player()!=event.player)return;
    const float total=std::max(0.f,before-vitality());
    if(parent && parent->player==event.player)parent->nestedLoss+=total;
    const float loss=total-event.nestedLoss;
    if(loss>0 && metres<concussion.radius){
        concussion.hit(event.player,read<DWORD>(0xB7CB84),loss,metres);
        ++blastHits;
    }
}

int option(const wchar_t* group,const wchar_t* key,int def,int high,const wchar_t* oldGroup=nullptr){
    constexpr int missing=-32768;
    int value=int(GetPrivateProfileIntW(group,key,missing,iniPath.c_str()));
    if(value==missing && oldGroup)value=int(GetPrivateProfileIntW(oldGroup,key,def,iniPath.c_str()));
    return std::clamp(value==missing?def:value,0,high);
}
void writeStatus(bool force=false){
    if(!logging.load())return;
    static DWORD last{};
    DWORD now=GetTickCount();
    if(!force && now-last<5000)return;
    last=now;
    unsigned active=0,attached=0,speechCount=0,routes=0,blocked=0,submerged=0;
    for(unsigned i=0;i<64;i++){
        if(sources[i].id<0 || now-sources[i].seen>200)continue;
        ++active;attached+=directfx::attached[i].load();
        speechCount+=speech[i].load();routes+=sources[i].route>0.f;
        blocked+=sources[i].shadow>.01f;submerged+=immersion::source[i].load()>.01f;
    }
    unsigned echoes=0,entries=0;
    AcquireSRWLockShared(&sceneLock);
    for(unsigned i=0;i<64;i++){
        if(sources[i].id<0 || now-sources[i].seen>200)continue;
        for(auto path:sceneView.paths[i].wall)echoes+=path.gain>0;
        entries+=sceneView.paths[i].entryEcho.gain>0;
    }
    ReleaseSRWLockShared(&sceneLock);
    FILE* file=_wfopen(logPath.c_str(),L"a");
    if(!file)return;
    std::fprintf(file,"t=%lu area=%d stadium=%d stadiumModel=%d acousticsActive=%d outputReady=%d outputError=%u\n",
        now,currentArea,int(stadiumActive),stadiumModel,int(enclosure.load()>=0),int(ready.load()),outputError.load());
    std::fprintf(file,"  SDK directSimulation=%d directFactory=%d reverb=%d ambisonicsDecoder=%d HRTF=%d DopplerHook=%d radioHook=%d\n",
        int(live::started),int(directfx::ready.load() && directfx::activation!=INVALID_HANDLE_VALUE),int(sdkReverbReady.load()),
        int(fieldReady.load()),int(hrtfReady.load()),int(alternativePitch),int(radioHook));
    std::fprintf(file,"  voices active=%u SteamDirect=%u compatibility=%u speech=%u blocked=%u detours=%u wallReturns=%u entranceReturns=%u\n",
        active,attached,active-attached,speechCount,blocked,routes,echoes,entries);
    std::fprintf(file,"  effects echo=%d reverb=%d wet=%.2f decayMs=%d street=%.3f enclosure=%.3f corridor=%.3f ambisonics=%d headphones=%d\n",
        int(wallEcho.load()),int(lateReverb.load()),wet.load(),decayMs.load(),cityReturn.load(),
        enclosedAmount.load(),corridorAmount.load(),int(useField.load()),int(headphones.load()));
    std::fprintf(file,"  water enabled=%d listener=%.3f sources=%u radioFiltered=%d steamWater=%d radioChain=%d cutoffHz=%d lossDb=%d modulation=%d\n",
        int(waterEnabled),immersion::listener.load(),submerged,int(directfx::streamWet.load()),int(directfx::ready.load()),int(directfx::streamActive.load()),
        immersion::cutoff.load(),immersion::loss.load(),immersion::modulation.load());
    std::fprintf(file,"  concussion hook=%d hits=%u intensity=%.3f\n",int(explosionHooks),blastHits,concussion.value(read<DWORD>(0xB7CB84)));
    auto injury=healthAudio::snapshot();
    std::fprintf(file,"  lowHealth enabled=%d threshold=%d strength=%d amount=%.3f deathGain=%.3f bypass=%d outputMix=%d\n",
        int(healthAudio::enabled.load()),healthAudio::threshold.load(),healthAudio::strength.load(),
        injury.amount,injury.fade,int(injury.bypass),int(gameOutput::live()));
    std::fprintf(file,"  injuryMix endpointHook=%d clients=%u packets=%u steamTail=%d stemUnderruns=%u\n",
        int(gameOutput::hooked.load()),gameOutput::wrapped.load(),gameOutput::packets.load(),int(gameOutput::hasSDK.load()),gameOutput::underruns.load());
    std::fprintf(file,"  propagation materials=1 volumeSampling=1 routing=1 airAbsorption=1 directivity=1 directOcclusion=%d roomRadius=%.1f sourceRadius=%.1f fieldRadius=%d worldRange=%.2f speechRange=%.2f\n",
        int(directOcclusion),probeRadius,sourceRadius,fieldRadius.load(),rangeScale.load(),speechRangeScale.load());
    std::fprintf(file,"  mixer queueUnderruns=%u limitedBlocks=%u\n",queueUnderruns.load(),limitedBlocks.load());
    std::fclose(file);
}
void loadSettings(){
    bool wasLogging=logging.load();
    logging=option(L"General",L"Logging",0,1);
    if(logging && !wasLogging){
        logPath=iniPath.substr(0,iniPath.size()-4)+L".log";
        if(FILE* file=_wfopen(logPath.c_str(),L"w")){
            std::fputs("Rebound.SA 0.65\nStatus every 5 seconds. Counters are cumulative for this game session.\n",file);
            std::fclose(file);
        }
    }
    const wchar_t* sections[]={L"EchoEQ",L"ReverbEQ"};
    const wchar_t* bands[]={L"LowDb",L"MidDb",L"HighDb"};
    constexpr int toneDefaults[2][3]={{0,0,-6},{-6,0,0}};
    for(int b=0;b<2;b++)for(int k=0;k<3;k++){
        int db=std::clamp(int(GetPrivateProfileIntW(sections[b],bands[k],toneDefaults[b][k],iniPath.c_str())),-18,12);
        wetEq[b][k]=std::pow(10.f,db/20.f);
    }
    rangeScale=.01f*std::max(100,option(L"Spatial",L"WorldSoundRangePercent",200,300));
    speechRangeScale=.01f*std::max(25,option(L"Spatial",L"PedSpeechRangePercent",100,300));
    streetBoost=std::pow(10.f,.05f*option(L"Levels",L"StreetBoostDb",12,12,L"Reverb"));
    wet=.01f*option(L"Levels",L"WetPercent",100,150,L"Reverb");
    probeRadius=float(std::max(40,option(L"Propagation",L"RoomProbeRadius",120,200)));
    sourceRadius=float(std::max(50,option(L"Propagation",L"SourceRadius",400,600)));
    waterEnabled=option(L"Underwater",L"Enabled",1,1);
    healthAudio::enabled=option(L"LowHealth",L"Enabled",1,1);
    healthAudio::threshold=std::max(10,option(L"LowHealth",L"ThresholdPercent",15,50));
    healthAudio::strength=option(L"LowHealth",L"StrengthPercent",100,100);
    concussion.enabled=option(L"Concussion",L"Enabled",1,1);
    concussion.radius=float(std::max(1,option(L"Concussion",L"Radius",18,100)));
    concussion.fullDamage=float(std::max(1,option(L"Concussion",L"FullDamage",50,200)));
    concussion.strength=.01f*option(L"Concussion",L"StrengthPercent",100,100);
    concussion.recovery=float(std::max(1,option(L"Concussion",L"MaxDurationSeconds",8,30)));
    concussion.ringing=.01f*std::max(100,option(L"Concussion",L"RingingPercent",250,400));
    if(!concussion.enabled || !concussion.strength)concussion.clear();
    immersion::loss=option(L"Underwater",L"AttenuationDb",0,36);
    immersion::cutoff=std::max(100,option(L"Underwater",L"CutoffHz",420,3000));
    immersion::modulation=option(L"Underwater",L"ModulationPercent",12,35);
    echoStretch=std::max(25,option(L"Echo",L"StretchPercent",100,300));
    echoDelayMs=option(L"Echo",L"DelayMs",0,250);
    preDelayMs=option(L"Reverb",L"PreDelayMs",0,250);
    decayPercent=std::max(25,option(L"Reverb",L"DecayPercent",100,250));
    lowDecayPercent=std::max(50,option(L"Reverb",L"LowDecayPercent",100,200));
    highDecayPercent=std::max(50,option(L"Reverb",L"HighDecayPercent",100,200));
    tunnelBoostDb=option(L"Levels",L"TunnelBoostDb",12,18,L"Reverb");
    quietFloorGain=std::pow(10.f,-.05f*option(L"Levels",L"QuietContrastDb",18,48,L"Reverb"));
    farGainDb=option(L"Levels",L"DistantBoostDb",24,24,L"Reverb");
    speechMix=.01f*option(L"Levels",L"SpeechWetPercent",35,100,L"Reverb");
    interiors=option(L"General",L"Interiors",0,1,L"Reverb");

    wallEcho=option(L"Effects",L"Echo",option(L"Propagation",L"WallEcho",1,1),1);
    lateReverb=option(L"Effects",L"Reverb",1,1);

    directOcclusion=option(L"Propagation",L"DirectOcclusion",1,1);

    headphones=option(L"Spatial",L"Headphones",1,1);

    fieldRadius=std::max(25,option(L"Spatial",L"AmbisonicsRadius",240,600));
    useField=option(L"Spatial",L"Ambisonics",1,1);
    reloadKey=std::max(1,option(L"Controls",L"ReloadKey",119,254));
    writeStatus(true);
}
void frame() {
    gameThread=GetCurrentThreadId();
    priorFrame();
    static bool reloadHeld=false;
    DWORD pid=0;GetWindowThreadProcessId(GetForegroundWindow(),&pid);
    bool pressed=pid==GetCurrentProcessId() && (GetAsyncKeyState(reloadKey)&0x8000);
    if(pressed && !reloadHeld)loadSettings();
    reloadHeld=pressed;
    static bool started=false;
    static bool triedDirect=false;
    if(!triedDirect){
        triedDirect=true;
        const auto directory=pluginDirectory();
        directfx::start(directory);
        live::started=live::start(directory+L"phonon.dll");
    }
    if (!started) {
        HANDLE h=CreateThread(nullptr,0,worker,nullptr,0,nullptr);
        if (h) {CloseHandle(h);started=true;}
    }
    DWORD now=GetTickCount();lastFrame=now;static DWORD last=now-100;
    bool paused=read<BYTE>(0xBA67A4)||read<BYTE>(0xB7CB48)||read<BYTE>(0xB7CB49);
    auto ped=paused?uintptr_t{}:reinterpret_cast<uintptr_t(__cdecl*)(int)>(0x56E210)(-1);
    static injury::Life life;
    unsigned playerIndex=read<BYTE>(0xB7CD74);
    bool wasted=playerIndex<2 && read<BYTE>(0xB7CD98+0x190*playerIndex+0xdc)==1;
    float screenAlpha=read<float>(0xB6F028+0xbfc);
    float sfx=read<float>(hardware+0x414)*read<float>(hardware+0x41c);
    healthAudio::master=std::isfinite(sfx)?std::clamp(sfx,0.f,1.f):0.f;
    const bool excluded=paused || healthAudio::excluded();
    const float hp=ped?read<float>(ped+0x540):0.f;
    static bool wasDead=false;
    if(!paused){
        if((ped && concussion.owner!=ped) || (wasDead && ped && hp>0 && !wasted) || excluded)concussion.clear();
        wasDead=wasted || (ped && hp<=0);
    }
    const float impact=concussion.enabled?concussion.value(read<DWORD>(0xB7CB84)):0.f;
    const bool low=healthAudio::enabled.load() && healthAudio::strength.load()>0;
    auto state=life.update(ped!=0,hp,ped?read<float>(ped+0x544):0.f,wasted,screenAlpha,
        healthAudio::threshold,low?healthAudio::strength.load():100,low || impact>0,excluded);
    if(!low)state.amount=0;
    healthAudio::publish(concussion.mix(state,impact));
    float targetWater=0;
    if(waterEnabled && ped){
        targetWater=read<float>(0xC8132C)>=.5f?1.f:0.f;
        auto vehicle=reinterpret_cast<uintptr_t(__cdecl*)(int,bool)>(0x56E0D0)(-1,false);
        if(vehicle && (read<BYTE>(vehicle+0x41)&1)){
            Vec seat{};reinterpret_cast<void(__cdecl*)(Vec*,int)>(0x56E010)(&seat,-1);
            seat.z+=.45f;targetWater=std::max(targetWater,depthAt(seat));
        }
    }
    playerPed=ped;
    immersion::listener=targetWater;
    refreshStaticWater();
    currentArea=read<int>(0xB72914);
    stadiumActive=ped && inStadium(currentArea,now);
    if ((!interiors && currentArea!=0 && !stadiumActive) || !ped) {
        enclosure=-1;earValid=false;last=now-100;writeStatus();return;
    }
    if(alternativePitch)updateEar();
    if (now-last>=100) {
        last=now;enclosure=measure();
    }
    {
        constexpr uintptr_t camera=0xB6F028+0x974;
        Vec right=unit(read<Vec>(camera)),ahead=unit(read<Vec>(camera+16)),up=unit(read<Vec>(camera+32));
        if(magnitude(right)>.99f && magnitude(ahead)>.99f && magnitude(up)>.99f){
            AcquireSRWLockExclusive(&sceneLock);
            sceneView.camera={audioVector(right),audioVector(up),audioVector(ahead),{}};
            ReleaseSRWLockExclusive(&sceneLock);
        }
    }
    if (enclosure.load()>=0) updateSources(now);
    writeStatus();
}
bool matches(uintptr_t addr,const char* sig,size_t size) {
    MEMORY_BASIC_INFORMATION m{};
    auto p=reinterpret_cast<void*>(addr);
    return VirtualQuery(p,&m,sizeof(m)) && m.State==MEM_COMMIT && !(m.Protect&(PAGE_NOACCESS|PAGE_GUARD))
        && !memcmp(p,sig,size);
}
using GameDeviceCreate=HRESULT(WINAPI*)(LPCGUID,IDirectSound**,IUnknown*);
HRESULT WINAPI createGameDevice(LPCGUID device,IDirectSound** out,IUnknown* outer){
    gameOutput::arm(pluginDirectory()+L"phonon.dll");
    gameOutput::opening=true;
    auto original=read<GameDeviceCreate>(0x85801c);
    HRESULT h=original(device,out,outer);
    gameOutput::opening=false;
    return h;
}
void install(HMODULE mod) {
    constexpr uintptr_t audio=0x4f10af, tick=0x53e981;
    if (uintptr_t(GetModuleHandleW(nullptr))!=0x400000 ||
        !matches(audio,"\x6a\x00\x6a\x00\x50\xff\x51\x30",8) || !matches(tick,"\xe8",1) ||
        !matches(0x56E010,"\x8b\x44\x24\x08\x33\xd2",6) ||
        !matches(tick+5,"\xb9\x90\xbc\xb6\x00",5) || !matches(0x56ba00,"\x83\xec\x60\x66\x81\x3d\x78\xcd\xb7\x00",10)) return;
    for (uintptr_t site:volumeCalls) {
        if (!matches(site,"\xe8",1) || site+5+read<int32_t>(site+1)!=0x4D8870) return;
    }
    for(uintptr_t site:positionCalls)
        if(!matches(site,"\xe8",1) || site+5+read<int32_t>(site+1)!=0x4D8920)return;
    constexpr uintptr_t createSite=0x4F0DB6;
    if(!matches(createSite,"\x8b\x08\x50\xff\x51\x0c",6))return;
    wchar_t path[MAX_PATH];DWORD n=GetModuleFileNameW(mod,path,MAX_PATH);
    if (!n || n>=MAX_PATH || n<4) return;
    wcscpy(path+n-4,L".ini");
    iniPath=path;
    if(!option(L"General",L"Enabled",1,1,L"Reverb"))return;
    loadSettings();

    priorFrame=reinterpret_cast<decltype(priorFrame)>(tick+5+read<int32_t>(tick+1));
    auto a=reinterpret_cast<BYTE*>(audio),t=reinterpret_cast<BYTE*>(tick);
    DWORD oldA,oldT,tmp,oldV;
    auto volumePage=reinterpret_cast<void*>(volumeCalls[0]);
    if (!VirtualProtect(a,8,PAGE_EXECUTE_READWRITE,&oldA)) return;
    if (!VirtualProtect(volumePage,0x600,PAGE_EXECUTE_READWRITE,&oldV)) {VirtualProtect(a,8,oldA,&tmp);return;}
    if (VirtualProtect(t,5,PAGE_EXECUTE_READWRITE,&oldT)) {
        HMODULE pinned;
        if (GetModuleHandleExW(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS|GET_MODULE_HANDLE_EX_FLAG_PIN,
                               reinterpret_cast<LPCWSTR>(mod),&pinned)) {
            // Both original CALLs share one executable page. Install them together.
            constexpr uintptr_t first=0x7374DA, delayed=0x7376A4;
            if(matches(first,"\xe8\xb1\x42\xe3\xff\x83\xc4\x1c",8) &&
               matches(delayed,"\xe8\xe7\x40\xe3\xff\x83\xc4\x1c",8)){
                auto* page=reinterpret_cast<void*>(first);
                DWORD saved{};
                if(VirtualProtect(page,delayed+5-first,PAGE_EXECUTE_READWRITE,&saved)){
                    for(uintptr_t site:{first,delayed}){
                        uint32_t offset=uintptr_t(explosionDamage)-(site+5);
                        memcpy(reinterpret_cast<void*>(site+1),&offset,4);
                    }
                    FlushInstructionCache(GetCurrentProcess(),page,delayed+5-first);
                    VirtualProtect(page,delayed+5-first,saved,&tmp);explosionHooks=true;
                }
            }
            constexpr uintptr_t audioCreate=0x4d99f0;
            if(matches(audioCreate,"\xff\x15\x1c\x80\x85\x00\x85\xc0",8)){
                DWORD protection;auto* site=reinterpret_cast<BYTE*>(audioCreate);
                if(VirtualProtect(site,6,PAGE_EXECUTE_READWRITE,&protection)){
                    uint32_t rel=uintptr_t(createGameDevice)-(audioCreate+5);
                    site[0]=0xe8;memcpy(site+1,&rel,4);site[5]=0x90;
                    FlushInstructionCache(GetCurrentProcess(),site,6);
                    VirtualProtect(site,6,protection,&tmp);
                }
            }
            constexpr uintptr_t volumeSite=0x4f041a;
            if(matches(volumeSite,"\xe8\xf1\xf5\xff\xff",5) && matches(0x4efa5c,"\xd8\x76\x18",3)){
                DWORD old;
                auto operand=reinterpret_cast<void*>(volumeSite+1);
                if(VirtualProtect(operand,4,PAGE_EXECUTE_READWRITE,&old)){
                    uint32_t offset=uintptr_t(calculateWorldVolume)-volumeSite-5;
                    memcpy(operand,&offset,4);
                    FlushInstructionCache(GetCurrentProcess(),operand,4);
                    VirtualProtect(operand,4,old,&tmp);
                }
            }
            waterGeometry=matches(0x6eb690,"\x8b\x54\x24\x0c",4);
            constexpr uintptr_t streamCreate=0x4f238f;
            if(matches(streamCreate,"\x8b\x08\x50\xff\x51\x0c\x85\xc0",8)){
                DWORD protection;
                auto site=reinterpret_cast<BYTE*>(streamCreate);
                if(VirtualProtect(site,6,PAGE_EXECUTE_READWRITE,&protection)){
                    uint32_t rel=uintptr_t(createStreamBuffer)-(streamCreate+6);
                    site[0]=0x50;site[1]=0xe8;std::memcpy(site+2,&rel,4);
                    FlushInstructionCache(GetCurrentProcess(),site,6);
                    VirtualProtect(site,6,protection,&protection);radioHook=true;
                }
            }
            bool compatible=true;
            for(auto site:doorCalls)compatible &= matches(site,"\xe8",1) && site+5+read<int32_t>(site+1)==0x449ff0;
            DWORD oldDoor;
            auto doorPage=reinterpret_cast<void*>(doorCalls[0]);
            constexpr size_t span=doorCalls[3]+5-doorCalls[0];
            if(compatible && VirtualProtect(doorPage,span,PAGE_EXECUTE_READWRITE,&oldDoor)){
                for(auto site:doorCalls){
                    uint32_t displacement=uintptr_t(findPanels)-(site+5);
                    memcpy(reinterpret_cast<void*>(site+1),&displacement,4);
                }
                FlushInstructionCache(GetCurrentProcess(),doorPage,span);
                VirtualProtect(doorPage,span,oldDoor,&tmp);
            }
            {
                constexpr uintptr_t calls[]={0x4ef42d,0x4f06db,0x4f0936,0x4f09dd};
                void (*wrappers[])()={pitchESI,pitchEDI,pitchOffset,pitchOffset};
                bool original=true;
                for(auto site:calls)original &= matches(site,"\xe8",1) && site+5+read<int32_t>(site+1)==0x4d7e40;
                // Preserve protection per site, including the installer's outer page protection.
                DWORD previous[4]{};int writable=0;
                if(original)for(;writable<4;writable++){
                    if(!VirtualProtect(reinterpret_cast<void*>(calls[writable]),5,PAGE_EXECUTE_READWRITE,&previous[writable]))break;
                }
                if(writable==4){
                    for(int i=0;i<4;i++){
                        uint32_t rel=uintptr_t(wrappers[i])-calls[i]-5;
                        memcpy(reinterpret_cast<void*>(calls[i]+1),&rel,4);
                        FlushInstructionCache(GetCurrentProcess(),reinterpret_cast<void*>(calls[i]),5);
                    }
                    alternativePitch=true;
                }
                while(writable>0){--writable;VirtualProtect(reinterpret_cast<void*>(calls[writable]),5,previous[writable],&tmp);}
            }
            BYTE patch[]={0x56,0x50,0xe8,0,0,0,0,0x90};
            uint32_t disp=uintptr_t(play)-(audio+7);
            memcpy(patch+3,&disp,4);memcpy(a,patch,8);
            disp=uintptr_t(frame)-(tick+5);memcpy(t+1,&disp,4);
            for (uintptr_t site:volumeCalls) {
                disp=uintptr_t(sourceVolume)-(site+5);
                memcpy(reinterpret_cast<void*>(site+1),&disp,4);
            }
            for(uintptr_t site:positionCalls){
                disp=uintptr_t(sourcePosition)-(site+5);
                memcpy(reinterpret_cast<void*>(site+1),&disp,4);
            }
            {
                BYTE code[]={0x50,0xe8,0,0,0,0};
                disp=uintptr_t(createSpatial)-(createSite+6);
                memcpy(code+2,&disp,4);memcpy(reinterpret_cast<void*>(createSite),code,sizeof(code));
            }
            FlushInstructionCache(GetCurrentProcess(),volumePage,0x600);
            FlushInstructionCache(GetCurrentProcess(),a,8);FlushInstructionCache(GetCurrentProcess(),t,5);
        }
        VirtualProtect(t,5,oldT,&tmp);
    }
    VirtualProtect(volumePage,0x600,oldV,&tmp);
    VirtualProtect(a,8,oldA,&tmp);
}
}
BOOL WINAPI DllMain(HMODULE mod,DWORD reason,LPVOID) {
    if(reason==DLL_PROCESS_ATTACH) install(mod);
    return TRUE;
}
