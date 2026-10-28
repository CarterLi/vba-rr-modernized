#include "stdafx.h"
#include "resource.h"
#include "MainWnd.h"
#include "WinResUtil.h"
#include "Reg.h"
#include "UiFont.h"
#include "VBA.h"
#include "Dialogs/Associate.h"
#include "Dialogs/Directories.h"
#include "Dialogs/FileDlg.h"
#include "Dialogs/GBColorDlg.h"
#include "Dialogs/Joypad.h"
#include "Dialogs/MaxScale.h"
#include "Dialogs/ModeConfirm.h"
#include "Dialogs/RewindInterval.h"
#include "Dialogs/Throttle.h"
#include "Dialogs/TextOptions.h"
#include "Input.h"

#include "../gba/GBA.h"
#include "../gba/GBAGlobals.h"
#include "../gba/Flash.h"
#include "../gba/agbprint.h"
#include "../gb/GB.h"
#include "../gb/gbGlobals.h"
#include "../gb/gbPrinter.h"
#include "../common/System.h"
#include "../common/SystemGlobals.h"
#include "../common/inputGlobal.h"
#include "../common/movie.h"
#include "../common/ImageWrite.h"
#include "../version.h"

// Defined in Direct3D11.cpp: false when D3D11.DLL or D3DCOMPILER_47.DLL is
// missing, in which case the renderer is not offered at all.
extern bool direct3D11Available();
#if VBA_ENABLE_WGPU
// Defined in Wgpu.cpp: false when wgpu_native.dll is missing or does not
// carry the entry points the renderer is built against, in which case the
// renderer is not offered at all.  wgpuBackendModel names the backend the
// running renderer presents with ("D3D12", "Vulkan", ...), NULL while it is
// not the render method that is presenting.
extern bool wgpuAvailable();
extern const char *wgpuBackendModel();
// Defined in Wgpu.cpp: whether the backend the menu names is one this machine
// has an adapter for, so the ones it has not are greyed out.
extern bool wgpuBackendAvailable(int backend);
#endif
#if VBA_ENABLE_OPENGL
// Defined in OpenGL3.cpp: the core profile version the OpenGL 3.3 renderer
// created, empty while it is not the render method that is presenting.  The
// renderer asks for the newest core version the driver has and descends from
// there, so this is what was actually created, see OpenGL3.cpp.
extern const char *openGL33Version();
#endif
// Defined in Direct3D11.cpp: the presentation model the Direct3D 11 backend is
// running with ("Flip", "Flip sequential", "Bitblt"), NULL while it is not the
// render method that is presenting.
extern const char *direct3D11PresentationModel();
// Defined in Direct3D.cpp: the same for the Direct3D 9Ex backend ("Flip",
// "Bitblt").
extern const char *direct3D9PresentationModel();
// Defined in Direct3D.cpp: which Direct3D 9 flavor the device that is running
// really is ("9Ex", or "9.0c" where Direct3DCreate9Ex is not there), NULL while
// it is not the render method that is presenting.
extern const char *direct3D9DeviceName();
// Defined in XAudio2Sound.cpp: how the running XAudio2 backend is buffering and
// how often it was left without data, for the sound API menu below.  All zero
// while no XAudio2 backend is running.
extern int xaudio2BufferCount();
extern int xaudio2BufferingMs();
extern int xaudio2DropoutCount();
// Defined in DirectSound.cpp: the same for its ring, which is fixed at ten
// blocks, of which the one being played is not in flight; all zero while no
// DirectSound backend is running.
extern int directSoundBufferCount();
extern int directSoundBufferingMs();

// Names the presentation model that a render method entry is running with, so
// that "Direct3D 11" reads "Direct3D 11 (Flip)".  The plain name comes from the
// menu item itself (the resource carries it) and the accelerator text is left to
// winAccelMgr, so nothing here has to be kept in step with the resources by
// hand.  device, when given, is the Direct3D 9 flavor that is really in use;
// the entry is named after it because "Direct3D 9Ex" would be a lie on a system
// where Direct3DCreate9Ex does not exist and the plain 9.0c device is used.
static void setRenderMethodModel(CCmdUI *pCmdUI, const char *model, const char *device = NULL)
{
	if (pCmdUI->m_pMenu == NULL)
		return;

	CString text;
	pCmdUI->m_pMenu->GetMenuString(pCmdUI->m_nID, text, MF_BYCOMMAND);

	// Drop the accelerator text winAccelMgr appends and a model named by an
	// earlier update, then add what is running now.  The list has to carry
	// every model this function can name, or the entry grows one suffix per
	// menu update - the WebGPU backends were missing from it and the entry
	// read "WebGPU (D3D12) (D3D12) (D3D12)" after three of them.
	int tab = text.Find('\t');
	if (tab >= 0)
		text = text.Left(tab);

	const char *models[] =
	{
		" (Flip)", " (Flip sequential)", " (Bitblt)",
		" (D3D12)", " (Vulkan)", " (Metal)", " (OpenGL)"
	};

	bool stripped = true;
	while (stripped)
	{
		stripped = false;

		for (int i = 0; i < (int)(sizeof(models) / sizeof(models[0])); i++)
		{
			CString suffix(models[i]);

			if (text.GetLength() > suffix.GetLength() &&
			    text.Right(suffix.GetLength()) == suffix)
			{
				text = text.Left(text.GetLength() - suffix.GetLength());
				stripped = true;
				break;
			}
		}
	}

	if (device != NULL)
	{
		// Undo the flavor an earlier update substituted before naming this one.
		text.Replace(_T("9.0c"), _T("9Ex"));
		if (strcmp(device, "9Ex") != 0)
			text.Replace(_T("9Ex"), winWideFromAnsi(device));
	}

	// The model arrives as a narrow string from every renderer, so the format
	// has to name its width the way the GL version entry below does; a plain
	// %s in a wide format reads the bytes as UTF-16 and the entry shows mojibake.
	if (model != NULL)
		text.AppendFormat(_T(" (%hs)"), model);

	pCmdUI->SetText(text);
	theApp.winAccelMgr.UpdateMenu(pCmdUI->m_pMenu->GetSafeHmenu());
}

#if VBA_ENABLE_OPENGL
// Names the core profile version the OpenGL 3.3 renderer is running with, so
// that "OpenGL 3.3" reads "OpenGL 3.3 (4.6 core)".  The request descends from
// the newest core version the driver has, so what is printed is what was
// actually created rather than what the entry asks for; the shape is the same
// as setRenderMethodModel above.
static void setRenderMethodGlVersion(CCmdUI *pCmdUI, const char *version)
{
	if (pCmdUI->m_pMenu == NULL)
		return;

	CString text;
	pCmdUI->m_pMenu->GetMenuString(pCmdUI->m_nID, text, MF_BYCOMMAND);

	// Drop the accelerator text winAccelMgr appends and a version named by an
	// earlier update, then add what is running now.
	int tab = text.Find('\t');
	if (tab >= 0)
		text = text.Left(tab);

	int paren = text.Find(_T(" ("));
	if (paren >= 0)
		text = text.Left(paren);

	if (version != NULL)
		text.AppendFormat(_T(" (%hs)"), version);

	pCmdUI->SetText(text);
	theApp.winAccelMgr.UpdateMenu(pCmdUI->m_pMenu->GetSafeHmenu());
}
#endif // VBA_ENABLE_OPENGL

// Names the buffering the backend this entry stands for is running with, so
// that "XAudio2 (2.9)" reads "XAudio2 (2.9) - 4 blocks, 67 ms" and the
// DirectSound entry next to it reads the same way for its ring; a machine that
// cannot keep those blocks filled shows how often the backend was left without
// data.  The blocks times the frame they hold are the latency that backend adds
// on top of its own processing quantum, which is what XAudio2 buys over the
// DirectSound ring; see XAudio2Sound.cpp and DirectSound.cpp.  The plain name
// comes from the resource and the accelerator text is left to winAccelMgr, like
// setRenderMethodModel above.
static void setSoundApiBuffering(CCmdUI *pCmdUI, int blocks, int ms, int dropouts)
{
	if (pCmdUI->m_pMenu == NULL)
		return;

	CString text;
	pCmdUI->m_pMenu->GetMenuString(pCmdUI->m_nID, text, MF_BYCOMMAND);

	int tab = text.Find('\t');
	if (tab >= 0)
		text = text.Left(tab);

	// Drop a buffering named by an earlier update before naming this one.
	int dash = text.Find(_T(" - "));
	if (dash >= 0)
		text = text.Left(dash);

	if (blocks > 0)
	{
		text.AppendFormat(_T(" - %d blocks, %d ms"), blocks, ms);

		if (dropouts > 0)
			text.AppendFormat(_T(", %d dropouts"), dropouts);
	}

	pCmdUI->SetText(text);
	theApp.winAccelMgr.UpdateMenu(pCmdUI->m_pMenu->GetSafeHmenu());
}

#define VBA_CONFIRM_MODE WM_APP + 100

void MainWnd::OnOptionsFrameskipThrottleNothrottle()
{
	systemSetThrottle(0);
}

void MainWnd::OnUpdateOptionsFrameskipThrottleNothrottle(CCmdUI*pCmdUI)
{
	setRadioCheck(pCmdUI, theApp.throttle == 0);
}

void MainWnd::OnOptionsFrameskipThrottle6()
{
	systemSetThrottle(6);
}

void MainWnd::OnUpdateOptionsFrameskipThrottle6(CCmdUI*pCmdUI)
{
	setRadioCheck(pCmdUI, theApp.throttle == 6);
}

void MainWnd::OnOptionsFrameskipThrottle15()
{
	systemSetThrottle(15);
}

void MainWnd::OnUpdateOptionsFrameskipThrottle15(CCmdUI*pCmdUI)
{
	setRadioCheck(pCmdUI, theApp.throttle == 15);
}

void MainWnd::OnOptionsFrameskipThrottle25()
{
	systemSetThrottle(25);
}

void MainWnd::OnUpdateOptionsFrameskipThrottle25(CCmdUI*pCmdUI)
{
	setRadioCheck(pCmdUI, theApp.throttle == 25);
}

void MainWnd::OnOptionsFrameskipThrottle50()
{
	systemSetThrottle(50);
}

void MainWnd::OnUpdateOptionsFrameskipThrottle50(CCmdUI*pCmdUI)
{
	setRadioCheck(pCmdUI, theApp.throttle == 50);
}

void MainWnd::OnOptionsFrameskipThrottle75()
{
	systemSetThrottle(75);
}

void MainWnd::OnUpdateOptionsFrameskipThrottle75(CCmdUI*pCmdUI)
{
	setRadioCheck(pCmdUI, theApp.throttle == 75);
}

void MainWnd::OnOptionsFrameskipThrottle100()
{
	systemSetThrottle(100);
}

void MainWnd::OnUpdateOptionsFrameskipThrottle100(CCmdUI*pCmdUI)
{
	setRadioCheck(pCmdUI, theApp.throttle == 100);
}

void MainWnd::OnOptionsFrameskipThrottle125()
{
	systemSetThrottle(125);
}

void MainWnd::OnUpdateOptionsFrameskipThrottle125(CCmdUI*pCmdUI)
{
	setRadioCheck(pCmdUI, theApp.throttle == 125);
}

void MainWnd::OnOptionsFrameskipThrottle150()
{
	systemSetThrottle(150);
}

void MainWnd::OnUpdateOptionsFrameskipThrottle150(CCmdUI*pCmdUI)
{
	setRadioCheck(pCmdUI, theApp.throttle == 150);
}

void MainWnd::OnOptionsFrameskipThrottle200()
{
	systemSetThrottle(200);
}

void MainWnd::OnUpdateOptionsFrameskipThrottle200(CCmdUI*pCmdUI)
{
	setRadioCheck(pCmdUI, theApp.throttle == 200);
}

void MainWnd::OnOptionsFrameskipThrottle300()
{
	systemSetThrottle(300);
}

void MainWnd::OnUpdateOptionsFrameskipThrottle300(CCmdUI*pCmdUI)
{
	setRadioCheck(pCmdUI, theApp.throttle == 300);
}

void MainWnd::OnOptionsFrameskipThrottle400()
{
	systemSetThrottle(400);
}

void MainWnd::OnUpdateOptionsFrameskipThrottle400(CCmdUI*pCmdUI)
{
	setRadioCheck(pCmdUI, theApp.throttle == 400);
}

void MainWnd::OnOptionsFrameskipThrottle600()
{
	systemSetThrottle(600);
}

void MainWnd::OnUpdateOptionsFrameskipThrottle600(CCmdUI*pCmdUI)
{
	setRadioCheck(pCmdUI, theApp.throttle == 600);
}

void MainWnd::OnOptionsFrameskipThrottle1000()
{
	systemSetThrottle(1000);
}

void MainWnd::OnUpdateOptionsFrameskipThrottle1000(CCmdUI*pCmdUI)
{
	setRadioCheck(pCmdUI, theApp.throttle == 1000);
}

void MainWnd::OnOptionsFrameskipThrottleOther()
{
	Throttle dlg;
	int      v = (int)dlg.DoModal();
	if (v)
		systemSetThrottle(v);
}

void MainWnd::OnUpdateOptionsFrameskipThrottleOther(CCmdUI*pCmdUI)
{
}

void MainWnd::OnOptionsFrameskipThrottleIncrease()
{
	systemIncreaseThrottle();
}

void MainWnd::OnUpdateOptionsFrameskipThrottleIncrease(CCmdUI*pCmdUI)
{
}

void MainWnd::OnOptionsFrameskipThrottleDecrease()
{
	systemDecreaseThrottle();
}

