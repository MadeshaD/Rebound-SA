#pragma once
// GTA uses the original DirectSound buffer methods and, on the same pointer,
// the extended Buffer8 methods. COM guarantees the identical vtable prefix;
// MinGW declares the two interfaces as separate C++ classes.
class BufferProxy:public IDirectSoundBuffer8 {
    std::atomic<ULONG> refs{1};
protected:
    IDirectSoundBuffer8* base;
    explicit BufferProxy(IDirectSoundBuffer8* real):base(real){}
    virtual ~BufferProxy(){base->Release();}
public:
    IDirectSoundBuffer* legacy(){
        return reinterpret_cast<IDirectSoundBuffer*>(static_cast<IDirectSoundBuffer8*>(this));
    }
    HRESULT STDMETHODCALLTYPE QueryInterface(REFIID id,void** out)override{
        if(!out)return E_POINTER;
        if(id==IID_IUnknown || id==IID_IDirectSoundBuffer || id==IID_IDirectSoundBuffer8){
            *out=static_cast<IDirectSoundBuffer8*>(this);AddRef();return S_OK;
        }
        return base->QueryInterface(id,out);
    }
    ULONG STDMETHODCALLTYPE AddRef()override{return ++refs;}
    ULONG STDMETHODCALLTYPE Release()override{ULONG n=--refs;if(!n)delete this;return n;}
    HRESULT STDMETHODCALLTYPE SetVolume(LONG v)override{return base->SetVolume(v);}
    HRESULT STDMETHODCALLTYPE GetVolume(LONG* v)override{return base->GetVolume(v);}
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
};
