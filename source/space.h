#pragma once
#include <algorithm>

inline float roofProximity(float height) {
    return std::clamp((24.f-height)/16.f,0.f,1.f);
}

struct SpaceResponse {
    float returnLevel, diffusion, tunnel, decay, corridor, enclosure;
};

inline SpaceResponse assessSpace(float height,float width,float length,float nearest,float surrounding,float facadeHeight,float largeRoom,float enclosure,float absorption) {
    float facing=std::clamp((48.f-width)/32.f,0.f,1.f);
    float wall=std::clamp(1.f-nearest/30.f,0.f,1.f);
    float roof=roofProximity(height);
    float tunnel=roof*std::max(facing,.75f*surrounding);
    float alley=facing*(.55f+.45f*surrounding);
    float size=std::clamp((length-8.f)/95.f,0.f,1.f);
    float diffusion=std::max(.60f*alley,tunnel);
    float decay=.30f+(.62f+.18f*size)*alley+tunnel*(.65f+1.90f*size);
    float level=.30f*wall+.45f*alley+.08f*roof+.65f*tunnel;
    float aspect=std::clamp((length/std::max(width,1.f)-1.f)/3.f,0.f,1.f);
    float canyon=facadeHeight*aspect*std::clamp((40.f-width)/24.f,0.f,1.f)*(1.f-roof);
    level+=.20f*canyon;
    diffusion+=.32f*canyon;
    decay+=canyon*(.35f+.90f*size);
    level+=.52f*largeRoom;
    diffusion=std::max(diffusion,.90f*largeRoom);
    decay+=2.1f*largeRoom;
    tunnel=std::max(tunnel,.80f*largeRoom);
    // Approximate a room with a box. Height is the ceiling clearance at ear level.
    float h=std::max(2.f,height+1.6f);
    float w=std::max(2.f,width), l=std::max(w,length);
    float alpha=std::clamp(absorption,.06f,.8f);
    float volumeOverArea=.5f/(1.f/h+1.f/w+1.f/l);
    float roomTime=.161f*volumeOverArea/alpha;
    // Long non-diffuse spaces retain a distinct succession of lateral returns.
    float narrow=1.f/(1.f+w/22.f);
    float corridor=enclosure*aspect*size*narrow;
    float hard=1.f-alpha;
    float enclosedTime=std::clamp(roomTime+.25f+4.f*corridor*hard,.35f,6.5f);
    // Small garages lose energy through fittings and openings that collision
    // triangles do not describe. Fade this correction out in longer rooms.
    float compact=std::clamp((22.f-l)/10.f,0.f,1.f)*std::clamp((8.f-h)/3.f,0.f,1.f);
    float compactTime=std::clamp(.18f+.018f*l+.022f*h-.35f*alpha,.22f,.78f);
    enclosedTime+=compact*(std::min(compactTime,enclosedTime)-enclosedTime);
    decay+=(enclosedTime-decay)*enclosure;
    return {std::min(1.f,level),diffusion,tunnel,decay,corridor*hard,enclosure};
}

// Roof and opposing wall hits distinguish a large enclosure from an isolated canopy.
inline float largeEnclosure(float ceiling,float radius,float width,float length,float pairs){
    float roof=std::clamp((radius-ceiling)/(.2f*radius),0.f,1.f);
    float size=std::clamp((length-35.f)/55.f,0.f,1.f);
    float breadth=std::clamp(width/32.f,.35f,1.f);
    return roof*size*breadth*std::clamp(pairs*2.f,0.f,1.f);
}

// Estimate how much facing masonry remains above ear height and how far a
// pair of rays can travel along the street in each direction.
template<class Distance>
float streetFacades(Vec ear,const Vec* axes,const float* ranges,int across,float width,float radius,float& height,Distance cast){
    float sides[2]{};
    for(int s=0;s<2;s++){
        float base=ranges[across+8*s];
        if(base>=std::min(60.f,radius-.2f))continue;
        Vec dir=axes[across];float sign=s?-1.f:1.f;dir.x*=sign;dir.y*=sign;
        for(float z:{4.f,9.f,16.f}){
            Vec origin=ear;origin.z+=z;
            float upper=cast(origin,dir,radius);
            if(upper<radius-.2f)sides[s]+=std::clamp(1.f-std::abs(upper-base)/(3.f+.35f*base),0.f,1.f)/3.f;
        }
    }
    height=std::min(sides[0],sides[1]);
    if(height<=0.f)return 0.f;
    int axis=(across+4)%8;
    auto extent=[&](int i){return std::min(ranges[i],.5f*(ranges[(i+1)%16]+ranges[(i+15)%16]));};
    float reach=std::max(10.f,std::min(2.f*width,radius*.8f));
    float openEnds=.5f*(std::min(1.f,extent(axis)/reach)+std::min(1.f,extent(axis+8)/reach));
    return height*std::clamp((100.f-width)/65.f,0.f,1.f)*(1.f-.40f*openEnds);
}