void MainWnd::OnUpdateOptionsFrameskipThrottleDecrease(CCmdUI*pCmdUI)
{
}

/*
   void MainWnd::OnOptionsFrameskipAutomatic()
   {
   theApp.autoFrameSkip = !theApp.autoFrameSkip;
   if(!theApp.autoFrameSkip && emulating)
    theApp.updateFrameSkip();
   }

   void MainWnd::OnUpdateOptionsFrameskipAutomatic(CCmdUI* pCmdUI)
   {
   pCmdUI->SetCheck(theApp.autoFrameSkip);
   }
 */

void MainWnd::OnOptionsFrameskipAccuratePitch()
{
	theApp.accuratePitchThrottle = true;
}

void MainWnd::OnUpdateOptionsFrameskipAccuratePitch(CCmdUI*pCmdUI)
{
	pCmdUI->SetCheck(theApp.accuratePitchThrottle);
	pCmdUI->Enable(!soundOffFlag && synchronize);
}

void MainWnd::OnOptionsFrameskipAccurateSpeed()
{
	theApp.accuratePitchThrottle = false;
}

void MainWnd::OnUpdateOptionsFrameskipAccurateSpeed(CCmdUI*pCmdUI)
{
	pCmdUI->SetCheck(!theApp.accuratePitchThrottle);
	pCmdUI->Enable(!soundOffFlag && synchronize);
}

BOOL MainWnd::OnOptionsFrameskip(UINT nID)
{
	switch (nID)
	{
	case ID_OPTIONS_VIDEO_FRAMESKIP_0:
	case ID_OPTIONS_VIDEO_FRAMESKIP_1:
	case ID_OPTIONS_VIDEO_FRAMESKIP_2:
	case ID_OPTIONS_VIDEO_FRAMESKIP_3:
	case ID_OPTIONS_VIDEO_FRAMESKIP_4:
	case ID_OPTIONS_VIDEO_FRAMESKIP_5:
	case ID_OPTIONS_VIDEO_FRAMESKIP_6:
	case ID_OPTIONS_VIDEO_FRAMESKIP_7:
	case ID_OPTIONS_VIDEO_FRAMESKIP_8:
	case ID_OPTIONS_VIDEO_FRAMESKIP_9:
		if (systemCartridgeType == IMAGE_GBA)
		{
			frameSkip = nID - ID_OPTIONS_VIDEO_FRAMESKIP_0;
		}
		else
		{
			gbFrameSkip = nID - ID_OPTIONS_VIDEO_FRAMESKIP_0;
		}
		if (emulating)
			theApp.updateFrameSkip();
		return TRUE;
		break;
	}
	return FALSE;
}

void MainWnd::OnUpdateOptionsVideoFrameskip0(CCmdUI*pCmdUI)
{
	setRadioCheck(pCmdUI, systemCartridgeType == IMAGE_GBA ? frameSkip == 0 : gbFrameSkip == 0);
}

void MainWnd::OnUpdateOptionsVideoFrameskip1(CCmdUI*pCmdUI)
{
	setRadioCheck(pCmdUI, systemCartridgeType == IMAGE_GBA ? frameSkip == 1 : gbFrameSkip == 1);
}

void MainWnd::OnUpdateOptionsVideoFrameskip2(CCmdUI*pCmdUI)
{
	setRadioCheck(pCmdUI, systemCartridgeType == IMAGE_GBA ? frameSkip == 2 : gbFrameSkip == 2);
}

void MainWnd::OnUpdateOptionsVideoFrameskip3(CCmdUI*pCmdUI)
{
	setRadioCheck(pCmdUI, systemCartridgeType == IMAGE_GBA ? frameSkip == 3 : gbFrameSkip == 3);
}

void MainWnd::OnUpdateOptionsVideoFrameskip4(CCmdUI*pCmdUI)
{
	setRadioCheck(pCmdUI, systemCartridgeType == IMAGE_GBA ? frameSkip == 4 : gbFrameSkip == 4);
}

void MainWnd::OnUpdateOptionsVideoFrameskip5(CCmdUI*pCmdUI)
{
	setRadioCheck(pCmdUI, systemCartridgeType == IMAGE_GBA ? frameSkip == 5 : gbFrameSkip == 5);
}

void MainWnd::OnUpdateOptionsVideoFrameskip6(CCmdUI*pCmdUI)
{
	setRadioCheck(pCmdUI, systemCartridgeType == IMAGE_GBA ? frameSkip == 6 : gbFrameSkip == 6);
}

void MainWnd::OnUpdateOptionsVideoFrameskip7(CCmdUI*pCmdUI)
{
	setRadioCheck(pCmdUI, systemCartridgeType == IMAGE_GBA ? frameSkip == 7 : gbFrameSkip == 7);
}

void MainWnd::OnUpdateOptionsVideoFrameskip8(CCmdUI*pCmdUI)
{
	setRadioCheck(pCmdUI, systemCartridgeType == IMAGE_GBA ? frameSkip == 8 : gbFrameSkip == 8);
}

void MainWnd::OnUpdateOptionsVideoFrameskip9(CCmdUI*pCmdUI)
{
	setRadioCheck(pCmdUI, systemCartridgeType == IMAGE_GBA ? frameSkip == 9 : gbFrameSkip == 9);
}

void MainWnd::OnOptionsVideoVsync()
{
	theApp.vsync = !theApp.vsync;
}

void MainWnd::OnUpdateOptionsVideoVsync(CCmdUI*pCmdUI)
{
	pCmdUI->SetCheck(theApp.vsync);
}

void MainWnd::OnUpdateOptionsVideoX1(CCmdUI*pCmdUI)
{
	setRadioCheck(pCmdUI, theApp.videoOption == VIDEO_1X);
}

void MainWnd::OnUpdateOptionsVideoX2(CCmdUI*pCmdUI)
{
	setRadioCheck(pCmdUI, theApp.videoOption == VIDEO_2X);
}

void MainWnd::OnUpdateOptionsVideoX3(CCmdUI*pCmdUI)
{
	setRadioCheck(pCmdUI, theApp.videoOption == VIDEO_3X);
}

void MainWnd::OnUpdateOptionsVideoX4(CCmdUI*pCmdUI)
{
	setRadioCheck(pCmdUI, theApp.videoOption == VIDEO_4X);
}

void MainWnd::OnUpdateOptionsVideoX5(CCmdUI*pCmdUI)
{
	setRadioCheck(pCmdUI, theApp.videoOption == VIDEO_5X);
}

void MainWnd::OnUpdateOptionsVideoX6(CCmdUI*pCmdUI)
{
	setRadioCheck(pCmdUI, theApp.videoOption == VIDEO_6X);
}

void MainWnd::OnUpdateOptionsVideoX7(CCmdUI*pCmdUI)
{
	setRadioCheck(pCmdUI, theApp.videoOption == VIDEO_7X);
}

void MainWnd::OnUpdateOptionsVideoX8(CCmdUI*pCmdUI)
{
	setRadioCheck(pCmdUI, theApp.videoOption == VIDEO_8X);
}

void MainWnd::OnUpdateOptionsVideoX9(CCmdUI*pCmdUI)
{
	setRadioCheck(pCmdUI, theApp.videoOption == VIDEO_9X);
}

void MainWnd::OnUpdateOptionsVideoX10(CCmdUI*pCmdUI)
{
	setRadioCheck(pCmdUI, theApp.videoOption == VIDEO_10X);
}

void MainWnd::OnUpdateOptionsVideoX11(CCmdUI*pCmdUI)
{
	setRadioCheck(pCmdUI, theApp.videoOption == VIDEO_11X);
}

void MainWnd::OnUpdateOptionsVideoX12(CCmdUI*pCmdUI)
{
	setRadioCheck(pCmdUI, theApp.videoOption == VIDEO_12X);
}

// The three fixed full screen sizes are window sizes like any other, so they
// are always available: nothing has to be mode-set for them.
void MainWnd::OnUpdateOptionsVideoFullscreen320x240(CCmdUI*pCmdUI)
{
	setRadioCheck(pCmdUI, theApp.videoOption == VIDEO_320x240);
}

void MainWnd::OnUpdateOptionsVideoFullscreen640x480(CCmdUI*pCmdUI)
{
	setRadioCheck(pCmdUI, theApp.videoOption == VIDEO_640x480);
}

void MainWnd::OnUpdateOptionsVideoFullscreen800x600(CCmdUI*pCmdUI)
{
	setRadioCheck(pCmdUI, theApp.videoOption == VIDEO_800x600);
}

BOOL MainWnd::OnOptionVideoSize(UINT nID)
{
	theApp.updateVideoSize(nID);
	theApp.m_pMainWnd->PostMessage(VBA_CONFIRM_MODE);
	return TRUE;
}

void MainWnd::OnOptionsVideoFullscreen320x240()
{
	OnOptionVideoSize(ID_OPTIONS_VIDEO_FULLSCREEN320X240);
}

void MainWnd::OnOptionsVideoFullscreen640x480()
{
	OnOptionVideoSize(ID_OPTIONS_VIDEO_FULLSCREEN640X480);
}

void MainWnd::OnOptionsVideoFullscreen800x600()
{
	OnOptionVideoSize(ID_OPTIONS_VIDEO_FULLSCREEN800X600);
}

void MainWnd::OnOptionsVideoFullscreen()
{
	GUID *pGUID = NULL;
	int   size  = theApp.display->selectFullScreenMode(&pGUID);
	if (size != -1)
	{
		int width      = (size >> 12) & 4095;
		int height     = (size & 4095);
		int colorDepth = (size >> 24);
		if (width != theApp.fsWidth ||
		    height != theApp.fsHeight ||
		    colorDepth != theApp.fsColorDepth ||
		    pGUID != theApp.pVideoDriverGUID ||
		    theApp.videoOption != VIDEO_OTHER)
		{
			theApp.fsForceChange    = true;
			theApp.fsWidth          = width;
			theApp.fsHeight         = height;
			theApp.fsColorDepth     = colorDepth;
			theApp.pVideoDriverGUID = pGUID;
			if (pGUID)
			{
				theApp.videoDriverGUID = *pGUID;
				regSetDwordValue(_T("defaultVideoDriver"), FALSE);
				regSetBinaryValue(_T("videoDriverGUID"),
				                  (char *)pGUID, sizeof(GUID));
			}
			else
			{
				regSetDwordValue(_T("defaultVideoDriver"), TRUE);
			}
			theApp.updateVideoSize(ID_OPTIONS_VIDEO_FULLSCREEN);
			theApp.m_pMainWnd->PostMessage(VBA_CONFIRM_MODE);
		}
	}
}

void MainWnd::OnUpdateOptionsVideoFullscreen(CCmdUI*pCmdUI)
{
	setRadioCheck(pCmdUI, theApp.videoOption == VIDEO_OTHER);
}

void MainWnd::OnOptionsVideoDisablesfx()
{
	cpuDisableSfx = !cpuDisableSfx;
	if (emulating && systemCartridgeType == IMAGE_GBA)
		CPUUpdateRender();
}

void MainWnd::OnUpdateOptionsVideoDisablesfx(CCmdUI*pCmdUI)
{
	pCmdUI->SetCheck(cpuDisableSfx);
}

void MainWnd::OnOptionsVideoScreenScaling(UINT nID)
{
	theApp.screenScaling = nID - ID_OPTIONS_VIDEO_SCALING_STRETCH;
	regSetDwordValue(_T("screenScaling"), theApp.screenScaling);

	// Only the picture area changes, so the window is left the size it is:
	// updateWindowSize() would drop a maximized window out of its state and
	// undo a size the user dragged out by hand.
	layoutNow();
	systemRefreshScreen();
}

void MainWnd::OnUpdateOptionsVideoScreenScaling(CCmdUI *pCmdUI)
{
	// A radio dot on the mode in use, drawn by Windows.  MFC's SetRadio()
	// cannot be used for it: that installs a check-mark bitmap of its own,
	// which ignores the menu theme.  See setRadioCheck().
	setRadioCheck(pCmdUI, theApp.screenScaling ==
	                      (int)(pCmdUI->m_nID - ID_OPTIONS_VIDEO_SCALING_STRETCH));
}

BOOL MainWnd::OnVideoLayer(UINT nID)
{
	layerSettings ^= 0x0100 << ((nID & 0xFFFF) - ID_OPTIONS_VIDEO_LAYERS_BG0);
	extern int32 layerEnable;
	layerEnable = DISPCNT & layerSettings;
	CPUUpdateRenderBuffers(false);
	return TRUE;
}

void MainWnd::OnUpdateVideoLayer(CCmdUI *pCmdUI)
{
	pCmdUI->SetCheck((layerSettings >> (8 + pCmdUI->m_nID - ID_OPTIONS_VIDEO_LAYERS_BG0)) & 1);
	switch (pCmdUI->m_nID)
	{
	case ID_OPTIONS_VIDEO_LAYERS_BG1:
	case ID_OPTIONS_VIDEO_LAYERS_BG2:
	case ID_OPTIONS_VIDEO_LAYERS_BG3:
	case ID_OPTIONS_VIDEO_LAYERS_WIN1:
	case ID_OPTIONS_VIDEO_LAYERS_OBJWIN:
		pCmdUI->Enable(systemCartridgeType == IMAGE_GBA);
		break;
	}
}

#if VBA_ENABLE_GDI
void MainWnd::OnOptionsVideoRendermethodGdi()
{
	theApp.renderMethod = GDI;
	theApp.updateRenderMethod(false);
}

void MainWnd::OnUpdateOptionsVideoRendermethodGdi(CCmdUI*pCmdUI)
{
	setRadioCheck(pCmdUI, theApp.renderMethod == GDI);
}
#endif // VBA_ENABLE_GDI

