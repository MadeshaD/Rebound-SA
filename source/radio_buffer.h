#pragma once
#include "buffer_proxy.h"

class RadioBuffer final:public BufferProxy {
    DWORD rate;
    explicit RadioBuffer(IDirectSoundBuffer8* real,DWORD hz):BufferProxy(real),rate(hz){}
public:
    HRESULT STDMETHODCALLTYPE SetFX(DWORD n,DSEFFECTDESC* fx,DWORD* result)override{
        const bool attached=directfx::appendEffect(base,directfx::streamSlot,rate,n,fx,result);
        directfx::streamActive=attached;
        if(attached)return DS_OK;
        directfx::streamWet=false;
        return base->SetFX(n,fx,result);
    }
    static void wrap(IDirectSoundBuffer** out,DWORD hz){
        IDirectSoundBuffer8* real{};
        if(FAILED((*out)->QueryInterface(IID_IDirectSoundBuffer8,reinterpret_cast<void**>(&real))))return;
        auto proxy=new(std::nothrow)RadioBuffer(real,hz);
        if(!proxy){real->Release();return;}
        proxy->SetFX(0,nullptr,nullptr);
        (*out)->Release();*out=proxy->legacy();
    }
};
