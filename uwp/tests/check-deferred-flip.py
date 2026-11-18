#!/usr/bin/env python3
"""Exercise the production display flip prelude, including deferred PS3-native flips."""
from pathlib import Path
import argparse,subprocess,tempfile
parser=argparse.ArgumentParser(description=__doc__);parser.add_argument('--cxx',default='c++');args=parser.parse_args()
root=Path(__file__).resolve().parents[2]
s=(root/'rpcs3/Emu/RSX/Core/RSXDisplayEngine.cpp').read_text()
a=s.index('void display_engine::handle_emu_flip(u32 buffer)');b=s.index('\n\t\tint_flip_index += flip_notification_count;',a)
prelude=s[a:b]+'\n accepted++;\n}\n'
h=(root/'rpcs3/Emu/RSX/Core/RSXDisplay.h').read_text();a=h.index('struct display_flip_info_t');b=h.index('\n\tclass vblank_thread',a)
queue=h[a:b]
code=r'''
#include <algorithm>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <deque>
#include <limits>
using u32=std::uint32_t;using u64=std::uint64_t;
constexpr u64 umax=std::numeric_limits<u64>::max();
static void require(bool ok,const char* msg){if(!ok){std::fprintf(stderr,"FAIL: %s\n",msg);std::exit(1);}}
#define ensure(x) require(bool(x),"production ensure")
struct Log {void error(const char*){require(false,"unexpected discarded queue");}} rsx_log;
struct frame_statistics_t {int draws=0;};
@QUEUE@
enum class frame_limit_type {none,_30,_50,_60,_120,display_rate,_auto,_ps3,infinite};
struct {struct {int max_cpu_preempt_count_per_frame=0;} core;struct {frame_limit_type frame_limit=frame_limit_type::_ps3;u32 vblank_rate=60;double second_frame_limit=0.;} video;} g_cfg;
bool g_disable_frame_limit=false;
static u64 now=1000000;static u64 get_system_time(){return now;}
static double cached_refresh_rate(){return 60.;}
namespace lv2_obj {static int waits=0;void wait_timeout(u64 delay,void*,bool){++waits;now+=delay;}}
struct Context {u32 id()const{return 1;}frame_statistics_t stats{37};};
struct Pump {
 bool requested_vsync=true;Context ctx;int frame_ends=0;
 struct {u64 idle_time=0;} performance_counters;
 display_flip_info_t* queued=nullptr;
 Context* get_context(){return &ctx;}
 void on_frame_end(Context* c,u32 buffer,bool){++frame_ends;queued->stats=c->stats;queued->push(buffer);c->stats={};}
};
struct Requested {void operator|=(int){}};
struct Vblank {u64 value=100;operator u64()const{return value;}u64 operator+()const{return value;}};
struct display_engine {
 Pump* m_pump;display_flip_info_t m_queued_flip{};
 struct {u32 buffer=0,context_id=0;Requested requested;} m_async_flip_request;
 struct flip_request {static constexpr int emu_requested=1;};
 Vblank vblank_count;u64 vblank_at_flip=umax,target_rsx_flip_time=0,int_flip_index=0;int flip_notification_count=0,accepted=0;
 explicit display_engine(Pump* p):m_pump(p){p->queued=&m_queued_flip;}
 void handle_emu_flip(u32 buffer);
};
@PRELUDE@
int main(){
 // Queued frame: repeated retries must neither consume its metadata nor fabricate frame ends.
 Pump p;display_engine d(&p);d.m_queued_flip.stats.draws=91;d.m_queued_flip.push(2);
 d.handle_emu_flip(2);
 require(d.accepted==0 && p.frame_ends==0,"first deferred flip finalized frame");
 for(int i=0;i<10000;++i)d.handle_emu_flip(2);
 require(d.accepted==0 && p.frame_ends==0,"deferred retries re-finalize frames");
 require(d.m_queued_flip.buffer_queue.size()==1 && d.m_queued_flip.stats.draws==91,"deferred retry consumes queued frame/stats");
 ++d.vblank_count.value;d.handle_emu_flip(2);
 require(d.accepted==1 && p.frame_ends==0 && d.m_queued_flip.buffer==2 && d.m_queued_flip.stats.draws==91,"queued flip not accepted intact after vblank");
 // Manually requested flip has no queue entry: finalize exactly once, when eligible.
 Pump q;display_engine manual(&q);
 for(int i=0;i<100;++i)manual.handle_emu_flip(3);
 require(q.frame_ends==0,"manual pending flip repeatedly finalizes frame");
 ++manual.vblank_count.value;manual.handle_emu_flip(3);
 require(q.frame_ends==1 && manual.accepted==1 && manual.m_queued_flip.stats.draws==37,"manual accepted flip loses stats or duplicates finalization");
 // No-vsync PS3-native and ordinary rate-limited paths must still progress.
 Pump n;n.requested_vsync=false;display_engine immediate(&n);immediate.handle_emu_flip(0);
 require(immediate.accepted==1 && n.frame_ends==1,"non-vsync flip did not progress");
 g_cfg.video.frame_limit=frame_limit_type::_60;
 Pump f;display_engine limited(&f);limited.int_flip_index=1;limited.target_rsx_flip_time=now+10000;limited.handle_emu_flip(1);
 require(limited.accepted==1 && f.frame_ends==1 && lv2_obj::waits==1,"ordinary frame limiter changed");
 Pump r;display_engine recursive(&r);recursive.m_queued_flip.in_progress=true;recursive.handle_emu_flip(1);
 require(recursive.accepted==0 && r.frame_ends==0,"in-progress recursion guard changed");
 std::puts("PASS: production flip prelude preserves queued metadata across 10000 deferred retries; manual flips finalize once; no-vsync, rate limit and recursion paths progress correctly.");
}
'''
code=code.replace('@QUEUE@',queue).replace('@PRELUDE@',prelude)
with tempfile.TemporaryDirectory(prefix='rpcs3-deferred-flip-') as tmp:
 p=Path(tmp);(p/'test.cpp').write_text(code)
 subprocess.run([args.cxx,'-std=c++20','-Wall','-Wextra','-Werror',str(p/'test.cpp'),'-o',str(p/'test')],check=True)
 subprocess.run([str(p/'test')],check=True)