#if VBA_ENABLE_DDRAW
void MainWnd::OnOptionsVideoRendermethodDirectdraw()
{
	theApp.renderMethod = DIRECT_DRAW;
	theApp.updateRenderMethod(false);
}

void MainWnd::OnUpdateOptionsVideoRendermethodDirectdraw(CCmdUI*pCmdUI)
{
	setRadioCheck(pCmdUI, theApp.renderMethod == DIRECT_DRAW);
}
#endif // VBA_ENABLE_DDRAW

void MainWnd::OnOptionsVideoRendermethodDirect3d()
{
	theApp.renderMethod = DIRECT_3D;
	theApp.updateRenderMethod(false);
}

void MainWnd::OnUpdateOptionsVideoRendermethodDirect3d(CCmdUI*pCmdUI)
{
	// The backend runs on a 9Ex device where the system has one and on a plain
	// 9.0c device where Direct3DCreate9Ex is missing, see Direct3D.cpp.
	bool active = theApp.renderMethod == DIRECT_3D;

	setRenderMethodModel(pCmdUI,
	                     active ? direct3D9PresentationModel() : NULL,
	                     active ? direct3D9DeviceName() : NULL);
	setRadioCheck(pCmdUI, active);
}

#if VBA_ENABLE_OPENGL
void MainWnd::OnOptionsVideoRendermethodOpengl()
{
	theApp.renderMethod = OPENGL;
	theApp.updateRenderMethod(false);
}

void MainWnd::OnUpdateOptionsVideoRendermethodOpengl(CCmdUI*pCmdUI)
{
	setRadioCheck(pCmdUI, theApp.renderMethod == OPENGL);
}

void MainWnd::OnOptionsVideoRendermethodOpengl33()
{
	theApp.renderMethod = OPENGL33;
	theApp.updateRenderMethod(false);
}

void MainWnd::OnUpdateOptionsVideoRendermethodOpengl33(CCmdUI*pCmdUI)
{
	// The renderer falls back through the rest of the chain where the driver
	// has no 3.3 core context to give, so the entry is always offered; the
	// core version it ended up creating is named the way the Direct3D entries
	// are named after their presentation model.
	bool active = theApp.renderMethod == OPENGL33;

	setRenderMethodGlVersion(pCmdUI, active ? openGL33Version() : NULL);

	setRadioCheck(pCmdUI, active);
}
#endif // VBA_ENABLE_OPENGL

void MainWnd::OnOptionsVideoRendermethodDirect3d11()
{
	theApp.renderMethod = DIRECT_3D11;
	theApp.updateRenderMethod(false);
}

void MainWnd::OnUpdateOptionsVideoRendermethodDirect3d11(CCmdUI*pCmdUI)
{
	// D3D11.DLL and D3DCOMPILER_47.DLL are not part of Windows 7, so the
	// renderer is only offered where both of them can be loaded.
	pCmdUI->Enable(direct3D11Available());

	// The renderer presents in the flip model where the system supports it and
	// in the bitblt model where it does not, see createSwapChain().
	setRenderMethodModel(pCmdUI, theApp.renderMethod == DIRECT_3D11 ? direct3D11PresentationModel() : NULL);

	setRadioCheck(pCmdUI, theApp.renderMethod == DIRECT_3D11);
}

#if VBA_ENABLE_WGPU
void MainWnd::OnOptionsVideoRendermethodWebgpu()
{
	theApp.renderMethod = WGPU;
	theApp.updateRenderMethod(false);
}

void MainWnd::OnUpdateOptionsVideoRendermethodWebgpu(CCmdUI*pCmdUI)
{
	// wgpu_native.dll is not part of any Windows, so the renderer is only
	// offered where the library can be loaded, like the Direct3D 11 entry is.
	pCmdUI->Enable(wgpuAvailable());

	// The renderer runs on the developer's backend of choice out of the ones
	// the adapter has, see requestAdapter(); the entry is named after it.
	setRenderMethodModel(pCmdUI, theApp.renderMethod == WGPU ? wgpuBackendModel() : NULL);

	setRadioCheck(pCmdUI, theApp.renderMethod == WGPU);
}

// The backend the WebGPU renderer runs on, which is the item id less the first
// of the five, see VBAWgpuBackend in WgpuConfig.h.
//
// Choosing one is also choosing the renderer itself: an entry that only set a
// backend would do nothing while another render method was presenting, which
// is not what picking "Direct3D 12" out of a WebGPU menu can be read as.  The
// switch is forced because the render method does not change, and
// updateRenderMethod() only rebuilds the display when it does.
void MainWnd::OnOptionsVideoWgpubackend(UINT nID)
{
	int backend = (int)(nID - ID_OPTIONS_VIDEO_WGPUBACKEND_AUTO);

	// A backend this machine has no adapter for is greyed out, but a menu can
	// still be reached by an accelerator, so the choice is checked here too.
	if (!wgpuBackendAvailable(backend))
		return;

	theApp.wgpuBackend = backend;
	regSetDwordValue(_T("wgpuBackend"), backend);

	theApp.renderMethod = WGPU;
	theApp.updateRenderMethod(true);
}

void MainWnd::OnUpdateOptionsVideoWgpubackend(CCmdUI *pCmdUI)
{
	int backend = (int)(pCmdUI->m_nID - ID_OPTIONS_VIDEO_WGPUBACKEND_AUTO);

	pCmdUI->Enable(wgpuBackendAvailable(backend));

	// A check mark rather than a radio dot: the dot is drawn from a bitmap
	// that reads as a white square under the dark frame this build paints,
	// while the check mark is the one the rest of the menu already uses.
	setRadioCheck(pCmdUI, theApp.wgpuBackend == backend);
}
#endif

#if VBA_ENABLE_DDRAW
void MainWnd::OnOptionsVideoDdrawemulationonly()
{
	theApp.ddrawEmulationOnly = !theApp.ddrawEmulationOnly;
}

void MainWnd::OnUpdateOptionsVideoDdrawemulationonly(CCmdUI*pCmdUI)
{
	pCmdUI->SetCheck(theApp.ddrawEmulationOnly);
}

void MainWnd::OnOptionsVideoDdrawusevideomemory()
{
	theApp.ddrawUseVideoMemory = !theApp.ddrawUseVideoMemory;
}

void MainWnd::OnUpdateOptionsVideoDdrawusevideomemory(CCmdUI*pCmdUI)
{
	pCmdUI->SetCheck(theApp.ddrawUseVideoMemory);
}
#endif // VBA_ENABLE_DDRAW

// The texture filter menu is one menu for all the renderers, while each of
// them keeps its own setting: the two Direct3D ones share d3dFilter, the two
// OpenGL ones share glFilter and the WebGPU one has its own.  These two read
// and write the setting of the renderer that is running, -1 when it has none.
static int currentTextureFilter()
{
	switch (theApp.renderMethod)
	{
	case DIRECT_3D:
	case DIRECT_3D11:
		return theApp.d3dFilter;
#if VBA_ENABLE_OPENGL
	case OPENGL:
	case OPENGL33:
		return theApp.glFilter;
#endif
#if VBA_ENABLE_WGPU
	case WGPU:
		return theApp.wgpuFilter;
#endif
	default:
		return -1;
	}
}

static void setTextureFilter(int filter)
{
	const char *option;
	int *value;

	switch (theApp.renderMethod)
	{
	case DIRECT_3D:
	case DIRECT_3D11:
		option = "d3dFilter";
		value  = &theApp.d3dFilter;
		break;
#if VBA_ENABLE_OPENGL
	case OPENGL:
	case OPENGL33:
		option = "glFilter";
		value  = &theApp.glFilter;
		break;
#endif
#if VBA_ENABLE_WGPU
	case WGPU:
		option = "wgpuFilter";
		value  = &theApp.wgpuFilter;
		break;
#endif
	default:
		return;
	}

	*value = filter;
	// The Direct3D 9 renderer reads the filter from the registry once a frame,
	// so the setting has to be there as well as in memory.
	regSetDwordValue(winWideFromAnsi(option), filter);
	if (theApp.display)
		theApp.display->setOption(option, filter);
}

void MainWnd::OnOptionsVideoTexturefilter(UINT nID)
{
	int filter = (int)(nID - ID_OPTIONS_VIDEO_TEXTUREFILTER_NEAREST);

	// A filter the renderer that is running cannot do stays unselectable, the
	// same way the upscaling menu treats its items.
	if (theApp.display == NULL || !theApp.display->filterAvailable(filter))
		return;

	setTextureFilter(filter);
}

void MainWnd::OnUpdateOptionsVideoTexturefilter(CCmdUI*pCmdUI)
{
	int filter = (int)(pCmdUI->m_nID - ID_OPTIONS_VIDEO_TEXTUREFILTER_NEAREST);

	pCmdUI->Enable(theApp.display != NULL && theApp.display->filterAvailable(filter));
	setRadioCheck(pCmdUI, currentTextureFilter() == filter);
}

#if VBA_ENABLE_OPENGL
void MainWnd::OnOptionsVideoRenderoptionsGltriangle()
{
	theApp.glType = 0;
	if (theApp.display)
		theApp.display->setOption("glType", 0);
}

void MainWnd::OnUpdateOptionsVideoRenderoptionsGltriangle(CCmdUI*pCmdUI)
{
	setRadioCheck(pCmdUI, theApp.glType == 0);
}

void MainWnd::OnOptionsVideoRenderoptionsGlquads()
{
	theApp.glType = 1;
	if (theApp.display)
		theApp.display->setOption("glType", 1);
}

void MainWnd::OnUpdateOptionsVideoRenderoptionsGlquads(CCmdUI*pCmdUI)
{
	setRadioCheck(pCmdUI, theApp.glType == 1);
}
#endif // VBA_ENABLE_OPENGL

void MainWnd::OnOptionsVideoRenderoptionsSelectskin()
{}

void MainWnd::OnUpdateOptionsVideoRenderoptionsSelectskin(CCmdUI*pCmdUI)
{}

void MainWnd::OnOptionsVideoRenderoptionsSkin()
{}

void MainWnd::OnUpdateOptionsVideoRenderoptionsSkin(CCmdUI*pCmdUI)
{}

void MainWnd::OnOptionsEmulatorAssociate()
{
	Associate dlg;
	dlg.DoModal();
}

void MainWnd::OnOptionsEmulatorDirectories()
{
	Directories dlg;
	dlg.DoModal();
}

void MainWnd::OnOptionsEmulatorFilenamePreference(UINT nID)
{
	theApp.filenamePreference = nID - ID_OPTIONS_PREFER_ARCHIVE_NAME;
}

void MainWnd::OnUpdateOptionsEmulatorFilenamePreference(CCmdUI *pCmdUI)
{
	// The two choices stay mutually exclusive, but they are drawn with ordinary
	// check marks: a radio bullet in a dark themed menu renders as a white block
	// with a corner dot, which reads as a broken icon rather than a selection.
	setRadioCheck(pCmdUI, pCmdUI->m_nID == theApp.filenamePreference + ID_OPTIONS_PREFER_ARCHIVE_NAME);
}

void MainWnd::OnOptionsVideoDisablestatusmessages()
{
	theApp.disableStatusMessage = !theApp.disableStatusMessage;
}

void MainWnd::OnUpdateOptionsVideoDisablestatusmessages(CCmdUI*pCmdUI)
{
	pCmdUI->SetCheck(theApp.disableStatusMessage);
}

void MainWnd::OnOptionsEmulatorSynchronize()
{
	synchronize = !synchronize;
}

void MainWnd::OnUpdateOptionsEmulatorSynchronize(CCmdUI*pCmdUI)
{
	pCmdUI->SetCheck(synchronize);
}

void MainWnd::OnOptionsEmulatorAlwaysOnTop()
{
	theApp.alwaysOnTop = !theApp.alwaysOnTop;
	SetWindowPos((theApp.alwaysOnTop ? &wndTopMost : &wndNoTopMost), 0, 0, 0, 0, SWP_NOSIZE | SWP_NOMOVE);
}

void MainWnd::OnUpdateOptionsEmulatorAlwaysOnTop(CCmdUI*pCmdUI)
{
	pCmdUI->SetCheck(theApp.alwaysOnTop);
}

void MainWnd::OnOptionsEmulatorModernFont()
{
	theApp.useModernFont = !theApp.useModernFont;
	uiFont::setModern(theApp.useModernFont);
}

void MainWnd::OnUpdateOptionsEmulatorModernFont(CCmdUI*pCmdUI)
{
	pCmdUI->SetCheck(theApp.useModernFont);
}

void MainWnd::OnOptionsEmulatorPausewheninactive()
{
	theApp.pauseWhenInactive = !theApp.pauseWhenInactive;
}

void MainWnd::OnUpdateOptionsEmulatorPausewheninactive(CCmdUI*pCmdUI)
{
	pCmdUI->SetCheck(theApp.pauseWhenInactive);
}

void MainWnd::OnOptionsEmulatorEnableBackgroundInput()
{
	theApp.enableBackgroundInput = !theApp.enableBackgroundInput;
}

void MainWnd::OnUpdateOptionsEmulatorEnableBackgroundInput(CCmdUI*pCmdUI)
{
	pCmdUI->SetCheck(theApp.enableBackgroundInput);
}

BOOL MainWnd::OnOptionsPriority(UINT nID)
{
	switch (nID)
	{
	case ID_OPTIONS_PRIORITY_HIGHEST:
		theApp.threadPriority = 0;
		break;
	case ID_OPTIONS_PRIORITY_ABOVENORMAL:
		theApp.threadPriority = 1;
		break;
	case ID_OPTIONS_PRIORITY_NORMAL:
		theApp.threadPriority = 2;
		break;
	case ID_OPTIONS_PRIORITY_BELOWNORMAL:
		theApp.threadPriority = 3;
		break;
	default:
		return FALSE;
	}
	theApp.updatePriority();

	return TRUE;
}

