// Geometry is published by the game thread. The mixer copies it once per block.
struct SceneView {
    EarlyPaths paths[64]{};
    Vec directions[64]{};
    IPLCoordinateSpace3 camera{{1,0,0},{0,1,0},{0,0,-1},{}};
};
SceneView sceneView;
SRWLOCK sceneLock=SRWLOCK_INIT;
std::atomic<bool> useField{true};
std::atomic<int> fieldRadius{240};

IPLVector3 audioVector(Vec v){return {v.x,v.z,-v.y};}
Vec unit(Vec v){
    float n=magnitude(v);
    return n>.001f && std::isfinite(n)?Vec{v.x/n,v.y/n,v.z/n}:Vec{};
}
struct SoundField {
    float basis[4][4]{},previous[64][4]{};
    decltype(&iplAmbisonicsDecodeEffectReset) resetDecoder{};
    IPLAmbisonicsDecodeEffect dec{};
    IPLHRTF hrtf{};
    decltype(&iplAmbisonicsEncodeEffectApply) encode{};
    decltype(&iplAmbisonicsDecodeEffectApply) decode{};
    decltype(&iplAmbisonicsEncodeEffectRelease) freeEnc{};
    decltype(&iplAmbisonicsDecodeEffectRelease) freeDec{};
    decltype(&iplHRTFRelease) freeHrtf{};
    float mix[4][frames]{};
    bool available{};
    bool start(SteamReturn& sdk){
        decltype(&iplHRTFCreate) newHrtf{};
        decltype(&iplAmbisonicsEncodeEffectCreate) newEnc{};
        decltype(&iplAmbisonicsDecodeEffectCreate) newDec{};
        if(!sdk.bind(newHrtf,"_iplHRTFCreate@16") || !sdk.bind(freeHrtf,"_iplHRTFRelease@4") ||
           !sdk.bind(newEnc,"_iplAmbisonicsEncodeEffectCreate@16") || !sdk.bind(newDec,"_iplAmbisonicsDecodeEffectCreate@16") ||
           !sdk.bind(encode,"_iplAmbisonicsEncodeEffectApply@16") || !sdk.bind(decode,"_iplAmbisonicsDecodeEffectApply@16") ||
           !sdk.bind(resetDecoder,"_iplAmbisonicsDecodeEffectReset@4") || !sdk.bind(freeEnc,"_iplAmbisonicsEncodeEffectRelease@4") || !sdk.bind(freeDec,"_iplAmbisonicsDecodeEffectRelease@4"))return false;
        IPLAudioSettings audio{Room::rate,frames};
        IPLHRTFSettings hs{};hs.volume=1.f;
        newHrtf(sdk.context,&audio,&hs,&hrtf);
        IPLAmbisonicsDecodeEffectSettings ds{};ds.speakerLayout.type=IPL_SPEAKERLAYOUTTYPE_STEREO;
        ds.hrtf=hrtf;ds.maxOrder=1;
        if(newDec(sdk.context,&audio,&ds,&dec)!=IPL_STATUS_SUCCESS)return false;
        IPLAmbisonicsEncodeEffectSettings es{1};
        // First-order harmonics are linear in the unit direction. Measure the SDK's
        // normalization and axis convention once, outside the real-time mixer.
        IPLAmbisonicsEncodeEffect encoder{};
        if(newEnc(sdk.context,&audio,&es,&encoder)!=IPL_STATUS_SUCCESS)return false;
        float ones[frames],tmp[4][frames],*planes[]={tmp[0],tmp[1],tmp[2],tmp[3]},*mono=ones;
        std::fill_n(ones,frames,1.f);
        IPLAudioBuffer in{1,frames,&mono},out{4,frames,planes};
        const Vec axes[]={{1,0,0},{0,1,0},{0,0,1},{-1,0,0}};
        for(int a=0;a<4;a++){
            IPLAmbisonicsEncodeEffectParams params{audioVector(axes[a]),1};
            encode(encoder,&params,&in,&out);
            encode(encoder,&params,&in,&out);
            for(int k=0;k<4;k++)basis[a][k]=tmp[k][frames-1];
        }
        freeEnc(&encoder);
        for(int k=0;k<4;k++){
            float constant=.5f*(basis[0][k]+basis[3][k]);
            for(int a=0;a<3;a++)basis[a][k]-=constant;
            basis[3][k]=constant;
        }
        available=true;return true;
    }
    ~SoundField(){

        if(dec)freeDec(&dec);
        if(hrtf)freeHrtf(&hrtf);
    }
    void add(int channel,float* mono,Vec direction){
        for(int k=0;k<4;k++){
            float target=basis[3][k]+direction.x*basis[0][k]+direction.y*basis[1][k]+direction.z*basis[2][k];
            float gain=previous[channel][k],step=(target-gain)/frames;
            for(int i=0;i<frames;i++){gain+=step;mix[k][i]+=gain*mono[i];}
            previous[channel][k]=target;
        }
    }
    void spatialize(float* stereo,const SceneView& view,float target,float& blend){
        float *channels[]={mix[0],mix[1],mix[2],mix[3]};
        float l[frames],r[frames],*output[]={l,r};
        IPLAudioBuffer in{4,frames,channels},out{2,frames,output};
        IPLAmbisonicsDecodeEffectParams params{1,hrtf,view.camera,headphones && hrtf?IPL_TRUE:IPL_FALSE};
        decode(dec,&params,&in,&out);
        for(int i=0;i<frames;i++){
            blend+=std::clamp(target-blend,-.001f,.001f);
            stereo[2*i]+=blend*(l[i]-stereo[2*i]);
            stereo[2*i+1]+=blend*(r[i]-stereo[2*i+1]);
        }
    }
};
