#pragma once
// The game keeps its original timing/volume buffer. Only an identified CJ line
// needs an isolated dry stem, and only while the injury mix is running.
struct SpeechPlayback {
    SpeechRoute* route{};
    std::unique_ptr<directfx::Effect> direct;
    immersion::Filter water;
    double cursor{};
    float gain{},pan{};
    bool synced{},diverted{},available{true};
    int slot;
    explicit SpeechPlayback(SpeechRoute* control,int channel,bool nativeDirect):route(control),slot(channel){
        if(!nativeDirect)return;
        direct.reset(new(std::nothrow)directfx::Effect);
        if(!direct){available=false;return;}
        WAVEFORMATEX f{WAVE_FORMAT_IEEE_FLOAT,1,32000,128000,4,32,0};
        DMO_MEDIA_TYPE t{};t.majortype=MEDIATYPE_Audio;t.subtype=MEDIASUBTYPE_IEEE_FLOAT;t.formattype=FORMAT_WaveFormatEx;
        t.cbFormat=sizeof(f);t.pbFormat=reinterpret_cast<BYTE*>(&f);t.bFixedSizeSamples=TRUE;t.lSampleSize=4;
        available=SUCCEEDED(direct->SetInputType(0,&t,0)) && SUCCEEDED(direct->SetOutputType(0,&t,0)) &&
                  SUCCEEDED(direct->Attach(channel,32000));
    }
    ~SpeechPlayback(){if(diverted)route->Divert(FALSE);route->Release();}
    void setRoute(bool requested){
        requested&=available;
        if(diverted==requested)return;
        if(SUCCEEDED(route->Divert(requested))){
            diverted=requested;synced=false;gain=0;
            if(direct)direct->Flush();
            water={};
        }
    }
    void mix(IDirectSoundBuffer* buffer,const short* pcm,DWORD count,DWORD prefix,DWORD bufferFrames,
             DWORD originalRate,bool loop,bool intro,unsigned queued,float gamePan,float* stereo){
        constexpr unsigned n=256;
        DWORD status{},bytePos{},rate{};
        LONG level{},nativePan{};
        if(!pcm || count<=prefix || !bufferFrames || FAILED(buffer->GetStatus(&status)) ||
           FAILED(buffer->GetCurrentPosition(&bytePos,nullptr)) || FAILED(buffer->GetVolume(&level))){
            available=false;setRoute(false);return;
        }
        if(!(status&DSBSTATUS_PLAYING)){synced=false;return;}
        if(FAILED(buffer->GetFrequency(&rate)) || !rate)rate=originalRate;
        if(!rate || rate>200000){available=false;setRoute(false);return;}
        float targetPan=std::clamp(gamePan,-1.f,1.f);
        if(SUCCEEDED(buffer->GetPan(&nativePan)) && nativePan)
            targetPan=std::copysign(1.f-std::pow(10.f,-std::abs(nativePan)/2000.f),float(nativePan));
        double step=double(rate)/32000;
        const DWORD length=count-prefix;
        double actual=(bytePos/2)%bufferFrames;
        if(loop){
            if(intro && actual>=bufferFrames-prefix)actual-=bufferFrames-prefix;
            else actual=prefix+std::fmod(actual,double(length));
        }
        double target=actual+queued*step;
        if(loop && target>=count)target=prefix+std::fmod(target-prefix,double(length));
        double error=target-cursor;
        if(loop && cursor>=prefix && target>=prefix)error=std::remainder(error,double(length));
        if(!synced){cursor=actual;pan=targetPan;synced=true;}
        else if(std::abs(error)>rate*.10){cursor=target;gain=0;}
        else step*=1.+std::clamp(error/rate*.12,-.002,.002);
        float mono[n]{};
        auto sample=[&](DWORD k){
            if(k<count)return float(pcm[k]);
            return loop?float(pcm[prefix+(k-prefix)%length]):0.f;
        };
        for(float& v:mono){
            auto k=DWORD(cursor);float x=sample(k);
            v=(x+(sample(k+1)-x)*float(cursor-k))/32768.f;
            cursor+=step;
            if(loop && cursor>=count)cursor=prefix+std::fmod(cursor-prefix,double(length));
        }
        if(direct)direct->Process(sizeof(mono),reinterpret_cast<BYTE*>(mono),0,DMO_INPLACE_NORMAL);
        else{
            water.prepare(immersion::amount(slot),32000,immersion::sharedWater(slot));
            for(auto& v:mono)v=water.sample(v);
        }
        const float desired=level<=DSBVOLUME_MIN?0.f:std::pow(10.f,level/2000.f);
        for(unsigned i=0;i<n;i++){
            gain+=(desired-gain)*.012f;pan+=(targetPan-pan)*.005f;
            float x=mono[i]*gain;
            stereo[2*i]+=x*std::sqrt(1.f-std::max(pan,0.f));
            stereo[2*i+1]+=x*std::sqrt(1.f+std::min(pan,0.f));
        }
    }
};
