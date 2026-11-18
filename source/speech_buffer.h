#pragma once
#include "buffer_proxy.h"
// Only identifiable player dialogue uses this volume proxy. The real buffer
// keeps GTA's cursor, pause, Doppler and bank lifetime. Routing is reversible.
struct SpeechRoute: IUnknown {
    virtual HRESULT STDMETHODCALLTYPE Divert(BOOL)=0;
};
constexpr GUID speechRouteId={0xb506e2fa,0x71da,0x4c63,{0x83,0xae,0x39,0x65,0x96,0xcf,0xc0,0x85}};
class SpeechBuffer final:public BufferProxy,public SpeechRoute {
    SRWLOCK volumeLock=SRWLOCK_INIT;
    LONG requested{};
    unsigned diversions{};
public:
    SpeechBuffer(IDirectSoundBuffer8* real,LONG volume):BufferProxy(real),requested(volume){}
    HRESULT STDMETHODCALLTYPE QueryInterface(REFIID id,void** p)override{
        if(id!=speechRouteId)return BufferProxy::QueryInterface(id,p);
        if(!p)return E_POINTER;
        *p=static_cast<SpeechRoute*>(this);AddRef();return S_OK;
    }
    ULONG STDMETHODCALLTYPE AddRef()override{return BufferProxy::AddRef();}
    ULONG STDMETHODCALLTYPE Release()override{return BufferProxy::Release();}
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
    static void wrap(IDirectSoundBuffer** out){
        IDirectSoundBuffer8* real{};
        if(FAILED((*out)->QueryInterface(IID_IDirectSoundBuffer8,(void**)&real)))return;
        LONG volume{};
        if(FAILED(real->GetVolume(&volume))){real->Release();return;}
        auto* proxy=new(std::nothrow)SpeechBuffer(real,volume);
        if(!proxy){real->Release();return;}
        (*out)->Release();*out=proxy->legacy();
    }
};
