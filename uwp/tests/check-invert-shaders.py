#!/usr/bin/env python3
"""Compile minimal fragment shaders using the production ROP generator and snippets.

Requires a host C++20 compiler and glslangValidator. No third-party Python modules
or cross-build dependencies are needed. Run before the full Xbox build.
"""
import argparse
from pathlib import Path
import re
import shutil
import subprocess
import tempfile


def section(text, start, end):
    first = text.index(start)
    return text[first:text.index(end, first)]


def block(text, start):
    first = text.index(start)
    brace = text.index("{", first)
    depth = 1
    last = brace + 1
    while depth:
        depth += (text[last] == "{") - (text[last] == "}")
        last += 1
    return text[first:last]


def run(command):
    result = subprocess.run(command, capture_output=True, text=True)
    if result.returncode:
        raise RuntimeError(" ".join(map(str, command)) + "\n" + result.stdout + result.stderr)
    return result.stdout


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--cxx", default="c++", help="host C++20 compiler executable")
    parser.add_argument("--glslang", default="glslangValidator", help="GLSL compiler executable")
    parser.add_argument("--generator-source", type=Path, help="alternate VKFragmentProgram.cpp, for regression reproduction")
    args = parser.parse_args()
    for tool in (args.cxx, args.glslang):
        if not shutil.which(tool):
            parser.error(f"required executable not found: {tool}")

    repo = Path(__file__).resolve().parents[2]
    program = repo / "rpcs3/Emu/RSX/Program"
    generator_path = args.generator_source or repo / "rpcs3/Emu/RSX/VK/VKFragmentProgram.cpp"
    generator = generator_path.read_text()
    common = (program / "GLSLCommon.cpp").read_text()
    header = (program / "GLSLCommon.h").read_text()
    enums = (repo / "rpcs3/Emu/RSX/gcm_enums.h").read_text()

    # Extract production code rather than reconstructing its declaration predicate.
    main_end = generator[generator.index("void VKFragmentDecompilerThread::insertMainEnd"):]
    init_call = "::glsl::insert_rop_init(OS, m_prog.mrt_buffers_count);"
    prefix = main_end[main_end.index('\tOS << "}\\n\\n";'):main_end.index(init_call) + len(init_call)]
    define_constants = block(common, "template <typename T>")
    rop_enum = block(header, "enum ROP_control_bits : u32") + ";"
    rop_defines = section(common, "program_common::define_glsl_constants<rsx::ROP_control_bits>(OS,", "\n\n\t\t\tif (props.ROP_channel_remap)")
    insert_init = block(common, "void insert_rop_init(")
    names = sorted(set(re.findall(r"\bRSX_SHADER_CONTROL_[A-Z_]+\b", prefix)))
    constants = []
    for name in names:
        value = re.search(r"\b" + name + r"\s*=\s*(0x[0-9a-fA-F]+)", enums)
        if not value:
            raise RuntimeError(f"cannot extract shader control value: {name}")
        constants.append(f"constexpr u32 {name} = {value.group(1)};")
    alpha_test = int(re.search(r"RSX_SHADER_CONTROL_ALPHA_TEST\s*=\s*(0x[0-9a-fA-F]+)", enums).group(1), 16)

    harness = r'''
#include <cstdint>
#include <cstdlib>
#include <initializer_list>
#include <iostream>
#include <sstream>
#include <string>
#include <type_traits>
#include <utility>
using u32 = std::uint32_t;
namespace program_common { @DEFINE_CONSTANTS@ }
namespace rsx { @ROP_ENUM@ }
namespace glsl { @INSERT_INIT@ }
@CONSTANTS@
const char* common_glsl =
#include "GLSLSnippets/RSXProg/RSXProgramCommon.glsl"
;
const char* fragment_prologue =
#include "GLSLSnippets/RSXProg/RSXFragmentPrologue.glsl"
;
const char* rop_epilogue =
#include "GLSLSnippets/RSXProg/RSXROPEpilogue.glsl"
;
int main(int argc, char** argv)
{
    if (argc != 4) return 2;
    struct { u32 ctrl; u32 mrt_buffers_count; } m_prog{
        static_cast<u32>(std::strtoul(argv[1], nullptr, 10)),
        static_cast<u32>(std::strtoul(argv[2], nullptr, 10))};
    const bool half = std::strtoul(argv[3], nullptr, 10) != 0;
    std::stringstream OS;
    OS << "#version 450\n";
    if (half) OS << "#extension GL_EXT_shader_explicit_arithmetic_types_float16: require\n";
    else OS << "#define _32_BIT_OUTPUT\n";
    OS << common_glsl;
    @ROP_DEFINES@
    if (m_prog.ctrl & RSX_SHADER_CONTROL_ALPHA_TEST)
        OS << "#define _ENABLE_ALPHA_TEST\n#define _ENABLE_COMPARISON_FUNC\n";
    OS << fragment_prologue;
    OS << "layout(set=0,binding=0) uniform Context {uint rop_control; float alpha_ref;} fs_contexts[1];\n"
          "const uint _fs_context_offset = 0;\n";
    for (u32 index=0; index<4; ++index)
    {
        const char* type = half ? "f16vec4" : "vec4";
        OS << type << " col" << index << " = " << type << "(0.);\n";
        if (index < m_prog.mrt_buffers_count)
            OS << "layout(location=" << index << ") out vec4 ocol" << index << ";\n";
    }
    OS << "void fs_main()\n{\n";
    @GENERATOR_PREFIX@
    OS << "\n\tfs_main();\n" << rop_epilogue << "\n}\n";
    std::cout << OS.str();
}
'''
    for key, value in {
        "DEFINE_CONSTANTS": define_constants,
        "ROP_ENUM": rop_enum,
        "INSERT_INIT": insert_init,
        "CONSTANTS": "\n".join(constants),
        "ROP_DEFINES": rop_defines,
        "GENERATOR_PREFIX": prefix,
    }.items():
        harness = harness.replace("@" + key + "@", value)

    with tempfile.TemporaryDirectory(prefix="rpcs3-invert-shaders-") as tmp:
        folder = Path(tmp)
        cpp = folder / "generator.cpp"
        cpp.write_text(harness)
        checked = 0
        for platform in ("uwp", "desktop"):
            binary = folder / ("generator-" + platform)
            command = [args.cxx, "-std=c++20", "-fno-exceptions", "-Wall", "-Wextra", "-Werror", "-I", str(program)]
            if platform == "uwp":
                command.append("-DRPCS3_UWP")
            run(command + [str(cpp), "-o", str(binary)])
            for mrt in (0, 1, 4):
                for alpha in (False, True):
                    for half in (False, True):
                        name = f"{platform}-mrt{mrt}-alpha{int(alpha)}-fp{16 if half else 32}"
                        shader = folder / (name + ".frag")
                        shader.write_text(run([str(binary), str(alpha_test if alpha else 0), str(mrt), str(int(half))]))
                        run([args.glslang, "-V", "--target-env", "vulkan1.1", "-S", "frag", "-o", str(shader.with_suffix(".spv")), str(shader)])
                        checked += 1
        print(f"PASS: {checked} fragment shaders compiled to SPIR-V using production ROP generation and snippets (UWP/desktop; MRT 0/1/4; alpha off/on; fp32/fp16).")


if __name__ == "__main__":
    main()
