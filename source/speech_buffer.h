#pragma once
// Only identifiable player dialogue uses this volume proxy. The real buffer
// keeps GTA's cursor, pause, Doppler and bank lifetime. Routing is reversible.
struct SpeechRoute: IUnknown {
    virtual HRESULT STDMETHODCALLTYPE Divert(BOOL)=0;
};
constexpr GUID speechRouteId={0xb506e2fa,0x71da,0x4c63,{0x83,0xae,0x39,0x65,0x96,0xcf,0xc0,0x85}};
class SpeechBuffer final:public IDirectSoundBuffer,public IDirectSoundBuffer8,public SpeechRoute {
    IDirectSoundBuffer8* base;
    std::atomic<ULONG> refs{1};
    SRWLOCK volumeLock=SRWLOCK_INIT;
    LONG requested{};
    unsigned diversions{};
public:
    SpeechBuffer(IDirectSoundBuffer8* real,LONG volume):base(real),requested(volume){}
    ~SpeechBuffer(){base->Release();}
    HRESULT STDMETHODCALLTYPE QueryInterface(REFIID id,void** p)override{
        if(!p)return E_POINTER;
        if(id==IID_IUnknown || id==IID_IDirectSoundBuffer)*p=static_cast<IDirectSoundBuffer*>(this);
        else if(id==IID_IDirectSoundBuffer8)*p=static_cast<IDirectSoundBuffer8*>(this);
        else if(id==speechRouteId)*p=static_cast<SpeechRoute*>(this);
        else return base->QueryInterface(id,p);
        AddRef();return S_OK;
    }
    ULONG STDMETHODCALLTYPE AddRef()override{return ++refs;}
    ULONG STDMETHODCALLTYPE Release()override{auto n=--refs;if(!n)delete this;return n;}
    HRESULT STDMETHODCALLTYPE Divert(BOOL value)override{
        AcquireSRWLockExclusive(&volumeLock);
        HRESULT hr=DS_OK;
        const unsigned next=value?diversions+1:(diversions?diversions-1:0);
        if(bool(diversions)!=bool(next))hr=base->SetVolume(next?DSBVOLUME_MIN:requested);
        if(SUCCEEDED(hr))diversions=next;
        ReleaseSRWLockExclusive(&volumeLock);return hr;
    }
    HRESULT STDMETHODCALLTYPE SetVolume(LONG value)override{
        if(value<DSBVOLUME_MIN || value>DSBVOLUME_MAX)return DSERR_INVALIDPARAM;
        AcquireSRWLockExclusive(&volumeLock);
        HRESULT hr=diversions?DS_OK:base->SetVolume(value);
        if(SUCCEEDED(hr))requested=value;
        ReleaseSRWLockExclusive(&volumeLock);return hr;
    }
    HRESULT STDMETHODCALLTYPE GetVolume(LONG* value)override{
        if(!value)return E_POINTER;
        AcquireSRWLockShared(&volumeLock);*value=requested;ReleaseSRWLockShared(&volumeLock);return DS_OK;
    }
    HRESULT STDMETHODCALLTYPE GetCaps(DSBCAPS*p)override{return base->GetCaps(p);}
    HRESULT STDMETHODCALLTYPE GetCurrentPosition(DWORD*a,DWORD*b)override{return base->GetCurrentPosition(a,b);}
    HRESULT STDMETHODCALLTYPE GetFormat(WAVEFORMATEX*a,DWORD b,DWORD*c)override{return base->GetFormat(a,b,c);}
    HRESULT STDMETHODCALLTYPE GetPan(LONG*p)override{return base->GetPan(p);}
    HRESULT STDMETHODCALLTYPE GetFrequency(DWORD*p)override{return base->GetFrequency(p);}
    HRESULT STDMETHODCALLTYPE GetStatus(DWORD*p)override{return base->GetStatus(p);}
    HRESULT STDMETHODCALLTYPE Initialize(IDirectSound*a,LPCDSBUFFERDESC b)override{return base->Initialize(a,b);}
    HRESULT STDMETHODCALLTYPE Lock(DWORD a,DWORD b,void**c,DWORD*d,void**e,DWORD*f,DWORD g)override{return base->Lock(a,b,c,d,e,f,g);}
    HRESULT STDMETHODCALLTYPE Play(DWORD a,DWORD b,DWORD c)override{return base->Play(a,b,c);}
    HRESULT STDMETHODCALLTYPE SetCurrentPosition(DWORD n)override{return base->SetCurrentPosition(n);}
    HRESULT STDMETHODCALLTYPE SetFormat(LPCWAVEFORMATEX p)override{return base->SetFormat(p);}
    HRESULT STDMETHODCALLTYPE SetPan(LONG p)override{return base->SetPan(p);}
    HRESULT STDMETHODCALLTYPE SetFrequency(DWORD n)override{return base->SetFrequency(n);}
    HRESULT STDMETHODCALLTYPE Stop()override{return base->Stop();}
    HRESULT STDMETHODCALLTYPE Unlock(void*a,DWORD b,void*c,DWORD d)override{return base->Unlock(a,b,c,d);}
    HRESULT STDMETHODCALLTYPE Restore()override{return base->Restore();}
    HRESULT STDMETHODCALLTYPE SetFX(DWORD a,DSEFFECTDESC*b,DWORD*c)override{return base->SetFX(a,b,c);}
    HRESULT STDMETHODCALLTYPE AcquireResources(DWORD a,DWORD b,DWORD*c)override{return base->AcquireResources(a,b,c);}
    HRESULT STDMETHODCALLTYPE GetObjectInPath(REFGUID a,DWORD b,REFGUID c,void**d)override{return base->GetObjectInPath(a,b,c,d);}
    static void wrap(IDirectSoundBuffer** out){
        IDirectSoundBuffer8* real{};
        if(FAILED((*out)->QueryInterface(IID_IDirectSoundBuffer8,(void**)&real)))return;
        LONG volume{};
        if(FAILED(real->GetVolume(&volume))){real->Release();return;}
        auto* proxy=new(std::nothrow)SpeechBuffer(real,volume);
        if(!proxy){real->Release();return;}
        (*out)->Release();*out=proxy;
    }
};
