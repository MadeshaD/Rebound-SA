#include "../concussion.h"
#include <cassert>
#include <cmath>
using injury::Concussion;
bool near(float a,float b){return std::abs(a-b)<.0001f;}
int main(){
    Concussion c;
    c.hit(1,1000,50,0);
    assert(near(c.value(1000),1));
    assert(near(c.value(5000),.5f));
    c.hit(1,5000,5,17);
    assert(near(c.value(5000),.5f) && c.value(9000)==0);
    c.hit(1,10000,12.5f,9);
    assert(near(c.value(10000),.25f) && near(c.value(11000),.125f) && c.value(12000)==0);
    c.clear();c.hit(1,100,50,18);c.hit(1,100,0,1);assert(!c.peak);
    c.hit(1,100,50,0);
    auto low=injury::evaluate(3,100,15,100,true,false,false,0);
    auto first=c.mix(low,c.value(100));assert(first.amount==1 && first.blastRing==2.5f);
    auto later=c.mix(low,c.value(6100));assert(later.amount==low.amount);
    auto healed=injury::evaluate(99,100,15,100,true,false,false,0);
    assert(near(c.mix(healed,c.value(6100)).amount,.25f));
    assert(c.mix(healed,c.value(8100)).amount==0);
    auto dead=c.mix(injury::evaluate(0,100,15,100,true,false,true,255),1);
    assert(dead.fade==0 && c.mix({},1).bypass);
    injury::Ring normal,blast;
    for(int i=0;i<48000;i++){
        normal.tick(1,1,48000);blast.tick(1,1,48000,2.5f);
    }
    assert(near(blast.gain/normal.gain,2.5f));
}
