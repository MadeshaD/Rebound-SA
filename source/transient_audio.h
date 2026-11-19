#pragma once
#include "acoustics.h"
#include <cstdint>
#include <array>

// World impacts are located on the collision face. Only exempt a floor hit at
// the source end of a direct segment, with the listener on the same air side.
// A slab between floors and a wall beside the impact remain occluders.
inline bool contactFloor(Vec a,Vec b,Vec source,Vec hit,Vec normal){
    auto within=[](Vec a,Vec b,float radius){
        const float x=a.x-b.x,y=a.y-b.y,z=a.z-b.z;
        return x*x+y*y+z*z<=radius*radius;
    };
    if(normal.z<=.65f || !within(hit,source,.44f))return false;
    const bool atStart=within(a,source,.40f);
    if(!atStart && !within(b,source,.40f))return false;
    Vec listener=atStart?b:a;
    return
           (listener.x-hit.x)*normal.x+(listener.y-hit.y)*normal.y+
           (listener.z-hit.z)*normal.z>.05f;
}

// During a nearby entry animation GTA briefly uses a distant fixed camera.
inline float doorwayRollOff(float playerDistance,float cameraDistance){
    if(!std::isfinite(playerDistance) || !std::isfinite(cameraDistance) ||
       playerDistance>=8.f || cameraDistance<=playerDistance+4.f)return 1.f;
    return cameraDistance/std::max(1.f,playerDistance);
}

// The door animation reports movement on many frames. Two separate movements
// may happen on the same leaf or in different interiors within four seconds.
struct DoorMotions {
    struct Motion {
        uintptr_t object{};
        int area{};
        uint32_t last{},requested{};
        Vec position{};
        bool pending{},local{};
    };
    std::array<Motion,8> leaves{};
    bool movement(uintptr_t leaf,int area,Vec position,bool local,uint32_t now){
        Motion* slot=&leaves[0];
        for(auto& m:leaves){
            if(m.object==leaf && m.area==area){slot=&m;break;}
            if(!m.object || (slot->object && uint32_t(now-m.last)>uint32_t(now-slot->last)))slot=&m;
        }
        const bool fresh=slot->object!=leaf || slot->area!=area || uint32_t(now-slot->last)>600;
        if(fresh)*slot={leaf,area,now,now,position,true,local};
        else{slot->last=now;slot->position=position;}
        return fresh;
    }
    template<class Ready,class Play>
    void service(int area,uint32_t now,Ready ready,Play play){
        bool queried=false,loaded=false;
        for(auto& m:leaves){
            if(!m.pending)continue;
            if(uint32_t(now-m.requested)>900 || (m.area!=area && !m.local)){
                m.pending=false;continue;
            }
            if(!queried){loaded=ready();queried=true;}
            if(loaded){
                m.pending=false;
                // The native door player treats a zero position as listener-relative.
                // Use that mode only for CJ's nearby doorway across a coordinate change.
                play(m.area==area?m.position:Vec{});
            }
        }
    }
};
