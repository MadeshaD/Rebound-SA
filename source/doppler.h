// These wrappers replace only the native Doppler multiplier. GTA applies
// its authored playback frequency and slow-motion factor after the call.
Vec earPosition{},earVelocity{};
DWORD earTime{};
bool earValid{};
uintptr_t earVehicle{};
DWORD pitchHoldUntil{};
bool changingSeat{};
void updateEar(){
    Vec current{};
    reinterpret_cast<void (__cdecl*)(Vec*,int)>(0x56E010)(&current,-1);
    current.z+=.65f;
    DWORD tick=read<DWORD>(0xB7CB84),dt=tick-earTime;
    if(!dt && earValid)return;
    auto vehicle=reinterpret_cast<uintptr_t (__cdecl*)(int,bool)>(0x56E0D0)(-1,false);
    if(earValid && vehicle!=earVehicle){changingSeat=true;pitchHoldUntil=tick+250;}
    earVehicle=vehicle;
    if(changingSeat && LONG(tick-pitchHoldUntil)>=0)changingSeat=false;
    Vec previousVelocity=earVelocity;
    earVelocity={};
    if(earValid && !changingSeat && dt && dt<=200){
        float k=1000.f/dt;
        Vec measured{(current.x-earPosition.x)*k,(current.y-earPosition.y)*k,(current.z-earPosition.z)*k};
        if(magnitude(measured)<150.f){
            // Reject single-frame velocity spikes from switching position anchors.
            float response=1.f-std::exp(-float(dt)/75.f);
            Vec change=difference(measured,previousVelocity);
            float limit=std::min(response, .09f*dt/std::max(.001f,magnitude(change)));
            earVelocity=advance(previousVelocity,change,limit);
        }
    }
    earValid=true;earTime=tick;earPosition=current;
}
extern "C" __attribute__((used,noinline,force_align_arg_pointer)) float __cdecl surfacePitch(uintptr_t sound){
    DWORD previous=read<DWORD>(sound+0x44),current=read<DWORD>(sound+0x40);
    float scale=read<float>(sound+0x50);
    if(read<unsigned short>(sound+0x56)&65)
        return reinterpret_cast<float(__cdecl*)(float,float,DWORD,DWORD,float)>(0x4d7e40)(
            read<float>(sound+0x4c),read<float>(sound+0x48),previous,current,scale);
    if(!earValid || changingSeat || !scale || !std::isfinite(scale) || GetCurrentThreadId()!=gameThread)return 1.f;
    Vec pos=read<Vec>(sound+0x24),prev=read<Vec>(sound+0x30);
    Vec delta{pos.x-earPosition.x,pos.y-earPosition.y,pos.z-earPosition.z};
    float dist=magnitude(delta);
    if(!(dist>.5f))return 1.f;
    DWORD dt=current-previous;
    float sourceAway=0.f;
    if(dt && dt<=200 && read<DWORD>(0xB7CB84)-current<=200)
        sourceAway=((pos.x-prev.x)*delta.x+(pos.y-prev.y)*delta.y+(pos.z-prev.z)*delta.z)*1000.f/(dt*dist);
    float listenerToward=(earVelocity.x*delta.x+earVelocity.y*delta.y+earVelocity.z*delta.z)/dist;
    return radialPitch(listenerToward,sourceAway);
}
__attribute__((naked)) void pitchESI(){asm volatile("pushl %esi\n call _surfacePitch\n addl $4,%esp\n ret");}
__attribute__((naked)) void pitchEDI(){asm volatile("pushl %edi\n call _surfacePitch\n addl $4,%esp\n ret");}
__attribute__((naked)) void pitchOffset(){asm volatile("leal 4(%edi),%eax\n pushl %eax\n call _surfacePitch\n addl $4,%esp\n ret");}