void MainWnd::OnUpdateOptionsPriority(CCmdUI *pCmdUI)
{
	switch (pCmdUI->m_nID)
	{
	case ID_OPTIONS_PRIORITY_HIGHEST:
		setRadioCheck(pCmdUI, theApp.threadPriority == 0);
		break;
	case ID_OPTIONS_PRIORITY_ABOVENORMAL:
		setRadioCheck(pCmdUI, theApp.threadPriority == 1);
		break;
	case ID_OPTIONS_PRIORITY_NORMAL:
		setRadioCheck(pCmdUI, theApp.threadPriority == 2);
		break;
	case ID_OPTIONS_PRIORITY_BELOWNORMAL:
		setRadioCheck(pCmdUI, theApp.threadPriority == 3);
		break;
	}
}

void MainWnd::OnOptionsEmulatorSpeeduptoggle()
{
	theApp.speedupToggle = !theApp.speedupToggle;
}

void MainWnd::OnUpdateOptionsEmulatorSpeeduptoggle(CCmdUI*pCmdUI)
{
	pCmdUI->SetCheck(theApp.speedupToggle);
}

void MainWnd::OnOptionsEmulatorRemoveintrosgba()
{
	//  theApp.removeIntros = !theApp.removeIntros;
}

void MainWnd::OnUpdateOptionsEmulatorRemoveintrosgba(CCmdUI*pCmdUI)
{
	pCmdUI->Enable(false);
	//  pCmdUI->SetCheck(theApp.removeIntros);
}

void MainWnd::OnOptionsEmulatorAutomaticallyipspatch()
{
	theApp.autoIPS = !theApp.autoIPS;
}

void MainWnd::OnUpdateOptionsEmulatorAutomaticallyipspatch(CCmdUI*pCmdUI)
{
	pCmdUI->SetCheck(theApp.autoIPS);
}

void MainWnd::OnOptionsEmulatorAgbprint()
{
	agbPrintEnable(!agbPrintIsEnabled());
}

void MainWnd::OnUpdateOptionsEmulatorAgbprint(CCmdUI*pCmdUI)
{
	pCmdUI->SetCheck(agbPrintIsEnabled());
}

void MainWnd::OnOptionsEmulatorRealtimeclock()
{
	theApp.winRtcEnable = !theApp.winRtcEnable;
}

void MainWnd::OnUpdateOptionsEmulatorRealtimeclock(CCmdUI*pCmdUI)
{
	pCmdUI->SetCheck(theApp.winRtcEnable);
	pCmdUI->Enable(!VBAMovieIsActive() || GetAsyncKeyState(VK_CONTROL));
}

void MainWnd::OnOptionsEmulatorAutohidemenu()
{
	theApp.autoHideMenu = !theApp.autoHideMenu;
}

void MainWnd::OnUpdateOptionsEmulatorAutohidemenu(CCmdUI*pCmdUI)
{
	pCmdUI->SetCheck(theApp.autoHideMenu);
}

void MainWnd::OnOptionsEmulatorRewindinterval()
{
	RewindInterval dlg((float)theApp.rewindTimer/6.0f, theApp.rewindSlots);
	int v = (int)dlg.DoModal();

	if (v >= 0)
	{
		int interval = v & 0x0000ffff;
		int slots    = (v & 0xffff0000) >> 16;

		int prevSlots = theApp.rewindSlots;

		theApp.rewindTimer = interval; // already converted to a multiple of 10 frames
		theApp.rewindSlots = slots;
		if (interval == 0 || slots == 0)
		{
			theApp.rewindTimer = theApp.rewindSlots = 0;
			regSetDwordValue(_T("rewindTimer"), interval);
			regSetDwordValue(_T("rewindSlots"), slots);
			if (theApp.rewindMemory)
				free(theApp.rewindMemory);
			theApp.rewindMemory     = NULL;
			theApp.rewindCount      = 0;
			theApp.rewindCounter    = 0;
			theApp.rewindSaveNeeded = false;
		}
		else
		{
			regSetDwordValue(_T("rewindTimer"), interval);
			regSetDwordValue(_T("rewindSlots"), slots);
			if (slots != prevSlots)
			{
				if (theApp.rewindMemory)
					free(theApp.rewindMemory);
				theApp.rewindMemory = NULL;
				theApp.rewindPos    = 0;
			}
			if (theApp.rewindMemory == NULL)
				theApp.rewindMemory = (char *)malloc(theApp.rewindSlots*REWIND_SIZE);
			theApp.rewindCount      = 0;
			theApp.rewindSaveNeeded = true;
		}
	}
}

BOOL MainWnd::OnOptionsEmulatorShowSpeed(UINT nID)
{
	switch (nID)
	{
	case ID_OPTIONS_EMULATOR_SHOWSPEED_NONE:
		theApp.showSpeed = 0;
		systemSetTitle(_T(VBA_NAME_AND_VERSION));
		break;
	case ID_OPTIONS_EMULATOR_SHOWSPEED_PERCENTAGE:
		theApp.showSpeed = 1;
		break;
	case ID_OPTIONS_EMULATOR_SHOWSPEED_DETAILED:
		theApp.showSpeed = 2;
		break;
	case ID_OPTIONS_EMULATOR_SHOWSPEED_TRANSPARENT:
		theApp.showSpeedTransparent = !theApp.showSpeedTransparent;
		break;
	default:
		return FALSE;
	}
	return TRUE;
}

void MainWnd::OnUpdateOptionsEmulatorShowSpeed(CCmdUI *pCmdUI)
{
	switch (pCmdUI->m_nID)
	{
	case ID_OPTIONS_EMULATOR_SHOWSPEED_NONE:
		setRadioCheck(pCmdUI, theApp.showSpeed == 0);
		break;
	case ID_OPTIONS_EMULATOR_SHOWSPEED_PERCENTAGE:
		setRadioCheck(pCmdUI, theApp.showSpeed == 1);
		break;
	case ID_OPTIONS_EMULATOR_SHOWSPEED_DETAILED:
		setRadioCheck(pCmdUI, theApp.showSpeed == 2);
		break;
	case ID_OPTIONS_EMULATOR_SHOWSPEED_TRANSPARENT:
		pCmdUI->SetCheck(theApp.showSpeedTransparent);
		break;
	}
}

void MainWnd::OnOptionsEmulatorSavetypeAutomatic()
{
	theApp.winSaveType = 0;
}

void MainWnd::OnUpdateOptionsEmulatorSavetypeAutomatic(CCmdUI*pCmdUI)
{
	setRadioCheck(pCmdUI, theApp.winSaveType == 0);
	pCmdUI->Enable(!VBAMovieIsActive() || GetAsyncKeyState(VK_CONTROL));
}

void MainWnd::OnOptionsEmulatorSavetypeEeprom()
{
	theApp.winSaveType = 1;
}

void MainWnd::OnUpdateOptionsEmulatorSavetypeEeprom(CCmdUI*pCmdUI)
{
	setRadioCheck(pCmdUI, theApp.winSaveType == 1);
	pCmdUI->Enable(!VBAMovieIsActive() || GetAsyncKeyState(VK_CONTROL));
}

void MainWnd::OnOptionsEmulatorSavetypeSram()
{
	theApp.winSaveType = 2;
}

void MainWnd::OnUpdateOptionsEmulatorSavetypeSram(CCmdUI*pCmdUI)
{
	setRadioCheck(pCmdUI, theApp.winSaveType == 2);
	pCmdUI->Enable(!VBAMovieIsActive() || GetAsyncKeyState(VK_CONTROL));
}

void MainWnd::OnOptionsEmulatorSavetypeFlash()
{
	theApp.winSaveType = 3;
}

void MainWnd::OnUpdateOptionsEmulatorSavetypeFlash(CCmdUI*pCmdUI)
{
	setRadioCheck(pCmdUI, theApp.winSaveType == 3);
	pCmdUI->Enable(!VBAMovieIsActive() || GetAsyncKeyState(VK_CONTROL));
}

void MainWnd::OnOptionsEmulatorSavetypeEepromsensor()
{
	theApp.winSaveType = 4;
}

void MainWnd::OnUpdateOptionsEmulatorSavetypeEepromsensor(CCmdUI*pCmdUI)
{
	setRadioCheck(pCmdUI, theApp.winSaveType == 4);
	pCmdUI->Enable(!VBAMovieIsActive() || GetAsyncKeyState(VK_CONTROL));
}

void MainWnd::OnOptionsEmulatorSavetypeNone()
{
	theApp.winSaveType = 5;
}

void MainWnd::OnUpdateOptionsEmulatorSavetypeNone(CCmdUI*pCmdUI)
{
	setRadioCheck(pCmdUI, theApp.winSaveType == 5);
	pCmdUI->Enable(!VBAMovieIsActive() || GetAsyncKeyState(VK_CONTROL));
}

void MainWnd::OnOptionsEmulatorSavetypeFlash512k()
{
	flashSetSize(0x10000);
	theApp.winFlashSize = 0x10000;
}

void MainWnd::OnUpdateOptionsEmulatorSavetypeFlash512k(CCmdUI*pCmdUI)
{
	setRadioCheck(pCmdUI, theApp.winFlashSize == 0x10000);
	pCmdUI->Enable(!VBAMovieIsActive() || GetAsyncKeyState(VK_CONTROL));
}

void MainWnd::OnOptionsEmulatorSavetypeFlash1m()
{
	flashSetSize(0x20000);
	theApp.winFlashSize = 0x20000;
}

void MainWnd::OnUpdateOptionsEmulatorSavetypeFlash1m(CCmdUI*pCmdUI)
{
	setRadioCheck(pCmdUI, theApp.winFlashSize == 0x20000);
	pCmdUI->Enable(!VBAMovieIsActive() || GetAsyncKeyState(VK_CONTROL));
}

void MainWnd::OnOptionsEmulatorUsebiosfile()
{
	if (!theApp.biosFileName.IsEmpty())
		theApp.useBiosFile = !theApp.useBiosFile;
}

void MainWnd::OnUpdateOptionsEmulatorUsebiosfile(CCmdUI*pCmdUI)
{
	pCmdUI->SetCheck(theApp.useBiosFile);
	pCmdUI->Enable(!theApp.biosFileName.IsEmpty() && (!VBAMovieIsActive() || GetAsyncKeyState(VK_CONTROL)));
}

void MainWnd::OnOptionsEmulatorSkipbios()
{
	theApp.skipBiosIntro = !theApp.skipBiosIntro;
}

void MainWnd::OnUpdateOptionsEmulatorSkipbios(CCmdUI*pCmdUI)
{
	pCmdUI->SetCheck(theApp.skipBiosIntro);
	pCmdUI->Enable(!VBAMovieIsActive() || GetAsyncKeyState(VK_CONTROL));
}

#ifdef USE_GBA_CORE_V7
void MainWnd::OnOptionsEmulatorGBALag()
{
	extern void TogglePrefetchHack();
	TogglePrefetchHack();
	memLagEnabled = memLagTempEnabled; // memLagEnabled is only to hold the last value that the user chose, so temporary changes
                                       // don't get into the registry
}

void MainWnd::OnUpdateOptionsEmulatorGBALag(CCmdUI*pCmdUI)
{
	pCmdUI->SetCheck(!memLagTempEnabled);
	pCmdUI->Enable(!VBAMovieIsActive() || GetAsyncKeyState(VK_CONTROL));
}
#endif

void MainWnd::OnOptionsEmulatorUseOldGBTiming()
{
	useOldFrameTiming = !useOldFrameTiming;
}

void MainWnd::OnUpdateOptionsEmulatorUseOldGBTiming(CCmdUI*pCmdUI)
{
	pCmdUI->SetCheck(useOldFrameTiming);
	pCmdUI->Enable(!VBAMovieIsActive() || GetAsyncKeyState(VK_CONTROL));
}

#ifdef USE_GB_CORE_V7
void MainWnd::OnOptionsEmulatorUseGBNullInputHack()
{
	if (VBAMovieIsActive())
		gbNullInputHackTempEnabled = !gbNullInputHackTempEnabled;
	else
		gbNullInputHackTempEnabled = gbNullInputHackEnabled = !gbNullInputHackEnabled;
}

void MainWnd::OnUpdateOptionsEmulatorUseGBNullInputHack(CCmdUI*pCmdUI)
{
	pCmdUI->SetCheck(gbNullInputHackTempEnabled);
	pCmdUI->Enable((!VBAMovieIsActive() && !useOldFrameTiming) || GetAsyncKeyState(VK_CONTROL));
}
#else
void MainWnd::OnOptionsEmulatorUseV20GBFrameHack()
{
	if (VBAMovieIsActive())
		gbV20GBFrameTimingHackTemp = !gbV20GBFrameTimingHackTemp;
	else
		gbV20GBFrameTimingHackTemp = gbV20GBFrameTimingHack = !gbV20GBFrameTimingHack;
}

void MainWnd::OnUpdateOptionsEmulatorUseV20GBFrameHack(CCmdUI*pCmdUI)
{
	pCmdUI->SetCheck(gbV20GBFrameTimingHackTemp);
	pCmdUI->Enable((!VBAMovieIsActive() && useOldFrameTiming) || GetAsyncKeyState(VK_CONTROL));
}
#endif

