#include "../return_diffusion.h"
#include "../transient_audio.h"
#include <cassert>
#include <cmath>
#include <cstdint>

int main(){
    ReturnDiffusion left{},right{};
    double energy=0,lateEnergy=0;
    float highest=0;
    for(unsigned n=0;n<32000;++n){
        float in=n==0?1.f:0.f;
        float l=left.echo(in),r=right.echo(-.25f*in);
        assert(std::abs(r+.25f*l)<1.e-7f);
        energy+=l*l;if(n>=32*40)lateEnergy+=l*l;
        highest=std::max(highest,std::abs(l));
    }
    assert(std::abs(energy-1.)<.00001 && lateEnergy<.001);
    assert(highest<.7f);
    left={};assert(left.echo(0)==0);
    const Vec source{0,0,.32f},floor{0,0,0},up{0,0,1},ear{1.4f,0,1.5f};
    assert(contactFloor(ear,source,source,floor,up));
    assert(contactFloor(source,ear,source,floor,up));
    assert(!contactFloor({.4f,0,-1.5f},source,source,floor,up));
    assert(!contactFloor(ear,source,source,{.1f,0,.25f},{1,0,0}));
    assert(!contactFloor(ear,source,source,{1.2f,0,1.f},up));
    assert(!contactFloor(ear,{1,1,0},source,floor,up));
    assert(doorwayRollOff(2.f,13.f)==6.5f);
    assert(doorwayRollOff(2.f,3.f)==1 && doorwayRollOff(40.f,75.f)==1);
    DoorMotions moves;
    int plays=0,queries=0;bool loaded=false;Vec played{};
    auto service=[&](int area,uint32_t time){
        moves.service(area,time,[&]{++queries;return loaded;},
                      [&](Vec p){++plays;played=p;});
    };
    // Repeated movement while the bank is streaming is submitted exactly once.
    assert(moves.movement(1,0,ear,true,4000));
    assert(!moves.movement(1,0,ear,true,4033));service(0,4050);assert(plays==0);
    loaded=true;service(0,4175);service(0,4180);assert(plays==1 && played.x==ear.x);
    // Interleaved leaves, area transitions, and another action within the old cooldown.
    assert(moves.movement(2,0,floor,false,4200));
    assert(!moves.movement(1,0,ear,true,4210));service(0,4210);assert(plays==2);
    assert(moves.movement(1,0,ear,true,5000));service(9,5090);assert(plays==3 && played.x==0 && played.z==0);
    assert(moves.movement(3,9,ear,false,5200));service(0,5300);assert(plays==3);
    assert(moves.movement(4,0,ear,true,5400));service(0,6400);assert(plays==3);
    // After a save reload the same native pointer can immediately represent a new sound.
    moves={};assert(moves.movement(1,0,ear,true,20));service(0,20);assert(plays==4);
    moves={};assert(moves.movement(1,0,ear,true,UINT32_MAX-19));
    assert(!moves.movement(1,0,ear,true,25));service(0,25);assert(plays==5);
    int was=queries;service(0,45);assert(queries==was);
}
