#pragma once

// Keep DirectSound as the sole renderer of each direct voice.
std::atomic<bool> headphones{true};
std::atomic<int> airDistance[64]{},emissionDb[64]{},spatialVoice[64]{};

struct ReleaseEq {
    void operator()(IDirectSoundFXParamEq* p)const {p->Release();}
};
using AirEq=std::unique_ptr<IDirectSoundFXParamEq,ReleaseEq>;

AirEq getAirEq(IDirectSoundBuffer* buffer){
    IDirectSoundBuffer8* extended=nullptr;
    IDirectSoundFXParamEq* eq=nullptr;
    if(SUCCEEDED(buffer->QueryInterface(IID_IDirectSoundBuffer8,reinterpret_cast<void**>(&extended)))){
        extended->GetObjectInPath(GUID_DSFX_STANDARD_PARAMEQ,0,IID_IDirectSoundFXParamEq,reinterpret_cast<void**>(&eq));
        extended->Release();
    }
    return AirEq(eq);
}

bool addAirEq(IDirectSoundBuffer* buffer){
    IDirectSoundBuffer8* extended=nullptr;
    if(FAILED(buffer->QueryInterface(IID_IDirectSoundBuffer8,reinterpret_cast<void**>(&extended))))return false;
    DSEFFECTDESC fx{};fx.dwSize=sizeof(fx);fx.guidDSFXClass=GUID_DSFX_STANDARD_PARAMEQ;
    DWORD result{};
    bool ok=SUCCEEDED(extended->SetFX(1,&fx,&result));
    extended->Release();
    if(ok){
        auto eq=getAirEq(buffer);
        DSFXParamEq neutral{8000.f,24.f,0.f};
        ok=eq && SUCCEEDED(eq->SetAllParameters(&neutral));
    }
    return ok;
}
