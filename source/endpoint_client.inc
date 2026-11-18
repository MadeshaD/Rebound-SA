class Client final:public IAudioClient3 {
 std::atomic<ULONG> refs{1};IAudioClient*real;IAudioClient2*c2=nullptr;IAudioClient3*c3=nullptr;
 mixpcm::Format format;Render*render=nullptr;SRWLOCK lock=SRWLOCK_INIT;
 void Remember(const WAVEFORMATEX*w,DWORD flags){
  format=(flags&AUDCLNT_STREAMFLAGS_LOOPBACK)?mixpcm::Format{}:Parse(w);
 }
public:
 explicit Client(IAudioClient*r):real(r){real->QueryInterface(__uuidof(IAudioClient2),(void**)&c2);real->QueryInterface(__uuidof(IAudioClient3),(void**)&c3);}
 ~Client(){if(render)render->Release();if(c3)c3->Release();if(c2)c2->Release();real->Release();}
 HRESULT STDMETHODCALLTYPE QueryInterface(REFIID id,void**p)override{
  if(!p)return E_POINTER;
  *p=nullptr;
  if(id==__uuidof(IUnknown)||id==__uuidof(IAudioClient)||(id==__uuidof(IAudioClient2)&&c2)||(id==__uuidof(IAudioClient3)&&c3)){
   *p=static_cast<IAudioClient3*>(this);AddRef();return S_OK;
  }
  // Unknown/private interfaces retain native behavior but can bypass processing.
  return real->QueryInterface(id,p);
 }
 ULONG STDMETHODCALLTYPE AddRef()override{return ++refs;}
 ULONG STDMETHODCALLTYPE Release()override{ULONG n=--refs;if(!n)delete this;return n;}
 HRESULT STDMETHODCALLTYPE Initialize(AUDCLNT_SHAREMODE m,DWORD flags,REFERENCE_TIME duration,REFERENCE_TIME periodicity,const WAVEFORMATEX*w,LPCGUID session)override{
  HRESULT hr=real->Initialize(m,flags,duration,periodicity,w,session);if(SUCCEEDED(hr))Remember(w,flags);return hr;
 }
 HRESULT STDMETHODCALLTYPE GetBufferSize(UINT32*p)override{return real->GetBufferSize(p);}
 HRESULT STDMETHODCALLTYPE GetStreamLatency(REFERENCE_TIME*p)override{return real->GetStreamLatency(p);}
 HRESULT STDMETHODCALLTYPE GetCurrentPadding(UINT32*p)override{return real->GetCurrentPadding(p);}
 HRESULT STDMETHODCALLTYPE IsFormatSupported(AUDCLNT_SHAREMODE m,const WAVEFORMATEX*w,WAVEFORMATEX**p)override{return real->IsFormatSupported(m,w,p);}
 HRESULT STDMETHODCALLTYPE GetMixFormat(WAVEFORMATEX**p)override{return real->GetMixFormat(p);}
 HRESULT STDMETHODCALLTYPE GetDevicePeriod(REFERENCE_TIME*a,REFERENCE_TIME*b)override{return real->GetDevicePeriod(a,b);}
 HRESULT STDMETHODCALLTYPE Start()override{return real->Start();}
 HRESULT STDMETHODCALLTYPE Stop()override{return real->Stop();}
 HRESULT STDMETHODCALLTYPE Reset()override{
  HRESULT hr=real->Reset();
  if(SUCCEEDED(hr)){
   AcquireSRWLockShared(&lock);
   if(render)render->ResetDSP();
   ReleaseSRWLockShared(&lock);
  }
  return hr;
 }
 HRESULT STDMETHODCALLTYPE SetEventHandle(HANDLE h)override{return real->SetEventHandle(h);}
 HRESULT STDMETHODCALLTYPE GetService(REFIID id,void**p)override{
  if(!p)return E_POINTER;
  if(id!=__uuidof(IAudioRenderClient)||!format.valid())return real->GetService(id,p);
  AcquireSRWLockExclusive(&lock);
  if(!render){
   IAudioRenderClient*r=nullptr;HRESULT hr=real->GetService(id,(void**)&r);
   if(FAILED(hr)){ReleaseSRWLockExclusive(&lock);*p=nullptr;return hr;}
   render=new(std::nothrow)Render(r,format);
   if(!render){*p=r;ReleaseSRWLockExclusive(&lock);return S_OK;}
   ++wrapped;
  }
  render->AddRef();*p=static_cast<IAudioRenderClient*>(render);ReleaseSRWLockExclusive(&lock);return S_OK;
 }
 HRESULT STDMETHODCALLTYPE IsOffloadCapable(AUDIO_STREAM_CATEGORY a,BOOL*b)override{return c2?c2->IsOffloadCapable(a,b):E_NOINTERFACE;}
 HRESULT STDMETHODCALLTYPE SetClientProperties(const AudioClientProperties*p)override{return c2?c2->SetClientProperties(p):E_NOINTERFACE;}
 HRESULT STDMETHODCALLTYPE GetBufferSizeLimits(const WAVEFORMATEX*w,BOOL e,REFERENCE_TIME*a,REFERENCE_TIME*b)override{return c2?c2->GetBufferSizeLimits(w,e,a,b):E_NOINTERFACE;}
 HRESULT STDMETHODCALLTYPE GetSharedModeEnginePeriod(const WAVEFORMATEX*w,UINT32*a,UINT32*b,UINT32*c,UINT32*d)override{return c3?c3->GetSharedModeEnginePeriod(w,a,b,c,d):E_NOINTERFACE;}
 HRESULT STDMETHODCALLTYPE GetCurrentSharedModeEnginePeriod(WAVEFORMATEX**w,UINT32*p)override{return c3?c3->GetCurrentSharedModeEnginePeriod(w,p):E_NOINTERFACE;}
 HRESULT STDMETHODCALLTYPE InitializeSharedAudioStream(DWORD flags,UINT32 period,const WAVEFORMATEX*w,LPCGUID session)override{
  HRESULT hr=c3?c3->InitializeSharedAudioStream(flags,period,w,session):E_NOINTERFACE;if(SUCCEEDED(hr))Remember(w,flags);return hr;
 }
};
