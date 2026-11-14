// One reusable simulation source. Synchronous direct queries are issued only
// from the game thread, where the loaded GTA collision is safe to access.
namespace live {
static IPLMaterial generic{{.10f,.20f,.30f},.05f,{.100f,.050f,.030f}};
static IPLMaterial concrete{{.05f,.07f,.08f},.05f,{.015f,.002f,.001f}};
static IPLMaterial glass{{.06f,.03f,.02f},.05f,{.060f,.044f,.011f}};
static IPLMaterial wood{{.11f,.07f,.06f},.05f,{.070f,.014f,.005f}};
static IPLMaterial metal{{.20f,.07f,.06f},.05f,{.200f,.025f,.010f}};

// Category IDs from the supplied surfaud.dat, joined by surface name to eSurfaceType.
// Grass, sand and water are approximate acoustic profiles.
static IPLMaterial grass{{.24f,.48f,.70f},.45f,{.11f,.055f,.025f}};
static IPLMaterial sand{{.20f,.40f,.58f},.34f,{.06f,.024f,.010f}};
static IPLMaterial gravel{{.60f,.70f,.80f},.05f,{.031f,.012f,.008f}};
static IPLMaterial water{{.01f,.02f,.03f},.04f,{.03f,.012f,.004f}};
static IPLMaterial tile{{.01f,.02f,.02f},.05f,{.060f,.044f,.011f}};
static IPLMaterial* const profiles[]={
    &generic,&concrete,&concrete,&concrete,&concrete,&concrete,&gravel,&concrete, // 0
    &concrete,&grass,&grass,&grass,&grass,&grass,&grass,&grass, // 8
    &grass,&grass,&concrete,&grass,&grass,&concrete,&concrete,&grass, // 16
    &generic,&gravel,&gravel,&gravel,&sand,&sand,&sand,&sand, // 24
    &sand,&sand,&sand,&concrete,&concrete,&concrete,&water,&water, // 32
    &grass,&grass,&wood,&wood,&wood,&glass,&glass,&glass, // 40
    &generic,&generic,&metal,&metal,&metal,&metal,&metal,&metal, // 48
    &metal,&metal,&metal,&metal,&metal,&concrete,&concrete,&metal, // 56
    &metal,&metal,&concrete,&concrete,&concrete,&concrete,&wood,&sand, // 64
    &wood,&wood,&sand,&sand,&sand,&sand,&sand,&sand, // 72
    &grass,&grass,&grass,&concrete,&concrete,&concrete,&concrete,&grass, // 80
    &concrete,&concrete,&wood,&wood,&wood,&wood,&wood,&wood, // 88
    &water,&water,&water,&water,&water,&gravel,&wood,&tile, // 96
    &wood,&wood,&tile,&tile,&wood,&gravel,&grass,&grass, // 104
    &grass,&grass,&grass,&grass,&grass,&grass,&grass,&grass, // 112
    &grass,&grass,&grass,&sand,&sand,&grass,&sand,&concrete, // 120
    &sand,&sand,&sand,&sand,&sand,&grass,&sand,&sand, // 128
    &sand,&wood,&concrete,&concrete,&gravel,&gravel,&gravel,&grass, // 136
    &concrete,&grass,&grass,&grass,&grass,&grass,&grass,&grass, // 144
    &grass,&grass,&water,&water,&water,&grass,&wood,&concrete, // 152
    &grass,&concrete,&metal,&wood,&metal,&concrete,&generic,&metal, // 160
    &metal,&generic,&generic,&metal,&wood,&wood,&wood,&glass, // 168
    &grass,&generic,&gravel, // 176
};
static_assert(sizeof(profiles)/sizeof(*profiles)==179);
IPLMaterial* surface(unsigned id){
    return id<179 ? profiles[id] : &generic;
}
int* remaining{};
void IPLCALL closest(const IPLRay* ray,float lo,float hi,IPLHit* hit,void*){
    *hit={};hit->distance=HUGE_VALF;
    if(GetCurrentThreadId()!=gameThread.load() || !std::isfinite(lo) || lo>=hi)return;
    // Skip the immediately preceding face when walking across multiple layers.
    float start=std::max(0.f,lo)+.005f;
    float length=std::min(hi-start,302.f);
    if(!(length>.001f))return;
    auto d=ray->direction,o=ray->origin;
    Vec a{o.x+start*d.x,o.y+start*d.y,o.z+start*d.z};
    Vec b{a.x+length*d.x,a.y+length*d.y,a.z+length*d.z};
    Hit p{};void* entity=nullptr;
    if(remaining)--*remaining;
    if(!castRay(a,b,p,entity))return;
    float t=(p.point.x-a.x)*d.x+(p.point.y-a.y)*d.y+(p.point.z-a.z)*d.z;
    if(t<0 || t>=length)return;
    hit->distance=start+t;hit->normal={p.normal.x,p.normal.y,p.normal.z};
    hit->material=surface(p.material);
}
void IPLCALL any(const IPLRay* ray,float lo,float hi,IPLuint8* blocked,void* data){
    IPLHit hit;closest(ray,lo,hi,&hit,data);*blocked=lo>=hi || std::isfinite(hit.distance);
}

#define DIRECT_API(X) X(ContextCreate,8) X(SceneCreate,12) X(SceneCommit,4) X(SimulatorCreate,12) X(SimulatorSetScene,8) X(SimulatorCommit,4) X(SourceCreate,12) X(SourceAdd,8) X(SourceSetInputs,12) X(SimulatorSetSharedInputs,12) X(SimulatorRunDirect,4) X(SourceGetOutputs,12) X(AirAbsorptionCalculate,36)
#define DECLARE_FN(n,b) decltype(&ipl##n) n{};
DIRECT_API(DECLARE_FN)
#undef DECLARE_FN
IPLContext context{};
IPLScene scene{};
IPLSimulator sim{};
IPLSource source{};
bool started{};
bool start(const std::wstring& path){
    HMODULE dll=LoadLibraryW(path.c_str());
    if(!dll)return false;
#define LOAD_FN(n,b) {auto symbol=GetProcAddress(dll,"_ipl" #n "@" #b);if(!symbol)return false;std::memcpy(&n,&symbol,sizeof(n));}
    DIRECT_API(LOAD_FN)
#undef LOAD_FN
#undef DIRECT_API
    IPLContextSettings cs{};cs.version=STEAMAUDIO_VERSION;
    IPLSceneSettings ss{};ss.type=IPL_SCENETYPE_CUSTOM;ss.closestHitCallback=closest;ss.anyHitCallback=any;
    IPLSimulationSettings setup{};setup.flags=IPL_SIMULATIONFLAGS_DIRECT;setup.sceneType=ss.type;
    setup.maxNumOcclusionSamples=5;setup.maxNumSources=1;setup.numThreads=1;
    setup.rayBatchSize=1;setup.samplingRate=Room::rate;setup.frameSize=frames;
    IPLSourceSettings settings{IPL_SIMULATIONFLAGS_DIRECT};
    if(ContextCreate(&cs,&context)||SceneCreate(context,&ss,&scene)||SimulatorCreate(context,&setup,&sim)||
       SourceCreate(sim,&settings,&source))return false;
    SceneCommit(scene);SimulatorSetScene(sim,scene);SourceAdd(source,sim);SimulatorCommit(sim);
    return true;
}
IPLCoordinateSpace3 space(Vec v){
    IPLCoordinateSpace3 c{};c.right={1,0,0};c.up={0,0,1};c.ahead={0,1,0};c.origin={v.x,v.y,v.z};return c;
}
IPLDirectEffectParams query(Vec emitter,bool volumetric,int& budget){
    IPLSimulationInputs in{};in.flags=IPL_SIMULATIONFLAGS_DIRECT;
    in.directFlags=static_cast<IPLDirectSimulationFlags>(IPL_DIRECTSIMULATIONFLAGS_OCCLUSION|IPL_DIRECTSIMULATIONFLAGS_TRANSMISSION);
    in.source=space(emitter);in.occlusionType=volumetric?IPL_OCCLUSIONTYPE_VOLUMETRIC:IPL_OCCLUSIONTYPE_RAYCAST;
    in.occlusionRadius=.85f;in.numOcclusionSamples=5;in.numTransmissionRays=2;
    IPLSimulationSharedInputs shared{};shared.listener=space(listener);
    remaining=&budget;
    SimulatorSetSharedInputs(sim,in.flags,&shared);SourceSetInputs(source,in.flags,&in);
    SimulatorRunDirect(sim);
    remaining=nullptr;
    IPLSimulationOutputs out{};SourceGetOutputs(source,in.flags,&out);return out.direct;
}
}
