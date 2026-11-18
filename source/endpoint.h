#pragma once
#include <audioclient.h>
#include <mmdeviceapi.h>
#include <ks.h>
#include <ksmedia.h>
#include "mix_pcm.h"
#include "stem_queue.h"
#include "injury_steam.h"

namespace gameOutput {
inline StemQueue stems;
inline std::atomic<DWORD> heartbeat{},auxiliaryThread{};
inline std::atomic<bool> online{},opening{},hooked{},hasSDK{};
inline std::atomic<unsigned> wrapped{},packets{},underruns{};
inline std::atomic<unsigned> fillGoal{1024};
inline HANDLE demand{};
inline std::wstring sdkPath;
inline SRWLOCK consumer=SRWLOCK_INIT;
inline void* owner{};
inline bool live(){return online.load() && GetTickCount()-heartbeat.load()<240;}
inline void signal(){if(demand)SetEvent(demand);}
#include "endpoint_format.inc"
struct SdkTail {
    SteamReturn api;
    void clear(){if(api.effect)api.reset(api.effect);}
    void apply(float* in,float* out,injury::Decay decay){
        injury::applySteamBlock(api.effect,api.apply,in,out,decay);
    }
};
class Render final:public IAudioRenderClient {
    std::atomic<ULONG> refs{1};
    IAudioRenderClient* real;
    mixpcm::Format fmt;
    injury::Program program;
    SdkTail sdk;
    BYTE* data{};
    UINT32 capacity{};
    DWORD writtenFlags{};
    unsigned epoch{};
    bool processed{},primed{};
    void leave(){
        AcquireSRWLockExclusive(&consumer);
        if(owner==this){online=false;hasSDK=false;owner=nullptr;stems.discard();signal();}
        program.reset(sdk);primed=false;
        ReleaseSRWLockExclusive(&consumer);
    }
    DWORD process(UINT32 n,DWORD flags){
        if(!TryAcquireSRWLockExclusive(&consumer))return flags;
        DWORD now=GetTickCount();
        const bool expired=now-heartbeat.load()>=240;
        if(owner!=this || expired){
            if(owner && owner!=this && !expired){ReleaseSRWLockExclusive(&consumer);return flags;}
            owner=this;stems.discard();primed=false;program.reset(sdk);
        }
        fillGoal=std::clamp(unsigned(std::ceil(n*(32000./fmt.rate)))+512u,1024u,12000u);
        heartbeat=now;hasSDK=sdk.api.effect!=nullptr;online=true;++packets;signal();
        auto state=healthAudio::snapshot();
        unsigned nextEpoch=healthAudio::stateVersion.load();
        if(nextEpoch!=epoch){program.reset(sdk);stems.discard();primed=false;epoch=nextEpoch;}
        if(!sdk.api.effect)state={};
        program.prepare(state,fmt.channels,sdk);
        if(!primed && stems.size()>=512)primed=true;
        bool missing=false,audible=false;
        const bool silent=bool(flags&AUDCLNT_BUFFERFLAGS_SILENT);
        const float master=healthAudio::master.load();
        const double step=32000./fmt.rate;
        const unsigned stride=fmt.bytes();
        for(unsigned i=0;i<n;i++){
            Stems s{};
            if(primed && !stems.pop(step,s)){primed=false;missing=true;}
            bool aux=s.world[0]||s.world[1]||s.player[0]||s.player[1];
            if(!aux && !program.running() && (state.bypass || state.fade==1.f)){
                // A silent WASAPI packet can contain undefined bytes. Any packet
                // promoted to audible must be completely initialized.
                if(silent)for(unsigned c=0;c<fmt.channels;c++)mixpcm::write(data+(i*fmt.channels+c)*stride,fmt,0);
                continue;
            }
            float mix[8]{},cj[8]{};
            for(unsigned c=0;c<fmt.channels;c++){
                auto* p=data+(i*fmt.channels+c)*stride;
                mix[c]=silent?0.f:mixpcm::read(p,fmt);
                if(fmt.channels==1){mix[c]+=.5f*(s.world[0]+s.world[1]);cj[c]=.5f*(s.player[0]+s.player[1]);}
                else if(c<2){mix[c]+=s.world[c];cj[c]=s.player[c];}
            }
            program.sample(mix,cj,fmt.channels,fmt.lfe,state,master,sdk);
            for(unsigned c=0;c<fmt.channels;c++){
                mixpcm::write(data+(i*fmt.channels+c)*stride,fmt,mix[c]);
                audible|=std::abs(mix[c])>1.e-8f;
            }
        }
        if(missing)++underruns;
        signal();ReleaseSRWLockExclusive(&consumer);
        return silent && audible?flags&~AUDCLNT_BUFFERFLAGS_SILENT:flags;
    }
public:
    Render(IAudioRenderClient* r,mixpcm::Format f):real(r),fmt(f){
        program.configure(f.rate);
        if(!sdkPath.empty() && sdk.api.start(sdkPath.c_str(),f.rate,injury::sdkFrames)){
            std::array<float,injury::sdkFrames> in{},out{};
            sdk.apply(in.data(),out.data(),injury::decay(.5f));sdk.clear();
        }
    }
    ~Render(){leave();real->Release();}
    HRESULT STDMETHODCALLTYPE QueryInterface(REFIID id,void** p)override{
        if(!p)return E_POINTER;
        *p=nullptr;
        if(id!=IID_IUnknown && id!=__uuidof(IAudioRenderClient))return E_NOINTERFACE;
        *p=static_cast<IAudioRenderClient*>(this);AddRef();return S_OK;
    }
    ULONG STDMETHODCALLTYPE AddRef()override{return ++refs;}
    ULONG STDMETHODCALLTYPE Release()override{auto n=--refs;if(!n)delete this;return n;}
    HRESULT STDMETHODCALLTYPE GetBuffer(UINT32 n,BYTE** p)override{
        HRESULT h=real->GetBuffer(n,p);
        if(SUCCEEDED(h)){data=n && p?*p:nullptr;capacity=n;processed=false;}
        return h;
    }
    HRESULT STDMETHODCALLTYPE ReleaseBuffer(UINT32 n,DWORD flags)override{
        if(n && n<=capacity && data && (flags&~DWORD(AUDCLNT_BUFFERFLAGS_SILENT))==0){
            if(!processed){writtenFlags=process(n,flags);processed=true;}
            flags=writtenFlags;
        }
        HRESULT h=real->ReleaseBuffer(n,flags);
        if(SUCCEEDED(h)){data=nullptr;capacity=0;processed=false;}
        return h;
    }
    void ResetDSP(){leave();data=nullptr;capacity=0;processed=false;}
};
#include "endpoint_client.inc"
using ActivateFn=HRESULT (STDMETHODCALLTYPE*)(IMMDevice*,REFIID,DWORD,PROPVARIANT*,void**);
inline ActivateFn original{};
inline bool gameCaller(){
    if(GetCurrentThreadId()==auxiliaryThread.load())return false;
    if(opening.load())return true;
    // Device recovery can occur later on the DirectSound thread. Front-end
    // waveOut and other audio clients in the process must remain outside.
    void* stack[16]{};
    USHORT n=CaptureStackBackTrace(1,16,stack,nullptr);
    auto ds=GetModuleHandleW(L"dsound.dll"),eax=GetModuleHandleW(L"eax.dll");
    for(USHORT i=0;i<n;i++){
        MEMORY_BASIC_INFORMATION m{};
        if(VirtualQuery(stack[i],&m,sizeof m) && (m.AllocationBase==ds || m.AllocationBase==eax))return true;
    }
    return false;
}
inline HRESULT STDMETHODCALLTYPE activate(IMMDevice* dev,REFIID id,DWORD ctx,PROPVARIANT* args,void** out){
    auto h=original(dev,id,ctx,args,out);
    if(FAILED(h) || !out || !*out || (id!=__uuidof(IAudioClient) && id!=__uuidof(IAudioClient2) && id!=__uuidof(IAudioClient3)) || !gameCaller())return h;
    IMMEndpoint* endpoint{};EDataFlow flow=eCapture;
    if(SUCCEEDED(dev->QueryInterface(__uuidof(IMMEndpoint),(void**)&endpoint))){endpoint->GetDataFlow(&flow);endpoint->Release();}
    if(flow!=eRender)return h;
    auto* requested=static_cast<IUnknown*>(*out);IAudioClient* base{};
    if(FAILED(requested->QueryInterface(__uuidof(IAudioClient),(void**)&base)))return h;
    auto* client=new(std::nothrow)Client(base);
    if(!client){base->Release();return h;}
    void* replacement{};
    auto hr=client->QueryInterface(id,&replacement);client->Release();
    if(SUCCEEDED(hr)){requested->Release();*out=replacement;}
    return h;
}
inline void arm(const std::wstring& path){
    // Called by the game's device creation on a COM-initialized thread.
    if(hooked)return;
    sdkPath=path;
    if(!demand)demand=CreateEventW(nullptr,FALSE,FALSE,nullptr);
    if(!demand)return;
    IMMDeviceEnumerator* e{};IMMDevice* d{};
    if(FAILED(CoCreateInstance(__uuidof(MMDeviceEnumerator),nullptr,CLSCTX_INPROC_SERVER,__uuidof(IMMDeviceEnumerator),(void**)&e)))return;
    if(SUCCEEDED(e->GetDefaultAudioEndpoint(eRender,eMultimedia,&d))){
        auto entry=(*reinterpret_cast<void***>(d))+3;HMODULE keep{};DWORD old{};
        if(GetModuleHandleExW(GET_MODULE_HANDLE_EX_FLAG_PIN|GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS,reinterpret_cast<LPCWSTR>(entry),&keep) &&
           VirtualProtect(entry,sizeof(*entry),PAGE_READWRITE,&old)){
            original=reinterpret_cast<ActivateFn>(*entry);
            hooked=InterlockedCompareExchangePointer(entry,reinterpret_cast<void*>(&activate),reinterpret_cast<void*>(original))==reinterpret_cast<void*>(original);
            DWORD tmp;VirtualProtect(entry,sizeof(*entry),old,&tmp);
        }
        d->Release();
    }
    e->Release();
}
}
