Frame upscaling shaders

The Direct3D 11 renderer can rebuild the frame at the window size with either
of two SDKs, or filter it with one of the libretro shaders.  Every shader is
compiled at run time with D3DCompile.  The sources are not compiled into the
project, but stored in the executable as RCDATA resources (see the Upscaler
shaders section of src/win32/vba.rc); the FidelityFX headers are far too large
for a C++ string literal, and resources keep these files readable and diffable
against upstream.  Nothing has to be shipped next to the executable.

The OpenGL 3.3 renderer runs the same FSR and libretro upscalers, from GLSL, and
compiles them with the driver instead, see the GLSL section at the end.  NVIDIA
Image Scaling runs there as well, from the GLSL its own HLSL is translated to below.

The two SDKs are pipelines of their own (a compute shader with a coefficient
texture, and a reconstruction pass plus a sharpening pass).  Everything else is
a row of the table g_upscalers in Direct3D11.cpp: a chain of pixel shader
passes, each of which renders either at a fixed multiple of the frame or at the
size of the target.  A single pass shader is a chain of one.  Adding one is a
row in that table, a resource here, a UPSCALER_* value and a menu item, and the
renderer hands every pass the frame texture (or the output of an earlier pass)
with the whole frame mapped onto the whole window, sampled with the point
sampler.  Adding one needs no code anywhere else.

NVIDIA Image Scaling (NIS)
--------------------------
NIS_Config.h   vendor file, verbatim from the NVIDIA Image Scaling SDK v1.0.3
               https://github.com/NVIDIAGameWorks/NVIDIAImageScaling
               (NIS/NIS_Config.h).  Provides NISConfig, NVScalerUpdateConfig()
               and the coefficient tables the compute shader reads.
NIS_Main.hlsl  vendor file, verbatim (NIS/NIS_Main.hlsl) -> NIS_SHADER_MAIN.
NIS_Scaler.h   vendor file, verbatim (NIS/NIS_Scaler.h) -> NIS_SHADER_SCALER,
               served to the include of NIS_Main.hlsl.
NIS_Main.glsl  ours, but generated rather than written: the same NIS_Main.hlsl
               through glslang's HLSL front end and spirv-cross, edited only for
               the names and the output format.  -> NIS_GLSL_MAIN.  Regenerate
               with .workbuddy-ai/tools/transpile-nis.py.
licence.txt    vendor file, verbatim (licence.txt upstream spelling).

AMD FidelityFX Super Resolution 1.0 (FSR)
-----------------------------------------
ffx_a.h        vendor file, verbatim from
               https://github.com/GPUOpen-Effects/FidelityFX-FSR
               (ffx_a.h, "[A] shader portability 1.20210629") ->
               FSR_SHADER_A.
ffx_fsr1.h     vendor file, verbatim (ffx_fsr1.h) -> FSR_SHADER_FSR1, included
               by FSR_Main.hlsl.
FSR_Main.hlsl  ours.  The two entry points the renderer compiles, "FSR_Easu"
               (the reconstruction pass) and "FSR_Rcas" (the sharpening pass),
               plus the input callbacks FSR asks for.  It defines A_GPU and
               A_HLSL, so it is the 32-bit path of the SDK.
licence-ffx.txt  the copyright notice at the head of both ffx files.

The constants every pass is handed (SinglePassConstants in Direct3D11.cpp, the
same uniforms in OpenGL3.cpp), the way its source declares them:

    float4 SourceSize;  // xy: the size the pass reads, zw: 1 / xy (one texel)
    float4 OutputSize;  // xy: the target size, zw: 1 / xy
    float4 Scale;       // xy: OutputSize.xy / SourceSize.xy

libretro shaders
----------------
Everything here is in libretro/, the sources and the ports side by side, so that
a shader and the HLSL that came out of it stay together.

The .glsl files are verbatim from https://github.com/libretro/glsl-shaders,
master ce9a412.  The OpenGL 3.3 renderer compiles them as they are, both stages
from the one file (they carry the vertex stage and the pixel stage, selected by
the VERTEX and FRAGMENT macros), and plays the part of RetroArch's shader
backend for them: the attributes, the uniforms and the textures of the passes
before them.  The .hlsl files are ours, and each one names the file it came
from at its head.

libretro/XBRZ_Freescale.glsl        -> XBRZ_GLSL_FREESCALE       xbrz/shaders
libretro/XBRZ_Freescale.hlsl        -> XBRZ_SHADER_FREESCALE
libretro/XbrzFreescale.wgsl         -> XBRZ_FREESCALE_WGSL
libretro/xbrz-freescale.glsl        the upstream file both of them came from,
                                    kept for comparison; nothing compiles it
