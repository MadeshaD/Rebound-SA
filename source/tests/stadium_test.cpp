// Fixture pairs transcribed from the supplied stock stadint.ipl.
// Native test: c++ -std=c++17 tests/stadium_test.cpp -o /tmp/stadium_test && /tmp/stadium_test
#include "../stadium.h"
#include <cassert>
int main(){
    const unsigned fixtures[][2]={
        {13590,14},
        {13591,14},
        {13592,14},
        {13593,14},
        {13594,14},
        {13595,15},
        {13596,15},
        {13597,15},
        {13598,1},
        {13599,1},
        {13600,1},
        {13601,1},
        {13602,1},
        {13605,16},
        {13606,16},
        {13607,16},
        {13608,16},
        {13609,16},
        {13610,16},
        {13611,15},
        {13612,15},
        {13613,15},
        {13614,15},
        {13615,15},
        {13616,15},
        {13617,15},
        {13618,15},
        {13619,15},
        {13620,15},
        {13621,15},
        {13622,16},
        {13623,16},
        {13624,7},
        {13625,7},
        {13626,7},
        {13627,1},
        {13628,7},
        {13629,7},
        {13630,7},
        {13631,4},
        {13632,4},
        {13633,4},
        {13634,14},
        {13635,14},
        {13636,14},
        {13637,14},
        {13638,14},
        {13639,14},
        {13640,14},
        {13641,14},
        {13642,14},
        {13643,14},
        {13644,14},
        {13645,14},
        {13646,14},
        {13647,14},
        {13648,14},
        {13649,14},
        {13650,14},
        {13651,15},
        {13652,15},
        {13653,15},
        {13654,7},
        {13655,7},
        {13656,15},
        {13657,14},
        {13658,14},
        {13659,7},
        {13660,7},
        {13661,7},
        {13662,4},
        {13663,4},
        {13664,4},
        {13665,4},
        {13666,14},
        {13667,4},
    };
    for(auto& pair:fixtures){
        assert(stadium::matches(pair[0],int(pair[1])));
        assert(stadium::candidateArea(int(pair[1])));
        for(int area=0;area<=18;++area)
            if(area!=int(pair[1]))assert(!stadium::matches(pair[0],area));
    }
    for(int area=0;area<=18;++area){
        assert(!stadium::matches(13589,area));
        assert(!stadium::matches(13668,area));
        assert(!stadium::matches(400,area));
        assert(!stadium::matches(0xffffffffu,area));
    }
    assert(!stadium::matches(13603,255)); // unused placement
    assert(!stadium::matches(13628,-1));
}
