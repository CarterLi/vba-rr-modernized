// OpenGLConfig.h: the compile time switch for the OpenGL display backends.
//
// VBA_ENABLE_OPENGL is what the "#if" in the places the renderers touch asks
// about: set to 1, the two OpenGL renderers are built and the Video menu
// carries their entries; set to 0, none of it is compiled and the menu items
// leave the resource with it.
//
// There are two renderers behind the switch: the fixed function one in
// OpenGL.cpp, which the menu calls "OpenGL (Legacy)", and the shader
// pipeline one in OpenGL3.cpp, which it calls "OpenGL 3.3".  Both of them
// run on opengl32.dll, which every Windows has, so nothing has to be shipped
// next to the executable for them.
//
// The GLSL sources the OpenGL 3.3 renderer compiles are only in the
// executable when it is built, see the switch in vba.rc: the Direct3D 11
// backend runs the HLSL ports of the same upscalers and the WebGPU one the
// WGSL ones, so a build without this backend does not need them either.
//
// The renderers build on every platform the project has, x86, x64 and ARM64,
// and nothing else in the tree refers to the switch.

#ifndef VBA_WIN32_OPENGLCONFIG_H
#define VBA_WIN32_OPENGLCONFIG_H

#pragma once

#ifndef VBA_ENABLE_OPENGL
#define VBA_ENABLE_OPENGL 1
#endif

#endif // VBA_WIN32_OPENGLCONFIG_H