libretro/xbr-lv2.glsl               -> XBR_LV2_GLSL              xbr/shaders
libretro/XBR_Lv2.hlsl               -> XBR_LV2_SHADER
libretro/xbr-lv2-noblend.glsl       -> XBR_LV2_NOBLEND_GLSL      xbr/shaders
libretro/XBR_Lv2NoBlend.hlsl        -> XBR_LV2_NOBLEND_SHADER
libretro/xbr-lv3.glsl               -> XBR_LV3_GLSL              xbr/shaders
libretro/XBR_Lv3.hlsl               -> XBR_LV3_SHADER
libretro/xbr-lv3-pass0.glsl         -> XBR_LV3_PASS0_GLSL        xbr/shaders
libretro/XBR_Lv3_Pass0.hlsl         -> XBR_LV3_PASS0_SHADER
libretro/xbr-lv3-pass1.glsl         -> XBR_LV3_PASS1_GLSL        xbr/shaders
libretro/XBR_Lv3_Pass1.hlsl         -> XBR_LV3_PASS1_SHADER
libretro/XbrLv3Multipass0.wgsl      -> XBR_LV3_PASS0_WGSL
libretro/XbrLv3Multipass1.wgsl      -> XBR_LV3_PASS1_WGSL
libretro/2xbr-lv1-c-pass0.glsl      -> XBR_2X_PASS0_GLSL         xbr/shaders
libretro/XBR_2x_Pass0.hlsl          -> XBR_2X_PASS0_SHADER
libretro/Xbr2xPass0.wgsl            -> XBR_2X_PASS0_WGSL
libretro/2xbr-lv1-c-pass1.glsl      -> XBR_2X_PASS1_GLSL         xbr/shaders
libretro/XBR_2x_Pass1.hlsl          -> XBR_2X_PASS1_SHADER
libretro/Xbr2xPass1.wgsl            -> XBR_2X_PASS1_WGSL
libretro/xbr-mlv4-pass1..4.glsl     -> XBR_MLV4_PASS1..4_GLSL    xbr/shaders
libretro/XBR_Mlv4_Pass1..4.hlsl     -> XBR_MLV4_PASS1..4_SHADER
libretro/XbrMlv4Pass1..4.wgsl       -> XBR_MLV4_PASS1..4_WGSL
libretro/super-xbr-pass0.glsl       -> SUPER_XBR_PASS0_GLSL      super-xbr/shaders
libretro/SuperXBR_Pass0.hlsl        -> SUPER_XBR_PASS0_SHADER
libretro/SuperXbrPass0.wgsl         -> SUPER_XBR_PASS0_WGSL
libretro/super-xbr-pass1.glsl       -> SUPER_XBR_PASS1_GLSL      super-xbr/shaders
libretro/SuperXBR_Pass1.hlsl        -> SUPER_XBR_PASS1_SHADER
libretro/SuperXbrPass1.wgsl         -> SUPER_XBR_PASS1_WGSL
libretro/custom-jinc2-sharper.glsl  -> CUSTOM_JINC2_SHARPER_GLSL super-xbr/shaders
libretro/SuperXBR_Jinc2.hlsl        -> SUPER_XBR_JINC2_SHADER
libretro/SuperXbrJinc2.wgsl         -> SUPER_XBR_JINC2_WGSL
libretro/scalefx-pass0..4.glsl      -> SCALEFX_PASS0..4_GLSL     scalefx/shaders
libretro/ScaleFX_Pass0..4.hlsl      -> SCALEFX_PASS0..4_SHADER
libretro/ScaleFxPass0..4.wgsl       -> SCALEFX_PASS0..4_WGSL
libretro/PT_SkyWalker541.glsl       -> PT_SKYWALKER541_GLSL      handheld/shaders
libretro/PT_SkyWalker541.hlsl       -> PT_SKYWALKER541_SHADER
libretro/PT_SkyWalker541.wgsl       -> PT_SKYWALKER541_WGSL
libretro/lanczos3.glsl              -> LANCZOS3_GLSL             windowed/shaders
libretro/Lanczos3.hlsl              -> LANCZOS3_SHADER
libretro/Lanczos3.wgsl              -> LANCZOS3_WGSL
libretro/bicubic.glsl               -> BICUBIC_GLSL              cubic/shaders
libretro/Bicubic.hlsl               -> BICUBIC_SHADER
libretro/Bicubic.wgsl               -> BICUBIC_WGSL
libretro/XbrLv2.wgsl                -> XBR_LV2_WGSL              xbr/shaders
libretro/XbrLv2NoBlend.wgsl         -> XBR_LV2_NOBLEND_WGSL      xbr/shaders
libretro/XbrLv3.wgsl                -> XBR_LV3_WGSL              xbr/shaders
libretro/stock.glsl                 the skeleton of the directory, unused

