#!/usr/bin/env python3
"""Exercise production Vulkan missing-pipeline accounting and bounded reporting on a host C++ compiler."""
from pathlib import Path
import argparse, subprocess, tempfile


def block(source, marker):
    start=source.index(marker)
    brace=source.index("{", start)
    depth=1; end=brace+1
    while depth:
        depth+=(source[end]=="{")-(source[end]=="}")
        end+=1
    return source[start:end]


def main():
    parser=argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--cxx",default="c++")
    args=parser.parse_args()
    root=Path(__file__).resolve().parents[2]
    stats=block((root/"rpcs3/Emu/RSX/Core/RSXDisplay.h").read_text(),"struct frame_statistics_t")+";"
    draw=(root/"rpcs3/Emu/RSX/VK/VKDraw.cpp").read_text()
    start=draw.index("bool VKGSRender::end_draw()")
    end=draw.index("\n\t// Load program execution environment",start)
    prefix=draw[start:end]+"\nreturn true;\n}\n"
    thread=(root/"rpcs3/Emu/RSX/RSXThread.cpp").read_text()
    assert "program lookups skipped" not in thread
    assert "debug-only cache-hint lookups elided" in thread
    assert "ctx->stats.uwp_pipeline_draw_attempts, ctx->stats.uwp_pipeline_unavailable_draws" in thread
    code=r'''
#include <cstdint>
#include <cstdlib>
#include <thread>
#include "rpcs3/Emu/RSX/Core/UWPPipelineReadiness.h"
using u32=std::uint32_t;
using s64=std::int64_t;
struct framebuffer_statistics_t {};
@STATS@
static void require(bool condition) { if (!condition) std::abort(); }
struct Context { frame_statistics_t stats{}; struct { int dirty=0; } draw_state; };
struct VKGSRender {
    Context* m_ctx;
    int m_interpreter_state=0;
    bool ready=false;
    int lookups=0;
    bool load_program() { ++lookups; return ready; }
    bool end_draw();
};
@DRAW@
int main() {
    Context ctx;
    VKGSRender renderer{&ctx};
    require(!renderer.end_draw());
    renderer.ready=true;
    require(renderer.end_draw());
    require(renderer.lookups==2);
#ifdef RPCS3_UWP
    require(ctx.stats.uwp_pipeline_draw_attempts==2);
    require(ctx.stats.uwp_pipeline_unavailable_draws==1);
    ctx.stats={};
    require(ctx.stats.uwp_pipeline_draw_attempts==0 && ctx.stats.uwp_pipeline_unavailable_draws==0);
#endif
    rsx::uwp_pipeline_readiness_window window;
    require(!window.record(0,1,8,3));
    require(!window.record(999999,1,100,2));
    auto sample=window.record(1000000,1,4,0);
    require(sample && sample->attempted_batches==112 && sample->unavailable_batches==5 && sample->interval_us==1000000);
    require(!window.record(1100000,2,3,1));
    sample=window.record(2100000,2,4,2);
    require(sample && sample->attempted_batches==7 && sample->unavailable_batches==3);
    require(!window.record(100,2,9,4));
    sample=window.record(1000100,2,1,0);
    require(sample && sample->attempted_batches==10 && sample->unavailable_batches==4);
    require(!window.record(2000100,2,0,0));
    require(!window.record(2000100,2,2,0));
    sample=window.record(3000100,2,3,0);
    require(sample && sample->attempted_batches==5 && sample->unavailable_batches==0);
}
'''
    code=code.replace("@STATS@",stats).replace("@DRAW@",prefix)
    with tempfile.TemporaryDirectory(prefix="rpcs3-readiness-") as directory:
        source=Path(directory)/"test.cpp"; source.write_text(code)
        for label,defines in (("UWP",["-DRPCS3_UWP"]),("desktop",[])):
            executable=Path(directory)/label
            subprocess.run([args.cxx,"-std=c++20","-Wall","-Wextra","-Werror",*defines,"-I",str(root),str(source),"-o",str(executable)],check=True)
            subprocess.run([str(executable)],check=True)
    print("PASS: production draw early-return accounting (UWP/desktop), ready/missing paths, frame reset, one-second aggregation, game transition, clock rollback, empty intervals and zero-miss reporting. No GPU behavior validated.")

if __name__=="__main__": main()