void MainWnd::OnOptionsEmulatorSelectbiosfile()
{
	LPCTSTR exts[] = { NULL };
	CString filter = winResLoadFilter(IDS_FILTER_BIOS);
	CString title  = winResLoadString(IDS_SELECT_BIOS_FILE);

	FileDlg dlg(this,
	            theApp.biosFileName,
	            filter,
	            0,
	            _T("BIOS"),
	            exts,
	            _T(""),
	            title,
	            false);

	if (dlg.DoModal() == IDOK)
	{
		theApp.biosFileName = dlg.GetPathName();
	}
}

void MainWnd::OnOptionsEmulatorCaptureFormat(UINT nID)
{
	imageCaptureSelectIndex((int)(nID - ID_OPTIONS_EMULATOR_FORMAT_FIRST));
}

void MainWnd::OnOptionsSoundDisable()
{
	if (soundOffFlag)
	{
		soundOffFlag = false;
		systemSoundCleanInit();
	}
	else
	{
		soundOffFlag = true;
		systemSoundShutdown();
	}
}

void MainWnd::OnUpdateOptionsSoundDisable(CCmdUI*pCmdUI)
{
	pCmdUI->SetCheck(soundOffFlag);
	pCmdUI->Enable(!VBAMovieIsActive() || GetAsyncKeyState(VK_CONTROL));
}

void MainWnd::OnOptionsSoundMute()
{
	if ((systemSoundGetEnabledChannels() & 0x030f) == 0)
		systemSoundEnableChannels(0x030f);
	else
		systemSoundDisableChannels(0x030f);
}

void MainWnd::OnUpdateOptionsSoundMute(CCmdUI*pCmdUI)
{
	pCmdUI->SetCheck((systemSoundGetEnabledChannels() & 0x030f) == 0);
}

void MainWnd::OnOptionsSoundOff()
{
	systemSoundDisableChannels(0x030f);
}

void MainWnd::OnUpdateOptionsSoundOff(CCmdUI*pCmdUI)
{
	pCmdUI->SetCheck((systemSoundGetEnabledChannels() & 0x030f) == 0);
}

void MainWnd::OnOptionsSoundOn()
{
	systemSoundEnableChannels(0x030f);
}

void MainWnd::OnUpdateOptionsSoundOn(CCmdUI*pCmdUI)
{
	pCmdUI->SetCheck(systemSoundGetEnabledChannels() == 0x030f);
}

void MainWnd::OnOptionsSoundUseoldsynchronization()
{
	theApp.useOldSync = !theApp.useOldSync;
	systemMessage(IDS_SETTING_WILL_BE_EFFECTIVE,
	              "Setting will be effective the next time you start the emulator");
}

void MainWnd::OnUpdateOptionsSoundUseoldsynchronization(CCmdUI*pCmdUI)
{
	pCmdUI->SetCheck(theApp.useOldSync);
}

void MainWnd::OnOptionsSoundEcho()
{
	soundEcho = !soundEcho;
}

void MainWnd::OnUpdateOptionsSoundEcho(CCmdUI*pCmdUI)
{
	pCmdUI->SetCheck(soundEcho);
}

void MainWnd::OnOptionsSoundLowpassfilter()
{
	soundLowPass = !soundLowPass;
}

void MainWnd::OnUpdateOptionsSoundLowpassfilter(CCmdUI*pCmdUI)
{
	pCmdUI->SetCheck(soundLowPass);
}

void MainWnd::OnOptionsSoundReversestereo()
{
	soundReverse = !soundReverse;
}

void MainWnd::OnUpdateOptionsSoundReversestereo(CCmdUI*pCmdUI)
{
	pCmdUI->SetCheck(soundReverse);
}

void MainWnd::OnOptionsSoundMuteFrameAdvance()
{
	theApp.muteFrameAdvance = !theApp.muteFrameAdvance;
}

void MainWnd::OnUpdateOptionsSoundMuteFrameAdvance(CCmdUI*pCmdUI)
{
	pCmdUI->SetCheck(theApp.muteFrameAdvance);
}

void MainWnd::OnOptionsSoundMuteWhenInactive()
{
	theApp.muteWhenInactive = !theApp.muteWhenInactive;
}

void MainWnd::OnUpdateOptionsSoundMuteWhenInactive(CCmdUI*pCmdUI)
{
	pCmdUI->SetCheck(theApp.muteWhenInactive);
}

void MainWnd::OnOptionsSound11khz()
{
	systemSoundSetQuality(4);
}

void MainWnd::OnUpdateOptionsSound11khz(CCmdUI*pCmdUI)
{
	setRadioCheck(pCmdUI, soundQuality == 4);
	pCmdUI->Enable((!VBAMovieIsActive() ||
	                GetAsyncKeyState(VK_CONTROL)) && !(theApp.soundRecording || theApp.aviRecording || theApp.nvAudioLog));
}

void MainWnd::OnOptionsSound22khz()
{
	systemSoundSetQuality(2);
}

void MainWnd::OnUpdateOptionsSound22khz(CCmdUI*pCmdUI)
{
	setRadioCheck(pCmdUI, soundQuality == 2);
	pCmdUI->Enable((!VBAMovieIsActive() ||
	                GetAsyncKeyState(VK_CONTROL)) && !(theApp.soundRecording || theApp.aviRecording || theApp.nvAudioLog));
}

void MainWnd::OnOptionsSound44khz()
{
	systemSoundSetQuality(1);
}

void MainWnd::OnUpdateOptionsSound44khz(CCmdUI*pCmdUI)
{
	setRadioCheck(pCmdUI, soundQuality == 1);
	pCmdUI->Enable(!(theApp.soundRecording || theApp.aviRecording || theApp.nvAudioLog));
}

// Re-creates the sound backend so a changed soundApi takes effect immediately.
// systemSoundInit() falls back to DirectSound if XAudio2 cannot start.
static void applySoundApi()
{
	if (!soundOffFlag)
		systemSoundCleanInit();
}

void MainWnd::OnOptionsSoundApiDirectsound()
{
	if (theApp.soundApi == SOUND_API_DIRECTSOUND)
		return;

	theApp.soundApi = SOUND_API_DIRECTSOUND;
	applySoundApi();
}

void MainWnd::OnUpdateOptionsSoundApiDirectsound(CCmdUI*pCmdUI)
{
	// The ring is fixed at ten blocks and nine of them are in flight; saying so
	// here is what the XAudio2 entry does with its queue, so the two can be
	// compared where the backend is chosen.
	setSoundApiBuffering(pCmdUI, directSoundBufferCount(), directSoundBufferingMs(), 0);

	setRadioCheck(pCmdUI, theApp.soundApi == SOUND_API_DIRECTSOUND);
	pCmdUI->Enable(!(theApp.soundRecording || theApp.aviRecording || theApp.nvAudioLog));
}

void MainWnd::OnOptionsSoundApiXaudio2()
{
	if (theApp.soundApi == SOUND_API_XAUDIO2)
		return;

	theApp.soundApi = SOUND_API_XAUDIO2;
	applySoundApi();
}

void MainWnd::OnUpdateOptionsSoundApiXaudio2(CCmdUI*pCmdUI)
{
	// The blocks the voice holds and the latency they add are what this backend
	// is tuned for, so they are shown where the backend is chosen.
	setSoundApiBuffering(pCmdUI, xaudio2BufferCount(), xaudio2BufferingMs(), xaudio2DropoutCount());

	setRadioCheck(pCmdUI, theApp.soundApi == SOUND_API_XAUDIO2);
	pCmdUI->Enable(!(theApp.soundRecording || theApp.aviRecording || theApp.nvAudioLog));
}

BOOL MainWnd::OnOptionsSoundVolume(UINT nID)
{
	soundVolume = nID - ID_OPTIONS_SOUND_VOLUME_1X;
	return TRUE;
}

void MainWnd::OnUpdateOptionsSoundVolume(CCmdUI *pCmdUI)
{
	setRadioCheck(pCmdUI, soundVolume == (int)(pCmdUI->m_nID - ID_OPTIONS_SOUND_VOLUME_1X));
}

void MainWnd::OnOptionsSoundVolume25x()
{
	soundVolume = 4;
}

void MainWnd::OnUpdateOptionsSoundVolume25x(CCmdUI*pCmdUI)
{
	setRadioCheck(pCmdUI, soundVolume == 4);
}

void MainWnd::OnOptionsSoundVolume5x()
{
	soundVolume = 5;
}

void MainWnd::OnUpdateOptionsSoundVolume5x(CCmdUI*pCmdUI)
{
	setRadioCheck(pCmdUI, soundVolume == 5);
}

static inline void OnSoundToggleEnabled(int c)
{
	if (systemSoundGetEnabledChannels() & c)
	{
		systemSoundDisableChannels(c);
	}
	else
	{
		systemSoundEnableChannels(c);
	}
}

void MainWnd::OnOptionsSoundChannel1()
{
	OnSoundToggleEnabled(0x01);
}

void MainWnd::OnUpdateOptionsSoundChannel1(CCmdUI*pCmdUI)
{
	pCmdUI->SetCheck(systemSoundGetEnabledChannels() & 0x01);
}

void MainWnd::OnOptionsSoundChannel2()
{
	OnSoundToggleEnabled(0x02);
}

void MainWnd::OnUpdateOptionsSoundChannel2(CCmdUI*pCmdUI)
{
	pCmdUI->SetCheck(systemSoundGetEnabledChannels() & 0x02);
}

void MainWnd::OnOptionsSoundChannel3()
{
	OnSoundToggleEnabled(0x04);
}

void MainWnd::OnUpdateOptionsSoundChannel3(CCmdUI*pCmdUI)
{
	pCmdUI->SetCheck(systemSoundGetEnabledChannels() & 0x04);
}

void MainWnd::OnOptionsSoundChannel4()
{
	OnSoundToggleEnabled(0x08);
}

void MainWnd::OnUpdateOptionsSoundChannel4(CCmdUI*pCmdUI)
{
	pCmdUI->SetCheck(systemSoundGetEnabledChannels() & 0x08);
}

void MainWnd::OnOptionsSoundDirectsounda()
{
	OnSoundToggleEnabled(0x0100);
}

void MainWnd::OnUpdateOptionsSoundDirectsounda(CCmdUI*pCmdUI)
{
	pCmdUI->SetCheck(systemSoundGetEnabledChannels() & 0x0100);
	//pCmdUI->Enable(systemCartridgeType == IMAGE_GBA);
}

void MainWnd::OnOptionsSoundDirectsoundb()
{
	OnSoundToggleEnabled(0x0200);
}

void MainWnd::OnUpdateOptionsSoundDirectsoundb(CCmdUI*pCmdUI)
{
	pCmdUI->SetCheck(systemSoundGetEnabledChannels() & 0x0200);
	//pCmdUI->Enable(systemCartridgeType == IMAGE_GBA);
}

void MainWnd::OnOptionsGameboyBorder()
{
	theApp.winGbBorderOn = !theApp.winGbBorderOn;
	gbBorderOn = theApp.winGbBorderOn;
	if (emulating && systemCartridgeType == 1 && gbBorderOn)
	{
		gbSgbRenderBorder();
	}
	theApp.updateWindowSize(theApp.videoOption);
}

void MainWnd::OnUpdateOptionsGameboyBorder(CCmdUI*pCmdUI)
{
	pCmdUI->SetCheck(theApp.winGbBorderOn);
}

void MainWnd::OnOptionsGameboyPrinter()
{
	theApp.winGbPrinterEnabled = !theApp.winGbPrinterEnabled;
	if (theApp.winGbPrinterEnabled)
		gbSerialFunction = gbPrinterSend;
	else
		gbSerialFunction = NULL;
}

void MainWnd::OnUpdateOptionsGameboyPrinter(CCmdUI*pCmdUI)
{
	pCmdUI->SetCheck(gbSerialFunction == gbPrinterSend);
}

void MainWnd::OnOptionsGameboyBorderAutomatic()
{
	gbBorderAutomatic = !gbBorderAutomatic;
	if (emulating && systemCartridgeType == 1 && gbBorderOn)
	{
		gbSgbRenderBorder();
		theApp.updateWindowSize(theApp.videoOption);
	}
}

void MainWnd::OnUpdateOptionsGameboyBorderAutomatic(CCmdUI*pCmdUI)
{
	pCmdUI->SetCheck(gbBorderAutomatic);
}

void MainWnd::OnOptionsGameboyAutomatic()
{
	gbEmulatorType = 0;
}

void MainWnd::OnUpdateOptionsGameboyAutomatic(CCmdUI*pCmdUI)
{
	setRadioCheck(pCmdUI, gbEmulatorType == 0);
	pCmdUI->Enable(!VBAMovieIsActive() || GetAsyncKeyState(VK_CONTROL));
}

void MainWnd::OnOptionsGameboyGba()
{
	gbEmulatorType = 4;
}

void MainWnd::OnUpdateOptionsGameboyGba(CCmdUI*pCmdUI)
{
	setRadioCheck(pCmdUI, gbEmulatorType == 4);
	pCmdUI->Enable(!VBAMovieIsActive() || GetAsyncKeyState(VK_CONTROL));
}

void MainWnd::OnOptionsGameboyCgb()
{
	gbEmulatorType = 1;
}

void MainWnd::OnUpdateOptionsGameboyCgb(CCmdUI*pCmdUI)
{
	setRadioCheck(pCmdUI, gbEmulatorType == 1);
	pCmdUI->Enable(!VBAMovieIsActive() || GetAsyncKeyState(VK_CONTROL));
}

void MainWnd::OnOptionsGameboySgb()
{
	gbEmulatorType = 2;
}

void MainWnd::OnUpdateOptionsGameboySgb(CCmdUI*pCmdUI)
{
	setRadioCheck(pCmdUI, gbEmulatorType == 2);
	pCmdUI->Enable(!VBAMovieIsActive() || GetAsyncKeyState(VK_CONTROL));
}