The chains, in the order the presets of those directories run them:

    xBRZ                                 1 pass,  at the target size
    xBR lv2, xBR lv2 no-blend, xBR lv3   1 pass,  at the target size
    xBR lv3 multipass                    1x, then the target size
    2xBR                                 1x, then the target size
    xBR-mlv4                             1x, 1x, 2x, then the target size
    super-xBR                            1x, 2x, then the target size
    ScaleFX                              1x four times, then the target size
    PT SkyWalker541                      1 pass,  at the target size
    Lanczos                              1 pass,  at the target size
    Bicubic                              1 pass,  at the target size

Both renderers run all of them.  A pass that reads a second texture names it
in the table of Direct3D11.cpp as an index of the outputs of the chain - 0 is
the frame, n the target of pass n - 1 - and the size of that texture is handed
to the shader as SecondSize, which is where a GLSL shader reads
OrigTextureSize and PassPrev<N>TextureSize.  The OpenGL backend resolves the
same names from the uniforms the pass declares.

The WebGPU renderer runs every chain, from WGSL: wgpu compiles neither GLSL nor
HLSL, so the .wgsl files beside the other two are ports written against the same
upstream files rather than translations - one per pass, the same names and the
same expressions.  The chains that read a second texture - xBR-mlv4, super-xBR,
ScaleFX, 2xBR and xBR lv3 multipass - carry a second binding, and a row of
g_wgpuUpscalers lists, per pass, the output it reads besides the one the pass
before it made, 0 being the frame.
Their taps are read with textureLoad and integer coordinates instead of
through a sampler, which is exact where a tap lands on a texel centre and
cheaper than a filtered fetch.  The chain there draws its last pass straight
into the back buffer when it comes out at its size, so it pays no blit of its
own.

PT_SkyWalker541.wgsl reads the frame through both of the textures upstream
samples, because the second one - the raw frame, before the palette - is the
frame itself here: a single pass chain binds it once, at binding 0.

The ports are written the way the upstream files are rather than transcribed by
a translator, so that they stay diffable against them: same names, same
expressions, same order, with the bvec4 algebra of GLSL spelled out in floats
and the taps that upstream computes in its vertex stage computed in the pixel
shader instead, because the renderer has one quad vertex shader for every
upscaler.  Where a pass takes its taps with textureOffset() it needs neither.
The xBR and ScaleFX sources are MIT, Hyllian's and Sp00kyFox's respectively, and
each port carries the notice of the shader it came from; bicubic.glsl is GPL,
Team XBMC's and Stefanos A.'s, and its two ports carry that notice in full;
PT_SkyWalker541.glsl carries no licence statement upstream, only its credit, and
lanczos3.glsl neither.

The first ScaleFX and PT ports were produced by translating the GLSL with
glslang and SPIRV-Cross, then given the same treatment by hand: the names, the
comments, the constant block, and one entry point that takes a uv.  Their
bodies are still in the shape the translator left them, which is why those five
files read differently from the xBR ones.

GLSL, for the OpenGL 3.3 renderer
---------------------------------
The ports, which the driver compiles while the renderer starts up.  GLSL has no
#include, so OpenGL3.cpp splices the resources into each other itself; the
FidelityFX headers above are the same resources, so both backends hand the same
bits to the same AMD code.

FSR_Body.glsl   ours.  The FSR_Main.hlsl wrapper in GLSL: the input callbacks
                and the uniforms, with no #version and no entry point of its
                own, so that one body serves the two passes.  -> FSR_GLSL_BODY.
FSR_Easu.glsl   ours.  The reconstruction pass, #version 420 core because the
                AMD headers use the implicit integer conversions and the
                bitfield operations of that version.  -> FSR_GLSL_EASU.
FSR_Rcas.glsl   ours.  The sharpening pass, the same version.  -> FSR_GLSL_RCAS.
NIS_Main.glsl   ours by translation, #version 430 core compute.  The samplers
                are inputTexture (the pass reads, bilinear and clamped),
                coefScalerTexture and coefUsmTexture (the banks, texelFetch),
                and the pass writes out_texture, an rgba8 image at binding 0,
                with the NISConfig block at binding 0 as well.  Where the
                context is older than 4.3 the renderer leaves NIS out.

The uniforms the FSR passes are handed are the block Direct3D 11 puts in its
constant buffer, as bits rather than as floats, plus the size of the pass input
for RCAS to clamp its taps with:

    uvec4 con0, con1, con2, con3;  // EASU, from FsrEasuCon()
    uvec4 conRcas;                 // RCAS, from FsrRcasCon()
    uvec4 sizeInfo;                // xy: the EASU target size, as integers

Rebuilding: vba.rc pulls the files above in as RCDATA resources, and the build
tracks them as inputs, so editing one is enough - MSBuild then recompiles the
resource script and relinks.
