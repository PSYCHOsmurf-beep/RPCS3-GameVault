#!/usr/bin/env python3
"""Exercise the actual UWP cache eviction method with deterministic pipeline storage."""
from pathlib import Path
import subprocess,tempfile,argparse
parser=argparse.ArgumentParser(description=__doc__)
parser.add_argument('--cxx',default='c++');args=parser.parse_args()
root=Path(__file__).resolve().parents[2]
text=(root/'rpcs3/Emu/RSX/Program/ProgramStateCache.h').read_text()
start=text.index('\ttemplate <typename Disposer>\n\tusz evict_idle_pipelines(')
op=text.index('{',start);depth=1;end=op+1
while depth:
 depth+=(text[end]=='{')-(text[end]=='}');end+=1
method=text[start:end]
code=r'''
#include <algorithm>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <memory>
#include <mutex>
#include <unordered_map>
#include <vector>
using u64=std::uint64_t;using usz=std::size_t;
static u64 now=1000000;
static u64 get_system_time(){return now;}
static void require(bool ok,const char* msg){if(!ok){std::fprintf(stderr,"FAIL: %s\n",msg);std::exit(1);}}
struct Mutex { bool held=false;void lock(){require(!held,"recursive map lock");held=true;}void unlock(){require(held,"unlock balance");held=false;}};
struct Pipeline {u64 uwp_last_use=0;int id=0;};
struct Cache {
 using pipeline_type=Pipeline;using pipeline_storage_type=std::unique_ptr<Pipeline>;
 std::unordered_map<int,pipeline_storage_type> m_storage;
 Mutex m_pipeline_mutex;
 void live(int id,u64 time){m_storage[id]=std::make_unique<Pipeline>(Pipeline{time,id});}
 @METHOD@
};
int main(){
 Cache cache; cache.live(1,0);cache.live(2,0);
 for(int i=3;i<=40;i++) cache.m_storage[i]=nullptr;
 unsigned callbacks=0;
 auto disposer=[&](auto& p){++callbacks;require(bool(p),"null disposed");p.reset();};
 require(cache.evict_idle_pipelines(2,100,nullptr,nullptr,disposer)==0,"pending placeholders evict valid live pipelines");
 require(callbacks==0 && cache.m_storage.size()==40 && cache.m_storage[1] && cache.m_storage[2],"within-budget contents changed");
 require(cache.evict_idle_pipelines(0,100,nullptr,nullptr,disposer)==0,"disabled limit evicts");
 Cache busy;for(int i=1;i<=9;i++) busy.live(i,i*100);
 auto* keep1=busy.m_storage[1].get();auto* keep2=busy.m_storage[2].get();busy.m_storage[9]->uwp_last_use=now;
 busy.m_storage[50]=nullptr;
 auto busy_disposer=[&](auto& p){require(!busy.m_pipeline_mutex.held,"disposal under lock");require(p && (p->id==3 || p->id==4),"not least recently used");p.reset();++callbacks;};
 require(busy.evict_idle_pipelines(8,10000,keep1,keep2,busy_disposer)==2,"wrong live target");
 require(busy.m_storage.size()==8 && busy.m_storage[1].get()==keep1 && busy.m_storage[2].get()==keep2,"pinned or placeholder lost");
 require(busy.m_storage.count(3)==0 && busy.m_storage.count(4)==0 && busy.m_storage.count(50)==1,"wrong cache entries erased");
 Cache recent;for(int i=1;i<=9;i++) recent.live(i,now);
 require(recent.evict_idle_pipelines(8,10000,nullptr,nullptr,disposer)==0,"recent pipelines evicted");
 Cache empty;require(empty.evict_idle_pipelines(8,0,nullptr,nullptr,disposer)==0,"empty cache failure");
 require(callbacks==2,"unexpected disposal count");
}
'''.replace('@METHOD@',method)
with tempfile.TemporaryDirectory() as tmp:
 p=Path(tmp)/'test.cpp';p.write_text(code);binary=Path(tmp)/'test'
 subprocess.run([args.cxx,'-std=c++20','-Wall','-Wextra','-Werror',str(p),'-o',str(binary)],check=True)
 subprocess.run([str(binary)],check=True)
print('PASS: production eviction excludes placeholders, pins active pipelines, honors LRU/idle/disabled limits, and disposes outside the map lock. No GPU lifetime or performance validation claimed.')