void MainWnd::OnOptionsGameboySgb2()
{
	gbEmulatorType = 5;
}

void MainWnd::OnUpdateOptionsGameboySgb2(CCmdUI*pCmdUI)
{
	setRadioCheck(pCmdUI, gbEmulatorType == 5);
	pCmdUI->Enable(!VBAMovieIsActive() || GetAsyncKeyState(VK_CONTROL));
}

void MainWnd::OnOptionsGameboyGb()
{
	gbEmulatorType = 3;
}

void MainWnd::OnUpdateOptionsGameboyGb(CCmdUI*pCmdUI)
{
	setRadioCheck(pCmdUI, gbEmulatorType == 3);
	pCmdUI->Enable(!VBAMovieIsActive() || GetAsyncKeyState(VK_CONTROL));
}

void MainWnd::OnOptionsGameboyRealcolors()
{
	gbColorOption = 0;
}

void MainWnd::OnUpdateOptionsGameboyRealcolors(CCmdUI*pCmdUI)
{
	setRadioCheck(pCmdUI, gbColorOption == 0);
}

void MainWnd::OnOptionsGameboyGameboycolors()
{
	gbColorOption = 1;
}

void MainWnd::OnUpdateOptionsGameboyGameboycolors(CCmdUI*pCmdUI)
{
	setRadioCheck(pCmdUI, gbColorOption == 1);
}

void MainWnd::OnOptionsGameboyColors()
{
	GBColorDlg dlg;
	if (dlg.DoModal())
	{
		gbPaletteOption = dlg.getWhich();
		memcpy(systemGbPalette, dlg.getColors(), 24*sizeof(u16));
		if (emulating && systemCartridgeType == 1)
		{
			memcpy(gbPalette, &systemGbPalette[dlg.getWhich()*8], 8*sizeof(u16));
		}
	}
}

BOOL MainWnd::OnOptionsFilter(UINT nID)
{
	switch (nID)
	{
	case ID_OPTIONS_FILTER_NORMAL:
		theApp.filterType = 0;
		break;
	case ID_OPTIONS_FILTER_TVMODE:
		theApp.filterType = 1;
		break;
	case ID_OPTIONS_FILTER_2XSAI:
		theApp.filterType = 2;
		break;
	case ID_OPTIONS_FILTER_SUPER2XSAI:
		theApp.filterType = 3;
		break;
	case ID_OPTIONS_FILTER_SUPEREAGLE:
		theApp.filterType = 4;
		break;
	case ID_OPTIONS_FILTER16BIT_PIXELATEEXPERIMENTAL:
		theApp.filterType = 5;
		break;
	case ID_OPTIONS_FILTER16BIT_MOTIONBLUREXPERIMENTAL:
		theApp.filterType = 6;
		break;
	case ID_OPTIONS_FILTER16BIT_ADVANCEMAMESCALE2X:
		theApp.filterType = 7;
		break;
	case ID_OPTIONS_FILTER16BIT_SIMPLE2X:
		theApp.filterType = 8;
		break;
	case ID_OPTIONS_FILTER_BILINEAR:
		theApp.filterType = 9;
		break;
	case ID_OPTIONS_FILTER_BILINEARPLUS:
		theApp.filterType = 10;
		break;
	case ID_OPTIONS_FILTER_SCANLINES:
		theApp.filterType = 11;
		break;
	case ID_OPTIONS_FILTER_HQ2X2:
		theApp.filterType = 12;
		break;
	case ID_OPTIONS_FILTER_HQ2X:
		theApp.filterType = 13;
		break;
	case ID_OPTIONS_FILTER_LQ2X:
		theApp.filterType = 14;
		break;
	case ID_OPTIONS_FILTER_HQ3X2:
		theApp.filterType = 15;
		break;
	case ID_OPTIONS_FILTER_HQ3X:
		theApp.filterType = 16;
		break;
	case ID_OPTIONS_FILTER16BIT_SIMPLE3X:
		theApp.filterType = 17;
		break;
	case ID_OPTIONS_FILTER16BIT_SIMPLE4X:
		theApp.filterType = 18;
		break;
	case ID_OPTIONS_FILTER16BIT_PIXELATEEXPERIMENTAL3X:
		theApp.filterType = 19;
		break;
	case ID_OPTIONS_FILTER16BIT_PIXELATEEXPERIMENTAL4X:
		theApp.filterType = 20;
		break;
	case ID_OPTIONS_FILTER_XBRZ2X:
		theApp.filterType = 21;
		break;
	case ID_OPTIONS_FILTER_XBRZ3X:
		theApp.filterType = 22;
		break;
	case ID_OPTIONS_FILTER_XBRZ4X:
		theApp.filterType = 23;
		break;
	case ID_OPTIONS_FILTER_XBRZ5X:
		theApp.filterType = 24;
		break;
	case ID_OPTIONS_FILTER_HQ4X:
		theApp.filterType = 25;
		break;
	case ID_OPTIONS_FILTER_SCALEFX3X:
		theApp.filterType = 26;
		break;
	default:
		return FALSE;
	}
	theApp.updateFilter();
	return TRUE;
}

void MainWnd::OnUpdateOptionsFilter(CCmdUI *pCmdUI)
{
	pCmdUI->Enable(systemColorDepth == 16 || systemColorDepth == 32);
	switch (pCmdUI->m_nID)
	{
	case ID_OPTIONS_FILTER_NORMAL:
		setRadioCheck(pCmdUI, theApp.filterType == 0);
		break;
	case ID_OPTIONS_FILTER_TVMODE:
		setRadioCheck(pCmdUI, theApp.filterType == 1);
		break;
	case ID_OPTIONS_FILTER_2XSAI:
		setRadioCheck(pCmdUI, theApp.filterType == 2);
		break;
	case ID_OPTIONS_FILTER_SUPER2XSAI:
		setRadioCheck(pCmdUI, theApp.filterType == 3);
		break;
	case ID_OPTIONS_FILTER_SUPEREAGLE:
		setRadioCheck(pCmdUI, theApp.filterType == 4);
		break;
	case ID_OPTIONS_FILTER16BIT_PIXELATEEXPERIMENTAL:
		setRadioCheck(pCmdUI, theApp.filterType == 5);
		break;
	case ID_OPTIONS_FILTER16BIT_MOTIONBLUREXPERIMENTAL:
		setRadioCheck(pCmdUI, theApp.filterType == 6);
		break;
	case ID_OPTIONS_FILTER16BIT_ADVANCEMAMESCALE2X:
		setRadioCheck(pCmdUI, theApp.filterType == 7);
		break;
	case ID_OPTIONS_FILTER16BIT_SIMPLE2X:
		setRadioCheck(pCmdUI, theApp.filterType == 8);
		break;
	case ID_OPTIONS_FILTER_BILINEAR:
		setRadioCheck(pCmdUI, theApp.filterType == 9);
		break;
	case ID_OPTIONS_FILTER_BILINEARPLUS:
		setRadioCheck(pCmdUI, theApp.filterType == 10);
		break;
	case ID_OPTIONS_FILTER_SCANLINES:
		setRadioCheck(pCmdUI, theApp.filterType == 11);
		break;
	case ID_OPTIONS_FILTER_HQ2X2:
		setRadioCheck(pCmdUI, theApp.filterType == 12);
		break;
	case ID_OPTIONS_FILTER_HQ2X:
		setRadioCheck(pCmdUI, theApp.filterType == 13);
		break;
	case ID_OPTIONS_FILTER_LQ2X:
		setRadioCheck(pCmdUI, theApp.filterType == 14);
		break;
	case ID_OPTIONS_FILTER_HQ3X2:
		setRadioCheck(pCmdUI, theApp.filterType == 15);
		break;
	case ID_OPTIONS_FILTER_HQ3X:
		setRadioCheck(pCmdUI, theApp.filterType == 16);
		break;
	case ID_OPTIONS_FILTER16BIT_SIMPLE3X:
		setRadioCheck(pCmdUI, theApp.filterType == 17);
		break;
	case ID_OPTIONS_FILTER16BIT_SIMPLE4X:
		setRadioCheck(pCmdUI, theApp.filterType == 18);
		break;
	case ID_OPTIONS_FILTER16BIT_PIXELATEEXPERIMENTAL3X:
		setRadioCheck(pCmdUI, theApp.filterType == 19);
		break;
	case ID_OPTIONS_FILTER16BIT_PIXELATEEXPERIMENTAL4X:
		setRadioCheck(pCmdUI, theApp.filterType == 20);
		break;
	case ID_OPTIONS_FILTER_XBRZ2X:
		pCmdUI->Enable(systemColorDepth == 32); // xBRZ has no 16-bit variant
		setRadioCheck(pCmdUI, theApp.filterType == 21);
		break;
	case ID_OPTIONS_FILTER_XBRZ3X:
		pCmdUI->Enable(systemColorDepth == 32);
		setRadioCheck(pCmdUI, theApp.filterType == 22);
		break;
	case ID_OPTIONS_FILTER_XBRZ4X:
		pCmdUI->Enable(systemColorDepth == 32);
		setRadioCheck(pCmdUI, theApp.filterType == 23);
		break;
	case ID_OPTIONS_FILTER_XBRZ5X:
		pCmdUI->Enable(systemColorDepth == 32);
		setRadioCheck(pCmdUI, theApp.filterType == 24);
		break;
	case ID_OPTIONS_FILTER_HQ4X:
		setRadioCheck(pCmdUI, theApp.filterType == 25);
		break;
	case ID_OPTIONS_FILTER_SCALEFX3X:
		pCmdUI->Enable(systemColorDepth == 32); // ScaleFX has no 16-bit variant
		setRadioCheck(pCmdUI, theApp.filterType == 26);
		break;
	}
}

BOOL MainWnd::OnOptionsFilterIFB(UINT nID)
{
	switch (nID)
	{
	case ID_OPTIONS_FILTER_INTERFRAMEBLENDING_NONE:
		theApp.ifbType = 0;
		break;
	case ID_OPTIONS_FILTER_INTERFRAMEBLENDING_MOTIONBLUR:
		theApp.ifbType = 1;
		break;
	case ID_OPTIONS_FILTER_INTERFRAMEBLENDING_SMART:
		theApp.ifbType = 2;
		break;
	default:
		return FALSE;
	}
	theApp.updateIFB();
	return TRUE;
}

void MainWnd::OnUpdateOptionsFilterIFB(CCmdUI *pCmdUI)
{
	switch (pCmdUI->m_nID)
	{
	case ID_OPTIONS_FILTER_INTERFRAMEBLENDING_NONE:
		setRadioCheck(pCmdUI, theApp.ifbType == 0);
		break;
	case ID_OPTIONS_FILTER_INTERFRAMEBLENDING_MOTIONBLUR:
		setRadioCheck(pCmdUI, theApp.ifbType == 1);
		break;
	case ID_OPTIONS_FILTER_INTERFRAMEBLENDING_SMART:
		setRadioCheck(pCmdUI, theApp.ifbType == 2);
		break;
	}
}

BOOL MainWnd::OnOptionsVideoLcdFilter(UINT nID)
{
	switch (nID)
	{
	case ID_OPTIONS_VIDEO_LCDFILTER_GBA_NONE:
		theApp.lcdFilterGba = LCD_FILTER_OFF;
		break;
	case ID_OPTIONS_VIDEO_LCDFILTER_GBA_GBA:
		theApp.lcdFilterGba = kLcdGbaGba;
		break;
	case ID_OPTIONS_VIDEO_LCDFILTER_GBA_SP:
		theApp.lcdFilterGba = kLcdGbaGbaSpBacklit;
		break;
	case ID_OPTIONS_VIDEO_LCDFILTER_GBA_MICRO:
		theApp.lcdFilterGba = kLcdGbaMicro;
		break;
	case ID_OPTIONS_VIDEO_LCDFILTER_GBA_NDS:
		theApp.lcdFilterGba = kLcdGbaDs;
		break;
	case ID_OPTIONS_VIDEO_LCDFILTER_GBA_DSLITE:
		theApp.lcdFilterGba = kLcdGbaDsLite;
		break;
	case ID_OPTIONS_VIDEO_LCDFILTER_GBA_NSO:
		theApp.lcdFilterGba = kLcdGbaNso;
		break;
	case ID_OPTIONS_VIDEO_LCDFILTER_GBC_NONE:
		theApp.lcdFilterGbc = LCD_FILTER_OFF;
		break;
	case ID_OPTIONS_VIDEO_LCDFILTER_GBC_CGB:
		theApp.lcdFilterGbc = kLcdGbcCgb;
		break;
	case ID_OPTIONS_VIDEO_LCDFILTER_GBC_NSO:
		theApp.lcdFilterGbc = kLcdGbcNso;
		break;
	default:
		return FALSE;
	}

	// The correction is baked into the palette translation tables, so rebuild
	// them and repaint; it takes effect from the next emulated frame on.
	systemUpdateColorMaps();
	systemRefreshScreen();
	return TRUE;
}

