#!/bin/sh
# Compiles the Vulkan version of the shader into ../precompile, needs glslang, SPIRV-Tools, Python and clang-format
set -e
cd "$(dirname "$0")"
out=../precompile

glslangValidator -V -D -S comp -e CSMain --target-env vulkan1.0 -DSGSR1_VK=1 \
    -o "$out/sgsr1_upscale.spv" sgsr1_upscale.hlsl

# glslang gives the output Rgba32f, it's the only storage image here
spirv-dis "$out/sgsr1_upscale.spv" |
    sed -e 's/\(OpTypeImage %float 2D 0 0 0 2\) Rgba32f/\1 Unknown/' \
        -e 's/^\( *\)OpCapability Shader$/&\n\1OpCapability StorageImageWriteWithoutFormat/' |
    spirv-as --target-env vulkan1.0 -o "$out/sgsr1_upscale.spv" -

spirv-val --target-env vulkan1.0 "$out/sgsr1_upscale.spv"
python3 ../../../shaders/shader_tools/create_header.py "$out/sgsr1_upscale.spv" \
    "$out/sgsr1_upscale_Vk.h" sgsr1_upscale_spv
rm "$out/sgsr1_upscale.spv"
clang-format -i --style=file "$out/sgsr1_upscale_Vk.h"
