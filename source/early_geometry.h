#pragma once
#include <algorithm>
#include <cmath>
struct Arrival {
    unsigned delay{};
    float gain{},pan{};
    Vec bearing{};
};
struct EarlyPaths {Arrival wall[2]{},entryEcho{};};
inline Vec difference(Vec a,Vec b){return {a.x-b.x,a.y-b.y,a.z-b.z};}
inline float dot3(Vec a,Vec b){return a.x*b.x+a.y*b.y+a.z*b.z;}
inline Vec advance(Vec a,Vec d,float t){return {a.x+d.x*t,a.y+d.y*t,a.z+d.z*t};}
// Mirror the source in the candidate plane. Both endpoints must be on the same side.
inline bool bouncePoint(Vec ear,Vec source,Vec plane,Vec normal,Vec& point){
    float a=dot3(difference(ear,plane),normal),b=dot3(difference(source,plane),normal);
    if(a*b<=0 || std::min(std::abs(a),std::abs(b))<.06f)return false;
    Vec image=advance(source,normal,-2*b);
    point=advance(ear,difference(image,ear),a/(a+b));
    return true;
}
inline Arrival arrival(float travelled,float straight,float gain,float pan){
    float samples=(travelled-straight)*(32000.f/343.f);
    if(!std::isfinite(samples) || samples<-.1f || samples>16000.f)return {};
    return {unsigned(std::max(0.f,std::round(samples))),gain,std::clamp(pan,-1.f,1.f)};
}
// Nearby reflections of a continuous loop can sweep through many phases of the
// dry note between two ray updates. Keep their acoustic delay until the wall
// fades or leaves the near field. Gain and bearing still follow the geometry.
struct NearWallDelay {
    unsigned samples{};
    bool held{};
    void update(Arrival& path,bool looping,bool restart,int stretch){
        if(restart || !looping || path.gain<=0.f){held=false;return;}
        const unsigned nearLimit=unsigned(800*stretch/100); // 25 ms before stretch
        if(path.delay>nearLimit+nearLimit/4){held=false;return;}
        if(!held && path.delay<=nearLimit){samples=path.delay;held=true;}
        if(held)path.delay=samples;
    }
};
// Scatter into the future so a short source can finish before its echo arrives.
struct ArrivalQueue {
    float stereo[65536][2]{};
    unsigned head{};
    void add(unsigned i,Arrival a,float x){
        if(a.gain<=0)return;
        float value=x*a.gain;
        auto& pair=stereo[(head+i+a.delay)&65535];
        pair[0]+=value*(1-a.pan)*.5f;pair[1]+=value*(1+a.pan)*.5f;
    }
    void drain(float* out,unsigned frames){
        for(unsigned i=0;i<frames;i++){
            auto& pair=stereo[(head+i)&65535];
            out[2*i]=pair[0];out[2*i+1]=pair[1];pair[0]=pair[1]=0;
        }
        head=(head+frames)&65535;
    }
};

// Intersect the two facade lines at ear height, on their air sides.
// Caller checks both finite legs against streamed collision.
inline bool outsideCorner(Vec a,Vec an,Vec b,Vec bn,Vec ear,Vec source,Vec& corner){
    if(std::abs(an.z)>.35f || std::abs(bn.z)>.35f)return false;
    if(dot3(an,difference(ear,a))<0)an=advance({},an,-1);
    if(dot3(bn,difference(source,b))<0)bn=advance({},bn,-1);
    float det=an.x*bn.y-an.y*bn.x;
    if(std::abs(det)<.25f)return false;
    float z=(ear.z+source.z)*.5f;
    // Work relative to a so the world-coordinate magnitude does not affect accuracy.
    float u=.22f-an.z*(z-a.z);
    float v=dot3(bn,difference(b,a))+.22f-bn.z*(z-a.z);
    corner={a.x+(u*bn.y-an.y*v)/det,a.y+(an.x*v-u*bn.x)/det,z};
    return magnitude(difference(corner,a))<35 && magnitude(difference(corner,b))<35;
}
