#pragma once
#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <cstring>

namespace mdJucePlugin::remotePanel {
struct TouchData {
 uint8_t phase=255;
 uint16_t contact=0;
 uint32_t sequence=0, generation=0;
 float x=0,y=0;
};
inline uint32_t readU32(const uint8_t* p) {
 return uint32_t(p[0]) | uint32_t(p[1])<<8 | uint32_t(p[2])<<16 | uint32_t(p[3])<<24;
}
inline TouchData decodeTouch(const uint8_t* p,size_t n) {
 TouchData t;
 if(n>=8) t.sequence=readU32(p+4);
 if(n!=20 || p[0]!=0x14 || p[1]>3) return t;
 auto x=readU32(p+12), y=readU32(p+16);
 // Bit validation remains correct with the project's -ffast-math settings.
 if((x&0x7f800000u)==0x7f800000u || (y&0x7f800000u)==0x7f800000u) return t;
 std::memcpy(&t.x,&x,4); std::memcpy(&t.y,&y,4);
 if(t.x < -100000 || t.x > 100000 || t.y < -100000 || t.y > 100000) return t;
 t.phase=p[1]; t.contact=uint16_t(p[2]) | uint16_t(p[3])<<8; t.generation=readU32(p+8);
 return t;
}
// Per-client PanelChunk token bucket. The sustained rate (1.25 B/us) is unchanged from the
// first v3 release; the burst cap holds one whole keyframe (~80 kB) so the first frame after
// a quiet period leaves immediately instead of trickling out over ~40 ms.
constexpr double g_panelBudgetBytesPerUs=1.25;
constexpr double g_panelBudgetBurst=131072;
struct PanelBudget {
 uint64_t lastUs=0;
 double tokens=16384;
 bool consume(uint64_t now,size_t bytes) {
  if(lastUs && now>=lastUs) tokens=std::min(g_panelBudgetBurst,tokens+(now-lastUs)*g_panelBudgetBytesPerUs);
  lastUs=now;
  if(tokens<bytes) return false;
  tokens-=bytes; return true;
 }
};
}
