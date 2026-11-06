#ifndef VBA_WIN32_DISASSEMBLE_H
#define VBA_WIN32_DISASSEMBLE_H

#pragma once

enum DISPLAY_TYPE
{
	GDI         = 0,
	DIRECT_DRAW = 1,
	DIRECT_3D   = 2,
	OPENGL      = 3,
	DIRECT_3D11 = 4,
	OPENGL33    = 5,
	WGPU        = 6
};

// The size "Other Full Screen..." uses: the resolution and colour depth of the
// desktop.  Every renderer presents into a borderless window the size of the
// monitor, so that is the mode there is to pick.
int displayDesktopMode();

class IDisplay
{
public:
	IDisplay() {};
	virtual ~IDisplay() {};

	virtual bool initialize() = 0;
	virtual void cleanup()    = 0;
	virtual void render()     = 0;
	virtual void clear() = 0;
	virtual bool changeRenderSize(int w, int h) { return true; };
	virtual void resize(int w, int h) {};
	virtual void setOption(const char *option, int value) = 0;
	virtual DISPLAY_TYPE getType() = 0;
	virtual int selectFullScreenMode(GUID * *) { return displayDesktopMode(); };

	// Whether this backend can run the upscaler whose UPSCALER_* value is
	// given, which is the question the upscaling menu greys its items out
	// with.  False here: the sampler is the only thing that scales the
	// frame unless a backend says otherwise.
	virtual bool upscalerAvailable(int upscaler) { return false; };

	// Whether this backend can filter the frame with the filter whose value is
	// given - 0 is nearest, 1 is bilinear and 2 is trilinear, the values of the
	// texture filter menu - which is the question that menu greys its items
	// out with.  False here: the backends without a sampler of their own do
	// not filter the frame at all.
	virtual bool filterAvailable(int filter) { return false; };

	// Whether this backend presents the extra, interpolated frame that the
	// frame interpolation menu asks for.  The ones that simply blit the frame
	// buffer with GDI or DirectDraw cannot, so the menu greys its items out
	// there.  See documentation/frame-interpolation.md.
	virtual bool supportsInterpolation() { return false; };
};

#endif // VBA_WIN32_DISASSEMBLE_H
