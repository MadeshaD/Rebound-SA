#pragma once
// Short or older mono buffers may reject an effects chain. This game-thread
// fallback takes a dry snapshot once. SDK rendering always occurs outside the
// native buffer lock, and surfacing copies back the exact original samples.
class StaticWater {
    IDirectSoundBuffer* owner{};
    uintptr_t bank{};
    // The first half is the original PCM. The second holds its fully submerged tone.
    std::unique_ptr<short[]> snapshot;
    DWORD savedBytes{};
    bool toneReady{};
    int lastWet{-1},lastShared{},lastHz{},lastLoss{},lastMod{};
    static bool transfer(IDirectSoundBuffer* buffer,short* samples,DWORD bytes,bool writing){
        void *a{},*b{};DWORD na{},nb{};
        if(FAILED(buffer->Lock(0,0,&a,&na,&b,&nb,DSBLOCK_ENTIREBUFFER)))return false;
        const bool valid=na<=bytes && nb==bytes-na && !(na%2 || nb%2) && (!na || a) && (!nb || b);
        if(valid){
            if(writing){if(na)std::memcpy(a,samples,na);if(nb)std::memcpy(b,samples+na/2,nb);}
            else {if(na)std::memcpy(samples,a,na);if(nb)std::memcpy(samples+na/2,b,nb);}
        }
        return SUCCEEDED(buffer->Unlock(a,na,b,nb)) && valid;
    }
public:
    ~StaticWater(){forget();}
    void forget(){
        if(!owner)return;
        owner->Release();
        owner=nullptr;bank=0;lastWet=-1;savedBytes=0;toneReady=false;snapshot.reset();
    }
    void update(IDirectSoundBuffer* buffer,uintptr_t soundBank,immersion::Settings s){
        if(owner && (owner!=buffer || bank!=soundBank))forget();
        if(!buffer)return;
        int level=int(std::lround(s.wet*8)),common=int(std::lround(s.shared*8));
        if(!level){
            if(owner && transfer(owner,snapshot.get(),savedBytes,true))forget();
            return;
        }
        const bool sameTone=toneReady && common==lastShared && s.cutoffHz==lastHz &&
                            s.lossDb==lastLoss && s.modulationPercent==lastMod;
        if(!directfx::ready || (sameTone && level==lastWet))return;
        DSBCAPS caps{};caps.dwSize=sizeof(caps);WAVEFORMATEX fmt{};
        if(FAILED(buffer->GetCaps(&caps)) || caps.dwBufferBytes<2 || caps.dwBufferBytes%2 || caps.dwBufferBytes>8*1024*1024 ||
           FAILED(buffer->GetFormat(&fmt,sizeof(fmt),nullptr)) || fmt.nChannels!=1 ||
           fmt.wFormatTag!=WAVE_FORMAT_PCM || fmt.wBitsPerSample!=16)return;
        // Only the game thread uses this workspace. Grow before locking a buffer.
        const DWORD bytes=caps.dwBufferBytes;
        const size_t samples=bytes/sizeof(short);
        if(!owner){
            auto copy=std::unique_ptr<short[]>(new(std::nothrow) short[2*samples]);
            if(!copy || !transfer(buffer,copy.get(),bytes,false))return;
            snapshot=std::move(copy);savedBytes=bytes;
            owner=buffer;owner->AddRef();bank=soundBank;
        }
        if(savedBytes!=bytes)return;
        short* dry=snapshot.get();
        short* submerged=dry+samples;
        if(!sameTone){
            static immersion::Transmission filter;
            if(!filter.open(directfx::api,fmt.nSamplesPerSec,1))return;
            filter.clear();s.wet=1.f;s.shared=common*.125f;
            for(size_t first=0;first<samples;first+=immersion::block){
                float input[immersion::block]{};
                const auto count=std::min<size_t>(immersion::block,samples-first);
                for(size_t i=0;i<count;i++)input[i]=dry[first+i]/32768.f;
                filter.process(input,s);
                for(size_t i=0;i<count;i++)submerged[first+i]=short(std::clamp(std::lrint(input[i]*32768.f),-32768L,32767L));
            }
            toneReady=true;lastWet=-1;lastShared=common;lastHz=s.cutoffHz;lastLoss=s.lossDb;lastMod=s.modulationPercent;
        }
        short* result=submerged;
        if(level<8){
            static std::unique_ptr<short[]> working;
            static size_t capacity{};
            if(samples>capacity){
                auto next=std::unique_ptr<short[]>(new(std::nothrow) short[samples]);
                if(!next)return;
                working=std::move(next);capacity=samples;
            }
            for(size_t i=0;i<samples;i++)working[i]=short((dry[i]*(8-level)+submerged[i]*level)/8);
            result=working.get();
        }
        if(transfer(buffer,result,bytes,true))lastWet=level;
    }
};
