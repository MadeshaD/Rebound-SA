// These four calls in CGarage already resolve the moving leaf every game update.
// Keep a snapshot of its geometry. Never dereference cached game objects.
struct DoorPanel {
    uintptr_t audio{},entity{};
    DWORD seen{};
    Vec origin{},axes[3]{},low{},high{};
    Vec nearest(Vec listener)const{
        Vec d{listener.x-origin.x,listener.y-origin.y,listener.z-origin.z};
        float lo[]={low.x,low.y,low.z},hi[]={high.x,high.y,high.z};
        Vec p=origin;
        for(int i=0;i<3;i++){
            Vec a=axes[i];
            float len2=a.x*a.x+a.y*a.y+a.z*a.z;
            float t=(a.x*d.x+a.y*d.y+a.z*d.z)/std::max(.0001f,len2);
            t=std::clamp(t,lo[i],hi[i]);
            p.x+=a.x*t;p.y+=a.y*t;p.z+=a.z*t;
        }
        return p;
    }
};
DoorPanel panels[16]{};
constexpr uintptr_t doorCalls[]={0x44a6c6,0x44a70a,0x44a7ad,0x44a7f1};
void __fastcall findPanels(uintptr_t garage,void*,uintptr_t* first,uintptr_t* second){
    using Find=void (__thiscall*)(uintptr_t,uintptr_t*,uintptr_t*);
    reinterpret_cast<Find>(0x449ff0)(garage,first,second);
    if(!first || !*first)return;
    uintptr_t object=*first,matrix=read<uintptr_t>(object+0x14);
    if(!matrix)return;
    unsigned model=read<unsigned short>(object+0x22);
    if(model>=20000)return;
    uintptr_t info=read<uintptr_t>(0xA9B0C8+4*model);
    uintptr_t col=info?read<uintptr_t>(info+0x14):0;
    if(!col)return;
    DoorPanel item{};
    item.audio=garage+0x50;item.entity=object;item.seen=GetTickCount();
    item.origin=read<Vec>(matrix+0x30);
    for(int i=0;i<3;i++)item.axes[i]=read<Vec>(matrix+16*i);
    item.low=read<Vec>(col);item.high=read<Vec>(col+12);
    if(!(item.high.x>=item.low.x && item.high.y>=item.low.y && item.high.z>=item.low.z))return;
    DoorPanel* dest=panels;
    for(auto& panel:panels){
        if(panel.audio==item.audio){dest=&panel;break;}
        if(!panel.audio || item.seen-panel.seen>item.seen-dest->seen)dest=&panel;
    }
    *dest=item;
}
const DoorPanel* activePanel(uintptr_t audio,DWORD now){
    for(const auto& panel:panels)if(panel.audio==audio && audio && now-panel.seen<800)return &panel;
    return nullptr;
}
