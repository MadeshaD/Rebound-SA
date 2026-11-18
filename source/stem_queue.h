#pragma once
#include <array>
#include <atomic>
#include <algorithm>
#include <cstdint>

// One producer (Rebound world effects), one consumer (GTA's output). Both stems
// have the same clock. A stream handoff discards old packets on the consumer.
struct Stems {float world[2]{},player[2]{};};
class StemQueue {
    static constexpr unsigned capacity=16384,mask=capacity-1;
    std::array<Stems,capacity> fifo{};
    std::atomic<unsigned> head{},tail{};
    double fraction{};
public:
    unsigned size()const{
        unsigned w=head.load(std::memory_order_acquire),r=tail.load(std::memory_order_acquire);
        return std::min(w-r,capacity);
    }
    bool push(const Stems* src,unsigned n){
        unsigned w=head.load(std::memory_order_relaxed),r=tail.load(std::memory_order_acquire);
        if(n>capacity-(w-r))return false;
        for(unsigned i=0;i<n;i++)fifo[(w+i)&mask]=src[i];
        head.store(w+n,std::memory_order_release);return true;
    }
    void discard(){tail.store(head.load(std::memory_order_acquire),std::memory_order_release);fraction=0;}
    bool pop(double step,Stems& out){
        unsigned r=tail.load(std::memory_order_relaxed),w=head.load(std::memory_order_acquire);
        const unsigned advance=unsigned(fraction+step);
        if(w-r<std::max(advance,2u)){out={};return false;}
        const auto& a=fifo[r&mask];const auto& b=fifo[(r+1)&mask];
        for(int c=0;c<2;c++){
            out.world[c]=a.world[c]+float(fraction)*(b.world[c]-a.world[c]);
            out.player[c]=a.player[c]+float(fraction)*(b.player[c]-a.player[c]);
        }
        fraction+=step-advance;
        if(advance)tail.store(r+advance,std::memory_order_release);
        return true;
    }
};
