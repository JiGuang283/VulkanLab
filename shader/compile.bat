@echo off
set GLSLC=%VULKAN_SDK%\Bin\glslc.exe
if not exist "%GLSLC%" set GLSLC=glslc

"%GLSLC%" legacy\forward.vert -o legacy\forward.vert.spv
"%GLSLC%" legacy\forward.frag -o legacy\forward.frag.spv
"%GLSLC%" pbr_lite\forward.vert -o pbr_lite\forward.vert.spv
"%GLSLC%" pbr_lite\forward.frag -o pbr_lite\forward.frag.spv
"%GLSLC%" pbr_lite\forward_normal_mapped.vert -o pbr_lite\forward_normal_mapped.vert.spv
"%GLSLC%" pbr_lite\forward_normal_mapped.frag -o pbr_lite\forward_normal_mapped.frag.spv
"%GLSLC%" material_debug\material.vert -o material_debug\material.vert.spv
"%GLSLC%" material_debug\base_color.frag -o material_debug\base_color.frag.spv
"%GLSLC%" material_debug\normal.frag -o material_debug\normal.frag.spv
"%GLSLC%" material_debug\roughness.frag -o material_debug\roughness.frag.spv
"%GLSLC%" material_debug\metallic.frag -o material_debug\metallic.frag.spv
"%GLSLC%" material_debug\occlusion.frag -o material_debug\occlusion.frag.spv
"%GLSLC%" material_debug\emissive.frag -o material_debug\emissive.frag.spv
"%GLSLC%" material_debug\alpha.frag -o material_debug\alpha.frag.spv
"%GLSLC%" material_debug\transmission.frag -o material_debug\transmission.frag.spv
pause
