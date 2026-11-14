#pragma once
#include <array>
#include <cmath>

// Eight mutually coupled delay lines, orthogonal Householder feedback,
// first-order high-frequency damping. Decay is selected from the measured room dimensions.
struct Room {
    static constexpr int rate=32000;
    int delays[8]={397,461,541,613,719,811,907,1013};
    std::array<std::array<float,2797>,8> lines{};
    int pos[8]{};
    float low[8]{}, feedback[8]{};
    int decay=0;
    explicit Room(bool large=false) {
        if (large) {
            constexpr int tunnel[8]={1031,1201,1423,1627,1877,2129,2477,2797};
            for (int i=0;i<8;i++) delays[i]=tunnel[i];
        }
        setDecay(450);
    }
    void clear() {
        for(auto& line:lines) line.fill(0);
        for(float& value:low) value=0;
    }
    void setDecay(int milliseconds) {
        if(decay==milliseconds) return;
        decay=milliseconds;
        for (int i=0;i<8;i++) feedback[i]=std::pow(0.001f,delays[i]/(rate*(decay*0.001f)));
    }
    void tick(float input,float& left,float& right) {
        float y[8],sum=0;
        for (int i=0;i<8;i++) {
            y[i]=lines[i][pos[i]];
            low[i]+=0.36f*(y[i]-low[i]);
            if (std::abs(low[i])<1e-18f) low[i]=0;
            sum+=low[i];
        }
        const float signs[]={1,-1,-1,1,-1,1,1,-1};
        for (int i=0;i<8;i++) {
            lines[i][pos[i]]=input*signs[i]*0.35f+feedback[i]*(low[i]-0.25f*sum);
            if (++pos[i]==delays[i]) pos[i]=0;
        }
        left=(y[0]-y[2]+y[4]-y[6])*0.5f;
        right=(y[1]-y[3]+y[5]-y[7])*0.5f;
    }
};