void MainWnd::OnUpdateOptionsVideoLcdFilter(CCmdUI *pCmdUI)
{
	switch (pCmdUI->m_nID)
	{
	case ID_OPTIONS_VIDEO_LCDFILTER_GBA_NONE:
		setRadioCheck(pCmdUI, theApp.lcdFilterGba == LCD_FILTER_OFF);
		break;
	case ID_OPTIONS_VIDEO_LCDFILTER_GBA_GBA:
		setRadioCheck(pCmdUI, theApp.lcdFilterGba == kLcdGbaGba);
		break;
	case ID_OPTIONS_VIDEO_LCDFILTER_GBA_SP:
		setRadioCheck(pCmdUI, theApp.lcdFilterGba == kLcdGbaGbaSpBacklit);
		break;
	case ID_OPTIONS_VIDEO_LCDFILTER_GBA_MICRO:
		setRadioCheck(pCmdUI, theApp.lcdFilterGba == kLcdGbaMicro);
		break;
	case ID_OPTIONS_VIDEO_LCDFILTER_GBA_NDS:
		setRadioCheck(pCmdUI, theApp.lcdFilterGba == kLcdGbaDs);
		break;
	case ID_OPTIONS_VIDEO_LCDFILTER_GBA_DSLITE:
		setRadioCheck(pCmdUI, theApp.lcdFilterGba == kLcdGbaDsLite);
		break;
	case ID_OPTIONS_VIDEO_LCDFILTER_GBA_NSO:
		setRadioCheck(pCmdUI, theApp.lcdFilterGba == kLcdGbaNso);
		break;
	case ID_OPTIONS_VIDEO_LCDFILTER_GBC_NONE:
		setRadioCheck(pCmdUI, theApp.lcdFilterGbc == LCD_FILTER_OFF);
		break;
	case ID_OPTIONS_VIDEO_LCDFILTER_GBC_CGB:
		setRadioCheck(pCmdUI, theApp.lcdFilterGbc == kLcdGbcCgb);
		break;
	case ID_OPTIONS_VIDEO_LCDFILTER_GBC_NSO:
		setRadioCheck(pCmdUI, theApp.lcdFilterGbc == kLcdGbcNso);
		break;
	}
}

// The largest ratio the upscalers are allowed to render the frame at, in the
// order of the Max Scale menu; 0 leaves it to the window.  These are not
// derived from the menu ids because the steps are not even.
static const int g_upscalerMaxScales[] = { 0, 2, 3, 4, 5, 6, 8 };

BOOL MainWnd::OnOptionsVideoUpscaler(UINT nID)
{
	int upscaler  = theApp.upscaler;
	int sharpness = theApp.upscalerSharpness;

	// How large the upscalers may make the frame before the sampler stretches
	// the rest; the renderers read it every frame, so nothing has to be told.
	if (nID >= ID_OPTIONS_VIDEO_UPSCALER_MAXSCALE_UNLIMITED)
	{
		theApp.upscalerMaxScale =
			g_upscalerMaxScales[(int)(nID - ID_OPTIONS_VIDEO_UPSCALER_MAXSCALE_UNLIMITED)];
		return TRUE;
	}

	// The upscaler items follow the UPSCALER_* values and the sharpening items
	// sit above them, so a new upscaler needs no case here (see the ids in
	// resource.h).
	if (nID < ID_OPTIONS_VIDEO_UPSCALER_SHARP_0)
	{
		upscaler = UPSCALER_OFF + (int)(nID - ID_OPTIONS_VIDEO_UPSCALER_OFF);
	}
	else
	{
		// They are 0, 25, 50, 75 and 100 percent, in that order.
		sharpness = 25 * (int)(nID - ID_OPTIONS_VIDEO_UPSCALER_SHARP_0);
	}

	// Only the render methods whose backend runs the upscaler shaders scale the
	// frame with them; every backend answers for itself through
	// upscalerAvailable(), and the renderer picks the settings up again when it
	// is recreated.
	theApp.upscaler          = upscaler;
	theApp.upscalerSharpness = sharpness;
	if (theApp.display)
		theApp.display->setOption("upscaler", upscaler);

	return TRUE;
}

void MainWnd::OnUpdateOptionsVideoUpscaler(CCmdUI *pCmdUI)
{
	// The upscalers are shaders, which only some of the render methods run, and
	// only where they built; the backend that is presenting answers both.
	if (pCmdUI->m_nID >= ID_OPTIONS_VIDEO_UPSCALER_MAXSCALE_UNLIMITED)
	{
		int scale = g_upscalerMaxScales[(int)(pCmdUI->m_nID - ID_OPTIONS_VIDEO_UPSCALER_MAXSCALE_UNLIMITED)];

		setRadioCheck(pCmdUI, theApp.upscalerMaxScale == scale);
		pCmdUI->Enable(theApp.upscaler != UPSCALER_OFF && theApp.display != NULL &&
		               theApp.display->upscalerAvailable(theApp.upscaler));
		return;
	}

	if (pCmdUI->m_nID < ID_OPTIONS_VIDEO_UPSCALER_SHARP_0)
	{
		int upscaler = UPSCALER_OFF + (int)(pCmdUI->m_nID - ID_OPTIONS_VIDEO_UPSCALER_OFF);

		setRadioCheck(pCmdUI, theApp.upscaler == upscaler);

		// The shaders that are built when the renderer starts are left out
		// where they did not compile, and the ones a backend does not have at
		// all are never there; both are what it answers with.
		pCmdUI->Enable(theApp.display != NULL && theApp.display->upscalerAvailable(upscaler));
		return;
	}

	// The sharpening amount only means something for the two SDKs that have one.
	int sharpness = 25 * (int)(pCmdUI->m_nID - ID_OPTIONS_VIDEO_UPSCALER_SHARP_0);
	bool sharpens = theApp.upscaler == UPSCALER_NIS || theApp.upscaler == UPSCALER_FSR;

	setRadioCheck(pCmdUI, theApp.upscalerSharpness == sharpness);
	pCmdUI->Enable(sharpens && theApp.display != NULL &&
	               theApp.display->upscalerAvailable(theApp.upscaler));
}

void MainWnd::OnOptionsLanguageSystem()
{
	theApp.winSetLanguageOption(0, false);
}

void MainWnd::OnUpdateOptionsLanguageSystem(CCmdUI*pCmdUI)
{
	setRadioCheck(pCmdUI, theApp.languageOption == 0);
}

void MainWnd::OnOptionsLanguageEnglish()
{
	theApp.winSetLanguageOption(1, false);
}

void MainWnd::OnUpdateOptionsLanguageEnglish(CCmdUI*pCmdUI)
{
	setRadioCheck(pCmdUI, theApp.languageOption == 1);
}

void MainWnd::OnOptionsLanguageOther()
{
	theApp.winSetLanguageOption(2, false);
}

void MainWnd::OnUpdateOptionsLanguageOther(CCmdUI*pCmdUI)
{
	setRadioCheck(pCmdUI, theApp.languageOption == 2);
}

void MainWnd::OnOptionsJoypadConfigure1()
{
	JoypadConfig dlg(0);
	dlg.DoModal();
}

void MainWnd::OnUpdateOptionsJoypadConfigure1(CCmdUI*pCmdUI)
{
	pCmdUI->Enable(theApp.videoOption != VIDEO_320x240);
}

void MainWnd::OnOptionsJoypadConfigure2()
{
	JoypadConfig dlg(1);
	dlg.DoModal();
}

void MainWnd::OnUpdateOptionsJoypadConfigure2(CCmdUI*pCmdUI)
{
	pCmdUI->Enable(theApp.videoOption != VIDEO_320x240);
}

void MainWnd::OnOptionsJoypadConfigure3()
{
	JoypadConfig dlg(2);
	dlg.DoModal();
}

void MainWnd::OnUpdateOptionsJoypadConfigure3(CCmdUI*pCmdUI)
{
	pCmdUI->Enable(theApp.videoOption != VIDEO_320x240);
}

void MainWnd::OnOptionsJoypadConfigure4()
{
	JoypadConfig dlg(3);
	dlg.DoModal();
}

void MainWnd::OnUpdateOptionsJoypadConfigure4(CCmdUI*pCmdUI)
{
	pCmdUI->Enable(theApp.videoOption != VIDEO_320x240);
}

BOOL MainWnd::OnOptionsJoypadDefault(UINT nID)
{
	theApp.joypadDefault = nID - ID_OPTIONS_JOYPAD_DEFAULTJOYPAD_1;
	return TRUE;
}

void MainWnd::OnUpdateOptionsJoypadDefault(CCmdUI *pCmdUI)
{
	setRadioCheck(pCmdUI, theApp.joypadDefault == (int)(pCmdUI->m_nID - ID_OPTIONS_JOYPAD_DEFAULTJOYPAD_1));
}

void MainWnd::OnOptionsJoypadMotionconfigure()
{
	MotionConfig dlg;
	dlg.DoModal();
}

void MainWnd::OnUpdateOptionsJoypadMotionconfigure(CCmdUI*pCmdUI)
{
	pCmdUI->Enable(theApp.videoOption != VIDEO_320x240);
}

void MainWnd::OnOptionsJoypadAllowLeftRight()
{
	theApp.allowLeftRight = !theApp.allowLeftRight;
}

void MainWnd::OnUpdateOptionsJoypadAllowLeftRight(CCmdUI*pCmdUI)
{
	pCmdUI->SetCheck(theApp.allowLeftRight);
}

void MainWnd::OnOptionsJoypadAutofireAccountForLag()
{
	theApp.autofireAccountForLag = !theApp.autofireAccountForLag;
}

void MainWnd::OnUpdateOptionsJoypadAutofireAccountForLag(CCmdUI*pCmdUI)
{
	pCmdUI->SetCheck(theApp.autofireAccountForLag);
}

// Toggle the auto-fire state of one button, exactly the way the
// "Auto-Fire <button>" menu commands have always done it: the bit is placed in
// the half of the auto-fire state that is active during the current frame and
// cleared in the other half.  systemGetOriginalJoypad() XORs the active half
// into the input and alternates the halves once per frame, which makes the
// button press every other frame.
// The configurable Auto-Fire keys in DirectInput use this as well, so a held
// key enables autofire and, if the button already fires because of Alt+?,
// disables it while it is held.
void VBAToggleAutoFireButton(u32 mask)
{
	int bit = (int)mask;

	int & autoFire  = (theApp.autoFireToggle ? theApp.autoFire : theApp.autoFire2);
	int & autoFire2 = (theApp.autoFireToggle ? theApp.autoFire2 : theApp.autoFire);

	if ((theApp.autoFire | theApp.autoFire2) & bit)
		autoFire &= ~bit;
	else
		autoFire |= bit;

	autoFire2       &= ~bit;
	theApp.autoHold &= ~bit;

	extern void VBAUpdateButtonPressDisplay(); VBAUpdateButtonPressDisplay();
}

BOOL MainWnd::OnOptionsJoypadAutofire(UINT nID)
{
	if (nID == ID_OPTIONS_JOYPAD_AUTOFIRE_CLEAR)
	{
		if (theApp.autoFire | theApp.autoFire2)
		{
			theApp.autoFire = theApp.autoFire2 = 0;
///      systemScreenMessage(winResLoadString(IDS_AUTOFIRE_DISABLED));
		}
		else
		{
///      systemScreenMessage(winResLoadString(IDS_AUTOFIRE_ALREADY_DISABLED));
			systemScreenMessage("already cleared");
		}

		extern void VBAUpdateButtonPressDisplay(); VBAUpdateButtonPressDisplay();

		return TRUE;
	}

	u32 mask;

	switch (nID)
	{
	case ID_OPTIONS_JOYPAD_AUTOFIRE_A:      mask = BUTTON_MASK_A;      break;
	case ID_OPTIONS_JOYPAD_AUTOFIRE_B:      mask = BUTTON_MASK_B;      break;
	case ID_OPTIONS_JOYPAD_AUTOFIRE_L:      mask = BUTTON_MASK_L;      break;
	case ID_OPTIONS_JOYPAD_AUTOFIRE_R:      mask = BUTTON_MASK_R;      break;
	case ID_OPTIONS_JOYPAD_AUTOFIRE_START:  mask = BUTTON_MASK_START;  break;
	case ID_OPTIONS_JOYPAD_AUTOFIRE_SELECT: mask = BUTTON_MASK_SELECT; break;
	case ID_OPTIONS_JOYPAD_AUTOFIRE_UP:     mask = BUTTON_MASK_UP;     break;
	case ID_OPTIONS_JOYPAD_AUTOFIRE_DOWN:   mask = BUTTON_MASK_DOWN;   break;
	case ID_OPTIONS_JOYPAD_AUTOFIRE_LEFT:   mask = BUTTON_MASK_LEFT;   break;
	case ID_OPTIONS_JOYPAD_AUTOFIRE_RIGHT:  mask = BUTTON_MASK_RIGHT;  break;
	default:
		return FALSE;
	}

	VBAToggleAutoFireButton(mask);

	return TRUE;
}

