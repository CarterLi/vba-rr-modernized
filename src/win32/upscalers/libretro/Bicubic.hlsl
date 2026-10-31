// Bicubic, the Mitchell-Netravali resampler of the Direct3D 11 renderer.
//
// Ported from the libretro GLSL shader "bicubic.glsl" (libretro/glsl-shaders,
// cubic/shaders).  The OpenGL 3.3 renderer runs that file as it is upstream,
// from the resource "BICUBIC_GLSL"; this is the port of it, because Direct3D
// compiles HLSL.
//
// It is a separable cubic filter of four taps per axis, taken at texel centres
// and stretched over the whole window, so it is factor agnostic like Lanczos:
// where Lanczos rings at the edges, this one rounds them off.  The kernel is
// Mitchell-Netravali, shaped by the two coefficients below - 1/3 and 1/3 is
// what upstream defaults to, 0.1 and 0.5 is its sharper preset, 0.0 and 0.75
// its sharper still.
//
// At 1x it does not hand the frame back the way Lanczos does: the kernel of a
// texel is 0.89 of it and 0.056 of each neighbour, so a frame drawn at its own
// size comes out slightly soft.  That is the shader, not the port.
//
// The upstream shader is GPL, and the port carries its notice:
//
//   Copyright (C) 2010 Team XBMC
//   http://www.xbmc.org
//   Copyright (C) 2011 Stefanos A.
//   http://www.opentk.com
//
//   This Program is free software; you can redistribute it and/or modify it
//   under the terms of the GNU General Public License as published by the Free
//   Software Foundation; either version 2, or (at your option) any later
//   version.
//
//   This Program is distributed in the hope that it will be useful, but
//   WITHOUT ANY WARRANTY; without even the implied warranty of
//   MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the GNU General
//   Public License for more details.
//
//   You should have received a copy of the GNU General Public License along
//   with this program; if not, write to the Free Software Foundation, 675 Mass
//   Ave, Cambridge, MA 02139, USA.
//   http://www.gnu.org/copyleft/gpl.html

// The quad vertex shader (VSMain) of the renderer feeds this one, so the
// varying has to be declared the way it is there.
struct BicubicVSOut
{
	float4 pos : SV_POSITION;
	float2 uv  : TEXCOORD0;
};

// The block the renderer hands every one of these passes, the one
// Direct3D11Display::scaleFrameWithUpscaler() fills:
//
//   SourceSize.xy: the size the pass reads, zw: one texel (1 / xy)
//   OutputSize.xy: the size of its target, zw: one pixel of it
//   Scale.xy:      OutputSize.xy / SourceSize.xy
//
// The taps are taken at texel centres, which is what the point sampler the
// renderer binds at s0 gives.
cbuffer UpscalerConstants : register(b0)
{
	float4 SourceSize;
	float4 OutputSize;
	float4 Scale;
};

Texture2D    frameTex     : register(t0);
SamplerState frameSampler : register(s0);

// Mitchell-Netravali, the two coefficients of the third degree piecewise
// polynomial of the kernel.  Upstream declares them as parameters and falls
// back to these two numbers when its backend does not hand it any, which is
// the branch the OpenGL renderer compiles as well.
#define BICUBIC_B 0.3333
#define BICUBIC_C 0.3333

// The kernel at the distance x from the centre of it: a cubic of radius two
// that sums to one over the texels it covers.
//
// Upstream writes the square of x as pow(x, 2.0).  pow() of a negative base is
// undefined in HLSL as much as in GLSL, and every other argument here is
// negative - x is 1 minus the fraction the sample sits at, so half of the four
// taps of an axis are handed a negative distance - so the square is written as
// the product it means rather than left to the driver.
float weight(float x)
{
	float ax = abs(x);
	float x2 = x * x;

	if (ax < 1.0)
	{
		return
			(
				x2 * ((12.0 - 9.0 * BICUBIC_B - 6.0 * BICUBIC_C) * ax + (-18.0 + 12.0 * BICUBIC_B + 6.0 * BICUBIC_C)) +
				(6.0 - 2.0 * BICUBIC_B)
			) / 6.0;
	}

	if (ax < 2.0)
	{
		return
			(
				x2 * ((-BICUBIC_B - 6.0 * BICUBIC_C) * ax + (6.0 * BICUBIC_B + 30.0 * BICUBIC_C)) +
				(-12.0 * BICUBIC_B - 48.0 * BICUBIC_C) * ax + (8.0 * BICUBIC_B + 24.0 * BICUBIC_C)
			) / 6.0;
	}

	return 0.0;
}

// The four taps of one axis, for the distance x between the sample and the
// third of them.
float4 weight4(float x)
{
	return float4(weight(x - 2.0),
	              weight(x - 1.0),
	              weight(x),
	              weight(x + 1.0));
}

float3 pixel(float xpos, float ypos)
{
	return frameTex.SampleLevel(frameSampler, float2(xpos, ypos), 0).rgb;
}

// One row of four pixels, weighted by the taps of the other axis.
float3 line_run(float ypos, float4 xpos, float4 linetaps)
{
	return
		pixel(xpos.r, ypos) * linetaps.r +
		pixel(xpos.g, ypos) * linetaps.g +
		pixel(xpos.b, ypos) * linetaps.b +
		pixel(xpos.a, ypos) * linetaps.a;
}

float4 main(BicubicVSOut input) : SV_TARGET
{
	float2 stepxy = SourceSize.zw;
	float2 pos = input.uv + stepxy * 0.5;
	float2 f = frac(pos / stepxy);

	float4 linetaps   = weight4(1.0 - f.x);
	float4 columntaps = weight4(1.0 - f.y);

	// Make sure all taps added together is exactly 1.0, otherwise some (very
	// small) distortion can occur.  The kernel is normalised over the four
	// taps of an axis rather than over its own radius, which is what the two
	// divisions of upstream do as well.
	linetaps   /= linetaps.r + linetaps.g + linetaps.b + linetaps.a;
	columntaps /= columntaps.r + columntaps.g + columntaps.b + columntaps.a;

	// The first of the four taps, which sits two texels left of the texel the
	// sample starts in.
	float2 xystart = (-1.5 - f) * stepxy + pos;
	float4 xpos = float4(xystart.x,
	                     xystart.x + stepxy.x,
	                     xystart.x + stepxy.x * 2.0,
	                     xystart.x + stepxy.x * 3.0);

	float3 rgb = line_run(xystart.y,                      xpos, linetaps) * columntaps.r +
	             line_run(xystart.y + stepxy.y,           xpos, linetaps) * columntaps.g +
	             line_run(xystart.y + stepxy.y * 2.0,     xpos, linetaps) * columntaps.b +
	             line_run(xystart.y + stepxy.y * 3.0,     xpos, linetaps) * columntaps.a;

	// The cubic overshoots its range at an edge, and upstream leaves it to the
	// target to clamp.  Clamping here as well costs nothing and keeps the
	// three ports equal whatever format the target has.
	return float4(saturate(rgb), 1.0);
}
