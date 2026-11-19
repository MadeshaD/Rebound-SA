#pragma once
#include "phonon.h"
#include <cstring>

// Each mixing path owns its effect and serializes processing and reset.
struct SteamReturn {
    SteamReturn()=default;
    SteamReturn(const SteamReturn&)=delete;
    SteamReturn& operator=(const SteamReturn&)=delete;
    HMODULE dll{};
    IPLContext context{};
    IPLReflectionEffect effect{};
    decltype(&iplContextRelease) releaseContext{};
    decltype(&iplContextRetain) retainContext{};
    decltype(&iplReflectionEffectCreate) createEffect{};
    decltype(&iplReflectionEffectRelease) releaseEffect{};
    decltype(&iplReflectionEffectReset) reset{};
    decltype(&iplReflectionEffectApply) apply{};

    template<typename Fn> bool bind(Fn& fn,const char* name) {
        auto symbol=GetProcAddress(dll,name);
        std::memcpy(&fn,&symbol,sizeof(fn));
        return fn!=nullptr;
    }
    bool createReturn(int rate,int frames){
        IPLAudioSettings audio{rate,frames};
        IPLReflectionEffectSettings settings{IPL_REFLECTIONEFFECTTYPE_PARAMETRIC,0,1};
        return context && createEffect(context,&audio,&settings,&effect)==IPL_STATUS_SUCCESS;
    }
    bool start(const wchar_t* path,int rate,int frames) {
        decltype(&iplContextCreate) createContext{};
        dll=LoadLibraryW(path);
        if (!dll || !bind(createContext,"_iplContextCreate@8") ||
            !bind(releaseContext,"_iplContextRelease@4") || !bind(retainContext,"_iplContextRetain@4") ||
            !bind(createEffect,"_iplReflectionEffectCreate@16") ||
            !bind(releaseEffect,"_iplReflectionEffectRelease@4") ||
            !bind(reset,"_iplReflectionEffectReset@4") ||
            !bind(apply,"_iplReflectionEffectApply@20")) return false;
        IPLContextSettings cs{};
        cs.version=STEAMAUDIO_VERSION;
        return createContext(&cs,&context)==IPL_STATUS_SUCCESS && createReturn(rate,frames);
    }
    bool startSibling(const SteamReturn& first,int rate,int frames){
        if(!first.context || !first.effect)return false;
        if(!GetModuleHandleExW(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS,
                               reinterpret_cast<LPCWSTR>(first.dll),&dll))return false;
        createEffect=first.createEffect;releaseEffect=first.releaseEffect;
        retainContext=first.retainContext;releaseContext=first.releaseContext;
        apply=first.apply;reset=first.reset;context=retainContext(first.context);
        return createReturn(rate,frames);
    }
    void process(float* input,float* output,int frames,float seconds,float cover,float soft,float lowTime,float highTime) {
        IPLReflectionEffectParams params{};
        params.type=IPL_REFLECTIONEFFECTTYPE_PARAMETRIC;
        params.numChannels=1;
        params.reverbTimes[0]=seconds*lowTime*(1.f+.23f*cover);
        params.reverbTimes[1]=seconds;
        params.reverbTimes[2]=seconds*highTime*(.53f-.12f*cover)*(1.f-.6f*soft);
        IPLAudioBuffer src{1,frames,&input},dst{1,frames,&output};
        apply(effect,&params,&src,&dst,nullptr);
    }
    template<int Frames> void clear(bool newScene){
        if(!effect)return;
        reset(effect);
        if(!newScene)return;
        // In SDK 4.8.1 a reset can leave a short remnant in the parametric
        // filters. Advance both filter banks with silence, then empty the delays.
        float silence[Frames]{},discard[Frames];
        for(int i=0;i<4;++i)process(silence,discard,Frames,.45f,0.f,0.f,1.f,1.f);
        reset(effect);
    }
    ~SteamReturn() {
        if (effect) releaseEffect(&effect);
        if (context) releaseContext(&context);
        if (dll) FreeLibrary(dll);
    }
};