void MainWnd::OnUpdateOptionsJoypadAutofire(CCmdUI *pCmdUI)
{
///  pCmdUI->Enable(emulating); // FIXME: this is right, but disabling menu items screws up accelerators until you view the
// menu!
	pCmdUI->Enable(TRUE); // TEMP

	int autoFires = (theApp.autoFire | theApp.autoFire2);

	bool check = true;
	switch (pCmdUI->m_nID)
	{
	case ID_OPTIONS_JOYPAD_AUTOFIRE_A:
		check = (autoFires & BUTTON_MASK_A) != 0;
		break;
	case ID_OPTIONS_JOYPAD_AUTOFIRE_B:
		check = (autoFires & BUTTON_MASK_B) != 0;
		break;
	case ID_OPTIONS_JOYPAD_AUTOFIRE_L:
		check = (autoFires & BUTTON_MASK_L) != 0;
///	extern int gbSgbMode; // from gbSGB.cpp
///	if(emulating && systemCartridgeType != IMAGE_GBA && !gbSgbMode) // regular GB has no L button
///      pCmdUI->Enable(false); // FIXME: this is right, but disabling menu items screws up accelerators until you view the
// menu!
		break;
	case ID_OPTIONS_JOYPAD_AUTOFIRE_R:
		check = (autoFires & BUTTON_MASK_R) != 0;
///	extern int gbSgbMode; // from gbSGB.cpp
///	if(emulating && systemCartridgeType != IMAGE_GBA && !gbSgbMode) // regular GB has no R button
///      pCmdUI->Enable(false); // FIXME: this is right, but disabling menu items screws up accelerators until you view the
// menu!
		break;
	case ID_OPTIONS_JOYPAD_AUTOFIRE_START:
		check = (autoFires & BUTTON_MASK_START) != 0;
		break;
	case ID_OPTIONS_JOYPAD_AUTOFIRE_SELECT:
		check = (autoFires & BUTTON_MASK_SELECT) != 0;
		break;
	case ID_OPTIONS_JOYPAD_AUTOFIRE_UP:
		check = (autoFires & BUTTON_MASK_UP) != 0;
		break;
	case ID_OPTIONS_JOYPAD_AUTOFIRE_DOWN:
		check = (autoFires & BUTTON_MASK_DOWN) != 0;
		break;
	case ID_OPTIONS_JOYPAD_AUTOFIRE_LEFT:
		check = (autoFires & BUTTON_MASK_LEFT) != 0;
		break;
	case ID_OPTIONS_JOYPAD_AUTOFIRE_RIGHT:
		check = (autoFires & BUTTON_MASK_RIGHT) != 0;
		break;
	case ID_OPTIONS_JOYPAD_AUTOFIRE_CLEAR:
		check = (autoFires == 0);
///    pCmdUI->Enable(!check); // FIXME: this is right, but disabling menu items screws up accelerators until you view the menu!
		break;
	}
	pCmdUI->SetCheck(check);
}

BOOL MainWnd::OnOptionsJoypadSticky(UINT nID)
{
	switch (nID)
	{
	case ID_STICKY_A:
		if (theApp.autoHold & BUTTON_MASK_A)
		{
			theApp.autoHold &= ~BUTTON_MASK_A;
///      systemScreenMessage(winResLoadString(IDS_STICKY_A_DISABLED));
		}
		else
		{
			theApp.autoHold |= BUTTON_MASK_A;
///      systemScreenMessage(winResLoadString(IDS_STICKY_A));
		}
		theApp.autoFire  &= ~BUTTON_MASK_A;
		theApp.autoFire2 &= ~BUTTON_MASK_A;
		break;
	case ID_STICKY_B:
		if (theApp.autoHold & BUTTON_MASK_B)
		{
			theApp.autoHold &= ~BUTTON_MASK_B;
///      systemScreenMessage(winResLoadString(IDS_STICKY_B_DISABLED));
		}
		else
		{
			theApp.autoHold |= BUTTON_MASK_B;
///      systemScreenMessage(winResLoadString(IDS_STICKY_B));
		}
		theApp.autoFire  &= ~BUTTON_MASK_B;
		theApp.autoFire2 &= ~BUTTON_MASK_B;
		break;
	case ID_STICKY_L:
		if (theApp.autoHold & BUTTON_MASK_L)
		{
			theApp.autoHold &= ~BUTTON_MASK_L;
///      systemScreenMessage(winResLoadString(IDS_STICKY_L_DISABLED));
		}
		else
		{
			theApp.autoHold |= BUTTON_MASK_L;
///      systemScreenMessage(winResLoadString(IDS_STICKY_L));
		}
		theApp.autoFire  &= ~BUTTON_MASK_L;
		theApp.autoFire2 &= ~BUTTON_MASK_L;
		break;
	case ID_STICKY_R:
		if (theApp.autoHold & BUTTON_MASK_R)
		{
			theApp.autoHold &= ~BUTTON_MASK_R;
///      systemScreenMessage(winResLoadString(IDS_STICKY_R_DISABLED));
		}
		else
		{
			theApp.autoHold |= BUTTON_MASK_R;
///      systemScreenMessage(winResLoadString(IDS_STICKY_R));
		}
		theApp.autoFire  &= ~BUTTON_MASK_R;
		theApp.autoFire2 &= ~BUTTON_MASK_R;
		break;
	case ID_STICKY_START:
		if (theApp.autoHold & BUTTON_MASK_START)
		{
			theApp.autoHold &= ~BUTTON_MASK_START;
///      systemScreenMessage(winResLoadString(IDS_STICKY_START_DISABLED));
		}
		else
		{
			theApp.autoHold |= BUTTON_MASK_START;
///      systemScreenMessage(winResLoadString(IDS_STICKY_START));
		}
		theApp.autoFire  &= ~BUTTON_MASK_START;
		theApp.autoFire2 &= ~BUTTON_MASK_START;
		break;
	case ID_STICKY_SELECT:
		if (theApp.autoHold & BUTTON_MASK_SELECT)
		{
			theApp.autoHold &= ~BUTTON_MASK_SELECT;
///      systemScreenMessage(winResLoadString(IDS_STICKY_SELECT_DISABLED));
		}
		else
		{
			theApp.autoHold |= BUTTON_MASK_SELECT;
///      systemScreenMessage(winResLoadString(IDS_STICKY_SELECT));
		}
		theApp.autoFire  &= ~BUTTON_MASK_SELECT;
		theApp.autoFire2 &= ~BUTTON_MASK_SELECT;
		break;
	case ID_STICKY_UP:
		if (theApp.autoHold & BUTTON_MASK_UP)
		{
			theApp.autoHold &= ~BUTTON_MASK_UP;
///      systemScreenMessage(winResLoadString(IDS_STICKY_UP_DISABLED));
		}
		else
		{
			theApp.autoHold |= BUTTON_MASK_UP;
///      systemScreenMessage(winResLoadString(IDS_STICKY_UP));
		}
		theApp.autoFire  &= ~BUTTON_MASK_UP;
		theApp.autoFire2 &= ~BUTTON_MASK_UP;
		if (!theApp.allowLeftRight)
			theApp.autoHold &= ~BUTTON_MASK_DOWN;
		break;
	case ID_STICKY_DOWN:
		if (theApp.autoHold & BUTTON_MASK_DOWN)
		{
			theApp.autoHold &= ~BUTTON_MASK_DOWN;
///      systemScreenMessage(winResLoadString(IDS_STICKY_DOWN_DISABLED));
		}
		else
		{
			theApp.autoHold |= BUTTON_MASK_DOWN;
///      systemScreenMessage(winResLoadString(IDS_STICKY_DOWN));
		}
		theApp.autoFire  &= ~BUTTON_MASK_DOWN;
		theApp.autoFire2 &= ~BUTTON_MASK_DOWN;
		if (!theApp.allowLeftRight)
			theApp.autoHold &= ~BUTTON_MASK_UP;
		break;
	case ID_STICKY_LEFT:
		if (theApp.autoHold & BUTTON_MASK_LEFT)
		{
			theApp.autoHold &= ~BUTTON_MASK_LEFT;
///      systemScreenMessage(winResLoadString(IDS_STICKY_LEFT_DISABLED));
		}
		else
		{
			theApp.autoHold |= BUTTON_MASK_LEFT;
///      systemScreenMessage(winResLoadString(IDS_STICKY_LEFT));
		}
		theApp.autoFire  &= ~BUTTON_MASK_LEFT;
		theApp.autoFire2 &= ~BUTTON_MASK_LEFT;
		if (!theApp.allowLeftRight)
			theApp.autoHold &= ~BUTTON_MASK_RIGHT;
		break;
	case ID_STICKY_RIGHT:
		if (theApp.autoHold & BUTTON_MASK_RIGHT)
		{
			theApp.autoHold &= ~BUTTON_MASK_RIGHT;
///      systemScreenMessage(winResLoadString(IDS_STICKY_RIGHT_DISABLED));
		}
		else
		{
			theApp.autoHold |= BUTTON_MASK_RIGHT;
///      systemScreenMessage(winResLoadString(IDS_STICKY_RIGHT));
		}
		theApp.autoFire  &= ~BUTTON_MASK_RIGHT;
		theApp.autoFire2 &= ~BUTTON_MASK_RIGHT;
		if (!theApp.allowLeftRight)
			theApp.autoHold &= ~BUTTON_MASK_LEFT;
		break;
	case ID_STICKY_CLEAR:
		if (theApp.autoHold != 0)
		{
			theApp.autoHold = 0;
///      systemScreenMessage(winResLoadString(IDS_STICKY_DISABLED));
		}
		else
		{
///      systemScreenMessage(winResLoadString(IDS_STICKY_ALREADY_DISABLED));
			systemScreenMessage("already cleared");
		}
		break;
	default:
		return FALSE;
	}

	extern void VBAUpdateButtonPressDisplay(); VBAUpdateButtonPressDisplay();

	return TRUE;
}

void MainWnd::OnUpdateOptionsJoypadSticky(CCmdUI *pCmdUI)
{
///  pCmdUI->Enable(emulating); // FIXME: this is right, but disabling menu items screws up accelerators until you view the
// menu!
	pCmdUI->Enable(TRUE); // TEMP

	bool check = true;
	switch (pCmdUI->m_nID)
	{
	case ID_STICKY_A:
		check = (theApp.autoHold & BUTTON_MASK_A) != 0;
		break;
	case ID_STICKY_B:
		check = (theApp.autoHold & BUTTON_MASK_B) != 0;
		break;
	case ID_STICKY_L:
		check = (theApp.autoHold & BUTTON_MASK_L) != 0;
///	extern int gbSgbMode; // from gbSGB.cpp
///	if(emulating && systemCartridgeType != IMAGE_GBA && !gbSgbMode) // regular GB has no L button
///      pCmdUI->Enable(false); // FIXME: this is right, but disabling menu items screws up accelerators until you view the
// menu!
		break;
	case ID_STICKY_R:
		check = (theApp.autoHold & BUTTON_MASK_R) != 0;
///	extern int gbSgbMode; // from gbSGB.cpp
///	if(emulating && systemCartridgeType != IMAGE_GBA && !gbSgbMode) // regular GB has no R button
///      pCmdUI->Enable(false); // FIXME: this is right, but disabling menu items screws up accelerators until you view the
// menu!
		break;
	case ID_STICKY_START:
		check = (theApp.autoHold & BUTTON_MASK_START) != 0;
		break;
	case ID_STICKY_SELECT:
		check = (theApp.autoHold & BUTTON_MASK_SELECT) != 0;
		break;
	case ID_STICKY_UP:
		check = (theApp.autoHold & BUTTON_MASK_UP) != 0;
		break;
	case ID_STICKY_DOWN:
		check = (theApp.autoHold & BUTTON_MASK_DOWN) != 0;
		break;
	case ID_STICKY_LEFT:
		check = (theApp.autoHold & BUTTON_MASK_LEFT) != 0;
		break;
	case ID_STICKY_RIGHT:
		check = (theApp.autoHold & BUTTON_MASK_RIGHT) != 0;
		break;
	case ID_STICKY_CLEAR:
		check = (theApp.autoHold == 0);
///    pCmdUI->Enable(!check); // FIXME: this is right, but disabling menu items screws up accelerators until you view the menu!
		break;
	}
	pCmdUI->SetCheck(check);
}

LRESULT MainWnd::OnConfirmMode(WPARAM, LPARAM)
{
	// we need to do this separately or the window will not have the right
	// parent. must be related to the way MFC does modal dialogs
	winConfirmMode();
	return 0;
}

void MainWnd::OnOptionsVideoFullscreenmaxscale()
{
	MaxScale dlg;


	dlg.DoModal();
}

void MainWnd::OnOptionsVideoTextdisplayoptions()
{
	TextOptions dlg;


	if (dlg.DoModal() == IDOK)
		systemRefreshScreen();
}

void MainWnd::OnUpdateOptionsVideoTextdisplayoptions(CCmdUI *pCmdUI)
{
	// The status bar takes over the whole HUD, so the on screen text has
	// nothing to configure while it is showing.  Fullscreen has no status
	// bar, so the options stay available there.
	pCmdUI->Enable(!theApp.statusBarHud || isFullscreenOption(theApp.videoOption));
}

void MainWnd::OnToolsStatusbar()
{
	const int wasVisible = statusBarVisible();

	theApp.statusBarHud = !theApp.statusBarHud;
	regSetDwordValue(_T("statusBarHud"), theApp.statusBarHud);

	if (wasVisible == statusBarVisible())
	{
		// Fullscreen ignores the status bar; the setting still applies when
		// the window comes back.
		return;
	}

	const int statusHeight = statusBarHeight();

	if (!IsIconic() && !IsZoomed())
	{
		// Grow or shrink the outer window by exactly the status bar height,
		// which keeps the picture the size the user resized it to.  WM_SIZE
		// lays the children out.
		RECT wr;
		GetWindowRect(&wr);

		int height = wr.bottom - wr.top;
		height += statusBarVisible() ? statusHeight : -statusHeight;

		SetWindowPos(NULL, 0, 0, wr.right - wr.left, height,
		             SWP_NOMOVE | SWP_NOZORDER | SWP_NOACTIVATE);
	}
	else
	{
		// A maximized window cannot grow, so the picture gets the space the
		// status bar takes.
		CRect client;
		GetClientRect(&client);
		layoutRenderArea(client.Width(), client.Height());
		systemRefreshScreen();
	}

	updateStatusBar();

	if (m_renderWnd.GetSafeHwnd())
		m_renderWnd.RedrawWindow(NULL, NULL, RDW_INVALIDATE | RDW_ERASE);
}

void MainWnd::OnUpdateToolsStatusbar(CCmdUI *pCmdUI)
{
	pCmdUI->SetCheck(theApp.statusBarHud);
}


