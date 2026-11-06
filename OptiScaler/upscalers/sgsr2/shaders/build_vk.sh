#!/bin/sh
# Compiles the Vulkan version of the shaders into ../precompile, needs glslang, SPIRV-Tools and Python
set -e
cd "$(dirname "$0")"
out=../precompile

for name in convert upscale; do
    glslangValidator -V -D -S comp -e CSMain --target-env vulkan1.0 -DSGSR2_VK=1 \
        -o "$out/sgsr2_$name.spv" "sgsr2_$name.hlsl"
done

# Only the game's output is Rgba32f, every other storage image declares its real format
spirv-dis "$out/sgsr2_upscale.spv" |
    sed -e 's/\(OpTypeImage %float 2D 0 0 0 2\) Rgba32f/\1 Unknown/' \
        -e 's/^\( *\)OpCapability Shader$/&\n\1OpCapability StorageImageWriteWithoutFormat/' |
    spirv-as --target-env vulkan1.0 -o "$out/sgsr2_upscale.spv" -

for name in convert upscale; do
    spirv-val --target-env vulkan1.0 "$out/sgsr2_$name.spv"
    python3 ../../../shaders/shader_tools/create_header.py "$out/sgsr2_$name.spv" \
        "$out/sgsr2_${name}_Vk.h" "sgsr2_${name}_spv"
    rm "$out/sgsr2_$name.spv"
done
