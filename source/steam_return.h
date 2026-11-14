#pragma once
#include "phonon.h"
#include <cstring>

// All calls, including destruction, run on the waveOut worker.
struct SteamReturn {
    HMODULE dll{};
    IPLContext context{};
    IPLReflectionEffect effect{};
    decltype(&iplContextCreate) createContext{};
    decltype(&iplContextRelease) releaseContext{};
    decltype(&iplReflectionEffectCreate) createEffect{};
    decltype(&iplReflectionEffectRelease) releaseEffect{};
    decltype(&iplReflectionEffectReset) reset{};
    decltype(&iplReflectionEffectApply) apply{};

    template<typename Fn> bool bind(Fn& fn,const char* name) {
        auto symbol=GetProcAddress(dll,name);
        std::memcpy(&fn,&symbol,sizeof(fn));
        return fn!=nullptr;
    }
    bool start(const wchar_t* path,int rate,int frames) {
        dll=LoadLibraryW(path);
        if (!dll || !bind(createContext,"_iplContextCreate@8") ||
            !bind(releaseContext,"_iplContextRelease@4") ||
            !bind(createEffect,"_iplReflectionEffectCreate@16") ||
            !bind(releaseEffect,"_iplReflectionEffectRelease@4") ||
            !bind(reset,"_iplReflectionEffectReset@4") ||
            !bind(apply,"_iplReflectionEffectApply@20")) return false;
        IPLContextSettings cs{};
        cs.version=STEAMAUDIO_VERSION;
        IPLAudioSettings audio{rate,frames};
        IPLReflectionEffectSettings settings{IPL_REFLECTIONEFFECTTYPE_PARAMETRIC,0,1};
        return createContext(&cs,&context)==IPL_STATUS_SUCCESS &&
               createEffect(context,&audio,&settings,&effect)==IPL_STATUS_SUCCESS;
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
    ~SteamReturn() {
        if (effect) releaseEffect(&effect);
        if (context) releaseContext(&context);
        if (dll) FreeLibrary(dll);
    }
};
