#pragma once
#include <algorithm>
#include <array>

namespace injury {
// Each stage splits one arrival into three. Four stages spread a transient over
// up to 81 arrival paths before the late reverb. No recirculating delay peaks.
class Diffusion {
    static constexpr unsigned size=8192;
    struct Stage {
        std::array<float,size> history{};
        unsigned first{},second{};
    };
    std::array<Stage,4> stages{};
    unsigned head{},valid{};
public:
    void configure(unsigned rate){
        constexpr float times[][2]={{3.7f,8.9f},{6.1f,14.3f},{10.7f,23.9f},{16.3f,36.7f}};
        for(unsigned i=0;i<stages.size();++i){
            auto samples=[rate](float ms){return std::clamp(unsigned(rate*.001f*ms+.5f),1u,size-1);};
            stages[i].first=samples(times[i][0]);stages[i].second=samples(times[i][1]);
        }
        clear();
    }
    void clear(){head=valid=0;}
    struct Return {float mid,side;};
    Return sample(float x){
        float side=0;
        for(auto& stage:stages){
            auto read=[&](unsigned offset){return valid>=offset?stage.history[(head-offset)&(size-1)]:0.f;};
            const float a=read(stage.first),b=read(stage.second);
            stage.history[head]=x;
            x=.28f*x+.42f*a+.30f*b;
            side=.08f*(a-b);
        }
        valid=std::min(valid+1,size);head=(head+1)&(size-1);
        return {x,side};
    }
};
}
