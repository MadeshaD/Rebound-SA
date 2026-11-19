// In-place DirectSound effect. The game retains its playback clock, voice
// allocation, 3D position, pause, pitch and volume. No second dry output.
#include "sdk_direct.h"
namespace directfx {
constexpr GUID cls={0x597def5e,0x678b,0x4808,{0x9d,0x28,0xae,0x79,0x24,0x36,0xf4,0x36}};
constexpr GUID controlId={0xb4e10697,0xa0df,0x48e6,{0xb7,0x43,0x20,0xa9,0x88,0x9b,0xec,0x11}};
constexpr int block=64;
struct Controls {
    std::atomic<float> metres{},visibility{1.f};
    std::atomic<float> transmission[3]{{1.f},{1.f},{1.f}},air[3]{{1.f},{1.f},{1.f}};
    std::atomic<unsigned> generation{};
    void neutral(){
        metres=0.f;visibility=1.f;
        for(int k=0;k<3;k++){transmission[k]=1.f;air[k]=1.f;}
        ++generation;
    }
};
Controls controls[64];
std::atomic<bool> attached[64]{},waterAttached[64]{};
HANDLE activation=INVALID_HANDLE_VALUE;
immersion::DirectApi api;
std::atomic<bool> ready{},streamActive{},streamWet{};
constexpr int streamSlot=64,waterOnlyBase=65;
bool isAmbientStream(){
    // CAEAudioHardware's active streaming track ID in the validated 1.0 US layout.
    return *reinterpret_cast<volatile const LONG*>(0xB5F8B8+0xe14+0x34)==174;
}
struct Control: IUnknown {virtual HRESULT STDMETHODCALLTYPE Attach(int slot,DWORD rate)=0;};

class Effect final: public IMediaObject, public IMediaObjectInPlace, public Control {
    std::atomic<ULONG> refs{1};
    IPLDirectEffect effect{};
    int slot=-1,pos=0;
    DWORD rate=0;
    WAVEFORMATEX input{},output{};
    float samples[block]{},result[block]{},wall[3]{1,1,1},airGain[3]{1,1,1};
    float visibility=1.f;
    immersion::Transmission water;
    std::unique_ptr<immersion::Stream> radio;
    AirPath atmosphere;float distance=0;
    bool counted=false,waterOnly=false;
    unsigned generation{};
    HRESULT set(bool isInput,DWORD i,const DMO_MEDIA_TYPE* type,DWORD flags){
        if(i)return DMO_E_INVALIDSTREAMINDEX;
        auto& fmt=isInput?input:output;
        if(flags&DMO_SET_TYPEF_CLEAR){fmt={};return S_OK;}
        if(!type || type->majortype!=MEDIATYPE_Audio || type->formattype!=FORMAT_WaveFormatEx ||
           !type->pbFormat || type->cbFormat<16)return DMO_E_TYPE_NOT_ACCEPTED;
        WAVEFORMATEX f{};memcpy(&f,type->pbFormat,std::min<size_t>(type->cbFormat,sizeof(f)));
        bool pcm=f.wFormatTag==WAVE_FORMAT_PCM && f.wBitsPerSample==16 && type->subtype==MEDIASUBTYPE_PCM;
        bool fp=f.wFormatTag==WAVE_FORMAT_IEEE_FLOAT && f.wBitsPerSample==32 && type->subtype==MEDIASUBTYPE_IEEE_FLOAT;
        if(!(pcm||fp) || (f.nChannels!=1 && f.nChannels!=2) || f.nBlockAlign!=f.nChannels*(f.wBitsPerSample/8) ||
           f.nSamplesPerSec<8000 || f.nSamplesPerSec>192000 || (slot>=0 && (rate!=f.nSamplesPerSec || f.nChannels!=(slot==streamSlot?2:1))))return DMO_E_TYPE_NOT_ACCEPTED;
        auto& other=isInput?output:input;
        if(other.nChannels && (other.nSamplesPerSec!=f.nSamplesPerSec || other.wFormatTag!=f.wFormatTag || other.nChannels!=f.nChannels))return DMO_E_TYPE_NOT_ACCEPTED;
        if(!(flags&DMO_SET_TYPEF_TEST_ONLY)){f.cbSize=0;fmt=f;}
        return S_OK;
    }
    HRESULT current(const WAVEFORMATEX& fmt,DWORD i,DMO_MEDIA_TYPE* t){
        if(i)return DMO_E_INVALIDSTREAMINDEX;
        if(!t)return E_POINTER;
        if(!fmt.nChannels)return DMO_E_TYPE_NOT_SET;
        *t={};t->pbFormat=(BYTE*)CoTaskMemAlloc(sizeof(fmt));if(!t->pbFormat)return E_OUTOFMEMORY;
        memcpy(t->pbFormat,&fmt,sizeof(fmt));t->cbFormat=sizeof(fmt);t->majortype=MEDIATYPE_Audio;
        t->formattype=FORMAT_WaveFormatEx;t->subtype=fmt.wFormatTag==1?MEDIASUBTYPE_PCM:MEDIASUBTYPE_IEEE_FLOAT;
        t->bFixedSizeSamples=TRUE;t->lSampleSize=fmt.nBlockAlign;return S_OK;
    }
public:
    ~Effect(){if(effect)api.release(&effect);}
    HRESULT STDMETHODCALLTYPE Attach(int channel,DWORD sourceRate) override{
        if(channel<0 || channel>=waterOnlyBase+64 || !ready.load())return E_INVALIDARG;
        if(input.nChannels && input.nChannels!=(channel==streamSlot?2:1))return DMO_E_TYPE_NOT_ACCEPTED;
        rate=input.nChannels?input.nSamplesPerSec:sourceRate;
        if(channel==streamSlot){
            radio.reset(new(std::nothrow)immersion::Stream);
            if(!radio || !radio->open(api,rate)){radio.reset();return E_FAIL;}
        }else{
            IPLAudioSettings settings{int(rate),block};IPLDirectEffectSettings es{1};
            waterOnly=channel>=waterOnlyBase;
            if(!waterOnly && !effect && api.create(api.context,&settings,&es,&effect)!=IPL_STATUS_SUCCESS)return E_FAIL;
            if(!water.open(api,rate,1))return E_FAIL;
            atmosphere.configure(float(rate));
        }
        slot=waterOnly?channel-waterOnlyBase:channel;return S_OK;
    }
    HRESULT STDMETHODCALLTYPE QueryInterface(REFIID id,void** out) override{
        if(!out)return E_POINTER;
        *out=nullptr;
        if(id==IID_IUnknown || id==IID_IMediaObject)*out=static_cast<IMediaObject*>(this);
        else if(id==IID_IMediaObjectInPlace)*out=static_cast<IMediaObjectInPlace*>(this);
        else if(id==controlId)*out=static_cast<Control*>(this);
        else return E_NOINTERFACE;
        AddRef();return S_OK;
    }
    ULONG STDMETHODCALLTYPE AddRef() override{return ++refs;}
    ULONG STDMETHODCALLTYPE Release() override{auto n=--refs;if(!n)delete this;return n;}
    HRESULT STDMETHODCALLTYPE Process(ULONG size,BYTE* data,REFERENCE_TIME,DWORD flags) override{
        if(!data)return E_POINTER;
        if(!input.nBlockAlign || (!radio && !effect && !waterOnly))return S_OK;
        if(radio){
            const bool ambient=isAmbientStream();
            const float wet=immersion::listener.load();
            const unsigned n=size/input.nBlockAlign;
            if(input.wFormatTag==WAVE_FORMAT_IEEE_FLOAT)
                radio->process(reinterpret_cast<float*>(data),n,wet,ambient,flags==DMO_INPLACE_ZERO);
            else radio->process(reinterpret_cast<short*>(data),n,wet,ambient,flags==DMO_INPLACE_ZERO);
            streamWet=!ambient && wet>.005f;
            return S_OK;
        }
        const unsigned nextGeneration=controls[slot].generation.load();
        if(nextGeneration!=generation){
            Flush();
            generation=nextGeneration;
        }
        if(!counted && !waterOnly){
            const auto& control=controls[slot];
            visibility=sdkGain(control.visibility.load());
            for(int k=0;k<3;k++){
                wall[k]=sdkGain(control.transmission[k].load());
                airGain[k]=sdkGain(control.air[k].load());
            }
            distance=control.metres.load();atmosphere.update(distance);
            counted=true;
        }
        bool fp=input.wFormatTag==WAVE_FORMAT_IEEE_FLOAT;
        const auto medium=immersion::channel(slot);
        for(ULONG i=0;i<size/input.nBlockAlign;i++){
            samples[pos]=flags==DMO_INPLACE_ZERO?0.f:
                (fp?reinterpret_cast<float*>(data)[i]:reinterpret_cast<short*>(data)[i]/32768.f);
            float y=std::isfinite(result[pos])?result[pos]:0.f;
            if(fp)reinterpret_cast<float*>(data)[i]=y;
            else reinterpret_cast<short*>(data)[i]=short(std::clamp(y*32768.f,-32768.f,32767.f));
            if(++pos!=block)continue;
            if(waterOnly){
                std::copy_n(samples,block,result);water.process(result,medium);pos=0;continue;
            }
            auto follow=[this](float current,float target){
                target=sdkGain(target);
                float alpha=std::min(1.f,block/((target<current?.19f:.065f)*rate));
                return current+alpha*(target-current);
            };
            const auto& control=controls[slot];
            visibility=follow(visibility,control.visibility.load());
            for(int k=0;k<3;k++){
                wall[k]=follow(wall[k],control.transmission[k].load());
                airGain[k]=follow(airGain[k],control.air[k].load());
            }
            distance+=-std::expm1(-float(block)/(.07f*rate))*(control.metres.load()-distance);
            atmosphere.update(distance);
            auto p=sdkDirectParams(visibility,wall,airGain,rate);
            float* a=samples,*b=result;
            IPLAudioBuffer src{1,block,&a},dst{1,block,&b};
            api.apply(effect,&p,&src,&dst);
            for(auto& value:result)value=atmosphere.process(value);
            water.process(result,medium);
            pos=0;
        }
        return S_OK;
    }
    HRESULT STDMETHODCALLTYPE Flush() override{atmosphere.clear();water.clear();if(radio)radio->clear();pos=0;counted=false;std::fill_n(result,block,0.f);if(effect)api.reset(effect);return S_OK;}
    HRESULT STDMETHODCALLTYPE GetStreamCount(DWORD*a,DWORD*b) override{if(!a||!b)return E_POINTER;*a=*b=1;return S_OK;}
    HRESULT STDMETHODCALLTYPE GetInputStreamInfo(DWORD i,DWORD*f) override{if(i||!f)return E_INVALIDARG;*f=0;return S_OK;}
    HRESULT STDMETHODCALLTYPE GetOutputStreamInfo(DWORD i,DWORD*f) override{if(i||!f)return E_INVALIDARG;*f=0;return S_OK;}
    HRESULT STDMETHODCALLTYPE GetInputType(DWORD i,DWORD,DMO_MEDIA_TYPE*) override{return i?DMO_E_INVALIDSTREAMINDEX:DMO_E_NO_MORE_ITEMS;}
    HRESULT STDMETHODCALLTYPE GetOutputType(DWORD i,DWORD,DMO_MEDIA_TYPE*) override{return i?DMO_E_INVALIDSTREAMINDEX:DMO_E_NO_MORE_ITEMS;}
    HRESULT STDMETHODCALLTYPE SetInputType(DWORD i,const DMO_MEDIA_TYPE*t,DWORD f) override{return set(true,i,t,f);}
    HRESULT STDMETHODCALLTYPE SetOutputType(DWORD i,const DMO_MEDIA_TYPE*t,DWORD f) override{return set(false,i,t,f);}
    HRESULT STDMETHODCALLTYPE GetInputCurrentType(DWORD i,DMO_MEDIA_TYPE*t) override{return current(input,i,t);}
    HRESULT STDMETHODCALLTYPE GetOutputCurrentType(DWORD i,DMO_MEDIA_TYPE*t) override{return current(output,i,t);}
    HRESULT STDMETHODCALLTYPE GetInputSizeInfo(DWORD i,DWORD*s,DWORD*l,DWORD*a) override{if(i||!s||!l||!a)return E_INVALIDARG;*s=*a=input.nBlockAlign;*l=0;return *s?S_OK:DMO_E_TYPE_NOT_SET;}
    HRESULT STDMETHODCALLTYPE GetOutputSizeInfo(DWORD i,DWORD*s,DWORD*a) override{if(i||!s||!a)return E_INVALIDARG;*s=*a=output.nBlockAlign;return *s?S_OK:DMO_E_TYPE_NOT_SET;}
    HRESULT STDMETHODCALLTYPE GetInputMaxLatency(DWORD i,REFERENCE_TIME*t) override{return i?E_INVALIDARG:GetLatency(t);}
    HRESULT STDMETHODCALLTYPE SetInputMaxLatency(DWORD,REFERENCE_TIME) override{return E_NOTIMPL;}
    HRESULT STDMETHODCALLTYPE Discontinuity(DWORD i) override{return i?E_INVALIDARG:Flush();}
    HRESULT STDMETHODCALLTYPE AllocateStreamingResources() override{return S_OK;}
    HRESULT STDMETHODCALLTYPE FreeStreamingResources() override{return Flush();}
    HRESULT STDMETHODCALLTYPE GetInputStatus(DWORD i,DWORD*f) override{if(i||!f)return E_INVALIDARG;*f=DMO_INPUT_STATUSF_ACCEPT_DATA;return S_OK;}
    HRESULT STDMETHODCALLTYPE ProcessInput(DWORD,IMediaBuffer*,DWORD,REFERENCE_TIME,REFERENCE_TIME) override{return E_NOTIMPL;}
    HRESULT STDMETHODCALLTYPE ProcessOutput(DWORD,DWORD,DMO_OUTPUT_DATA_BUFFER*,DWORD*) override{return E_NOTIMPL;}
    HRESULT STDMETHODCALLTYPE Lock(LONG) override{return S_OK;}
    HRESULT STDMETHODCALLTYPE Clone(IMediaObjectInPlace**) override{return E_NOTIMPL;}
    HRESULT STDMETHODCALLTYPE GetLatency(REFERENCE_TIME*t) override{if(!t)return E_POINTER;*t=10000000LL*block/(rate?rate:44100);return S_OK;}
};
class Factory final: public IClassFactory {
public:
    HRESULT STDMETHODCALLTYPE QueryInterface(REFIID id,void** p) override{
        if(!p)return E_POINTER;
        *p=nullptr;
        if(id!=IID_IUnknown && id!=IID_IClassFactory)return E_NOINTERFACE;
        *p=this;AddRef();return S_OK;
    }
    ULONG STDMETHODCALLTYPE AddRef() override{return 2;}
    ULONG STDMETHODCALLTYPE Release() override{return 1;}
    HRESULT STDMETHODCALLTYPE LockServer(BOOL) override{return S_OK;}
    HRESULT STDMETHODCALLTYPE CreateInstance(IUnknown* outer,REFIID id,void** p) override{
        if(outer)return CLASS_E_NOAGGREGATION;
        auto e=new(std::nothrow) Effect;if(!e)return E_OUTOFMEMORY;
        HRESULT h=e->QueryInterface(id,p);e->Release();return h;
    }
};
bool openFactory(){
    HMODULE module{};
    if(!GetModuleHandleExW(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS |
                          GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT,
                          reinterpret_cast<LPCWSTR>(&openFactory),&module))return false;
    wchar_t path[32768]{};
    DWORD length=GetModuleFileNameW(module,path,32768);
    if(!length || length>=32768)return false;
    ACTCTXW a{};
    a.cbSize=sizeof(a);
    a.dwFlags=ACTCTX_FLAG_RESOURCE_NAME_VALID | ACTCTX_FLAG_HMODULE_VALID;
    a.lpSource=path;
    a.lpResourceName=MAKEINTRESOURCEW(101);
    a.hModule=module;
    activation=CreateActCtxW(&a);
    return activation!=INVALID_HANDLE_VALUE;
}
BOOL CALLBACK initializeMedia(PINIT_ONCE,void* argument,void**){
    // Audio callbacks and cached effects can refer to this context for the
    // lifetime of the pinned ASI. Initialization is outside the loader lock.
    auto directory=static_cast<const wchar_t*>(argument);
    HMODULE dll=LoadLibraryW((std::wstring(directory)+L"phonon.dll").c_str());
    if(!dll)return FALSE;
    auto bind=[dll](auto& fn,const char* name){
        auto p=GetProcAddress(dll,name);std::memcpy(&fn,&p,sizeof(fn));return p!=nullptr;
    };
    decltype(&iplContextCreate) newContext{};
    IPLContextSettings cs{};cs.version=STEAMAUDIO_VERSION;
    if(!bind(newContext,"_iplContextCreate@8") || !bind(api.create,"_iplDirectEffectCreate@16") ||
       !bind(api.apply,"_iplDirectEffectApply@16") || !bind(api.reset,"_iplDirectEffectReset@4") ||
       !bind(api.release,"_iplDirectEffectRelease@4") || newContext(&cs,&api.context)!=IPL_STATUS_SUCCESS){
        FreeLibrary(dll);return FALSE;
    }
    openFactory();ready=true;return TRUE;
}
void start(const std::wstring& directory){
    static INIT_ONCE init=INIT_ONCE_STATIC_INIT;
    InitOnceExecuteOnce(&init,initializeMedia,const_cast<wchar_t*>(directory.c_str()),nullptr);
}
// Original effects stay in their original order (the game addresses its two
// radio bass EQs by index). Nothing calls SetFX on an actively playing stream.
bool appendEffect(IDirectSoundBuffer8* buffer,int channel,DWORD rate,DWORD n,DSEFFECTDESC* native,DWORD* results){
    if(!ready.load() || activation==INVALID_HANDLE_VALUE || n>8 || (n && !native))return false;
    ULONG_PTR cookie{};
    if(!ActivateActCtx(activation,&cookie))return false;
    DSEFFECTDESC chain[9]{};
    DWORD status[9]{};
    if(n)std::copy_n(native,n,chain);
    chain[n].dwSize=sizeof(DSEFFECTDESC);chain[n].guidDSFXClass=cls;
    Control* control{};
    bool ok=buffer->SetFX(n+1,chain,status)==DS_OK &&
            SUCCEEDED(buffer->GetObjectInPath(cls,0,controlId,reinterpret_cast<void**>(&control)));
    if(ok){ok=SUCCEEDED(control->Attach(channel,rate));control->Release();}
    if(ok && n && results)std::copy_n(status,n,results);
    DeactivateActCtx(0,cookie);
    return ok;
}
bool install(IDirectSoundBuffer* b,int channel,DWORD rate){
    IDirectSoundBuffer8* ext{};
    if(FAILED(b->QueryInterface(IID_IDirectSoundBuffer8,reinterpret_cast<void**>(&ext))))return false;
    bool ok=appendEffect(ext,channel,rate,0,nullptr,nullptr);
    if(!ok)ext->SetFX(0,nullptr,nullptr);
    ext->Release();return ok;
}
}
