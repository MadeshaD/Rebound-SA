#pragma once
#include <algorithm>
#include <cmath>
#include <cstdint>
#include <cstring>
namespace mixpcm {
struct Format { unsigned rate=0,channels=0,bits=0; bool floating=false; int lfe=-1; unsigned validBits=0;
 bool valid()const {return rate>=8000&&rate<=384000&&channels>=1&&channels<=8&&(floating?bits==32:(bits==8||bits==16||bits==24||bits==32));}
 unsigned bytes()const{return bits/8;}
};
inline float read(const unsigned char*p,Format f){
 if(f.floating){float x;memcpy(&x,p,4);return std::isfinite(x)?x:0;}
 if(f.bits==8)return (int(*p)-128)/128.f;
 uint32_t u=0;for(unsigned j=0;j<f.bytes();j++)u|=uint32_t(p[j])<<(j*8);
 int64_t n=u;if(u&(uint32_t(1)<<(f.bits-1)))n-=int64_t(1)<<f.bits;
 return float(double(n)/double(uint64_t(1)<<(f.bits-1)));
}
inline void write(unsigned char*p,Format f,float x){
 x=std::clamp(x,-1.f,1.f);
 if(f.floating){memcpy(p,&x,4);return;}
 int64_t scale=int64_t(1)<<(f.bits-1);
 int64_t n=std::clamp(int64_t(std::llround(double(x)*scale)),-scale,scale-1);
 if(f.bits==8){*p=static_cast<unsigned char>(n+128);return;}
 uint32_t u=uint32_t(n);if(f.validBits && f.validBits<f.bits)u &= ~((uint32_t(1)<<(f.bits-f.validBits))-1);for(unsigned j=0;j<f.bytes();j++)p[j]=static_cast<unsigned char>(u>>(8*j));
}
}
