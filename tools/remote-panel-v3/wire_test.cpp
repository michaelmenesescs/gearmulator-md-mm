#include "source/elektron/md/mdJucePlugin/mdRemotePanelWire.h"
#include "source/elektron/md/mdJucePlugin/mdRemotePanelOwnership.h"
#include <array>
#include <cstdlib>
#include <iostream>
#include <limits>
using namespace mdJucePlugin::remotePanel;
#define CHECK(x) do { if(!(x)) { std::cerr << "FAIL line " << __LINE__ << ": " #x << '\n'; return 1; } } while(false)
int main() {
 std::array<uint8_t,20> b{0x14,0,0x34,0x12,0x78,0x56,0x34,0x12,9};
 float x=71.25f,y=-2.5f; std::memcpy(b.data()+12,&x,4); std::memcpy(b.data()+16,&y,4);
 auto t=decodeTouch(b.data(),b.size());
 CHECK(t.phase==0 && t.contact==0x1234 && t.sequence==0x12345678 && t.generation==9 && t.x==x && t.y==y);
 for(size_t n=0;n<20;++n) CHECK(decodeTouch(b.data(),n).phase==255);
 CHECK(decodeTouch(b.data(),21).phase==255);
 for(uint32_t bits:{0x7f800000u,0xff800000u,0x7fc00001u,0x7f800001u}) {
  std::memcpy(b.data()+12,&bits,4); CHECK(decodeTouch(b.data(),20).phase==255);
 }
 std::memcpy(b.data()+12,&x,4);
 for(uint8_t phase=0;phase<4;++phase) { b[1]=phase; CHECK(decodeTouch(b.data(),20).phase==phase); }
 b[1]=4; CHECK(decodeTouch(b.data(),20).phase==255);
 b[1]=0; x=100001; std::memcpy(b.data()+12,&x,4); CHECK(decodeTouch(b.data(),20).phase==255);
 RowOwners rows;
 using C=RowOwners::Change;
 CHECK(rows.change(0,0,1,true)==C::RowChanged);
 CHECK(rows.change(0,0,1,true)==C::StillOwned && rows.count(0)==2);
 CHECK(rows.change(1,0,2,true)==C::RowChanged && rows.mergedRow(0,0)==3);
 CHECK(rows.change(0,0,1,false)==C::StillOwned && rows.count(0)==1 && rows.mergedRow(0,0)==3);
 CHECK(rows.change(0,0,1,false)==C::RowChanged && rows.mergedRow(0,0)==2);
 CHECK(rows.change(0,0,1,false)==C::Unmatched && rows.count(0)==0);
 CHECK(rows.change(1,0,2,false)==C::RowChanged && rows.mergedRow(0,4)==4);
 // Two logical aliases hold one physical bit; desktop row ownership is separate.
 rows.change(2,0,8,true); rows.change(3,0,8,true);
 rows.change(2,0,8,false); CHECK(rows.mergedRow(0,0)==8);
 rows.change(3,0,8,false); CHECK(rows.mergedRow(0,8)==8 && rows.mergedRow(0,0)==0);
 std::cout << "PASS production RowOwners: same-control refs 1/2/1/0, different bits, aliases, desktop merge, unmatched release\n";
 PanelBudget budget; size_t sent=0;
 for(uint64_t us=1000;us<=1001000;us+=1000) if(budget.consume(us,16425)) sent+=16425;
 CHECK(sent<=1250000+16384 && sent>1200000);
 CHECK(!budget.consume(1001000,2*1024*1024));
 budget.consume(2000000,0); CHECK(budget.tokens<=32768);
 std::cout << "PASS Touch: LE layout, phases, short/oversize, NaN/Inf, coordinate bounds (-ffast-math)\n";
 std::cout << "PASS PanelBudget: simulated 1 s bytes=" << sent << " burst<=32768, rejects 2 MiB\n";
}
