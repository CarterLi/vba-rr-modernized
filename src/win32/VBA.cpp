// VBA.cpp : Defines the class behaviors for the application.
//
#include "stdafx.h"
#include <mmsystem.h>
#include <cassert>
#include <locale>
#include <share.h>

#include "resource.h"
#include "VBA.h"
#include "AVIWrite.h"
#include "BitmapFont.h"
#include "DarkMode.h"
#include "GameText.h"
#include "Input.h"
#include "IUpdate.h"
#include "MainWnd.h"
#include "Reg.h"
#include "UiFont.h"
#include "VideoEncoders.h"
#include "WavWriter.h"
#include "WinResUtil.h"
#include "WinMiscUtil.h"
#include "Dialogs/LangSelect.h"
#include "Dialogs/ramwatch.h"

#include "../gba/GBA.h"
#include "../gba/GBAGlobals.h"
#include "../gb/GB.h"
#include "../gb/gbGlobals.h"
#include "../gb/gbPrinter.h"
#include "../common/CheatSearch.h"
#include "../gba/RTC.h"
#include "../common/ImageWrite.h"
#include "../common/SystemGlobals.h"
#include "../common/Util.h"
#include "../common/Text.h"
#include "../common/movie.h"
#include "../common/nesvideos-piece.h"
#include "../common/HashTrace.h"
#include "../common/vbalua.h"
#include "../filters/filters.h"
#include "../version.h"

#if VBA_ENABLE_GDI
extern IDisplay *newGDIDisplay();
#endif
#if VBA_ENABLE_DDRAW
extern IDisplay *newDirectDrawDisplay();
#endif
extern IDisplay *newDirect3DDisplay();
#if VBA_ENABLE_OPENGL
extern IDisplay *newOpenGLDisplay();
extern IDisplay *newOpenGL33Display();
#endif
extern IDisplay *newDirect3D11Display();
#if VBA_ENABLE_WGPU
extern IDisplay *newWgpuDisplay();
// Defined in Wgpu.cpp: whether wgpu_native.dll itself is there.  Where it is
// not, the WebGPU entries leave the menu instead of staying greyed.
extern bool wgpuLibraryAvailable();
#endif

// The renderer a build with no stored settings starts on.  A build that
// targets Windows XP starts on Direct3D 9, which every machine that runs XP
// has, and keeps DirectDraw only when that backend is built; every other
// build starts on Direct3D 11.
static DISPLAY_TYPE defaultRenderMethod()
{
#if defined(WINXP)
#if VBA_ENABLE_DDRAW
	return DIRECT_DRAW;
#else
	return DIRECT_3D;
#endif
#else
	return DIRECT_3D11;
#endif
}

extern Input *newDirectInput();

extern void remoteStubSignal(int, int);
extern void remoteOutput(char *, u32);
extern void remoteStubMain();
extern void remoteSetProtocol(int);
extern void remoteCleanUp();


bool debugger = false;

CString movieFileToPlay;
bool playMovieFile		   = false;
bool playMovieFileReadOnly = false;
CString wavFileToOutput;
bool outputWavFile	= false;
bool outputAVIFile	= false;
bool flagHideMenu	= false;
int	 quitAfterTime	= -1;
int	 pauseAfterTime = -1;

// nowhere good to put them to

// True when the text has a character the built-in 8x8 font cannot draw.  That
// font carries 256 glyphs, one per code of the Latin-1 range, and nothing
// beyond them, so it cannot show the letters of a translation.
static bool hasNonAscii(const wchar_t *text)
{
	if (text == NULL)
		return false;

	for (const wchar_t *p = text; *p != 0; p++)
	{
		if ((unsigned)*p > 0x7F)
			return true;
	}

	return false;
}

void DrawTextMessages(u8 *dest, int pitch, int left, int bottom)
{
	// The In Game and On Game modes can use a system raster font instead of
	// the built-in 8x8 bitmap.  gameTextLineHeight() returns 0 when the
	// selected face cannot be configured, in which case the historical
	// built-in renderer takes over.
	const bool builtinGameFont = gameTextFontIsBuiltin();
	const int gameSize = builtinGameFont ? 8 : theApp.gameTextSize;
	const int gameLineHeight = builtinGameFont
		? 0
		: gameTextLineHeight(gameSize);

	// The built-in font is scaled relative to its historical 8 pixel height.
	int percent = (gameSize * 100 + 4) / 8;
	if (percent < 25)
		percent = 25;
	if (percent > 200)
		percent = 200;

	const int lineHeight = gameLineHeight > 0
		? gameLineHeight
		: drawTextLineHeight(percent);

	// The margins are in emulated frame pixels here, so In Game and On Game
	// text moves and stretches with the picture just like the frame does.
	const int x = left + theApp.textMarginLeft;
	const int textBottom = bottom - theApp.textMarginBottom;
	const bool onStatusBar = theApp.statusBarActive();

	for (int slot = 0; slot < SCREEN_MESSAGE_SLOTS; slot++)
	{
		if (theApp.screenMessage[slot])
		{
			if ((theApp.screenMessageDuration[slot] < 0 ||
			     (int)(GetTickCount() - theApp.screenMessageTime[slot]) < theApp.screenMessageDuration[slot]) &&
			    (!theApp.disableStatusMessage || slot == 1 || slot == 2))
			{
				if (onStatusBar)
					continue;

				// The built-in font is ASCII by design, and these two modes
				// draw into the frame with it.  A message that came from the
				// string table has an English rendering of its own, which is
				// what these modes show; anything the core wrote is kept out
				// rather than drawn as so many Latin letters, and the status
				// bar and the On Screen mode have a font that can draw it.
				if (gameLineHeight == 0 && hasNonAscii(theApp.screenMessageAsciiBuffer[slot]))
					continue;

				const int y = textBottom - lineHeight * (slot + 1);

				if (gameLineHeight > 0)
				{
					drawGameText(dest,
					             pitch,
					             x,
					             y,
					             bottom,
					             theApp.screenMessageAsciiBuffer[slot],
					             winAnsiFromWide(theApp.screenMessageColorBuffer[slot]),
					             gameSize);
				}
				else
				{
					drawText(dest,
					         pitch,
					         x,
					         y,
					         winAnsiFromWide(theApp.screenMessageAsciiBuffer[slot]),
					         winAnsiFromWide(theApp.screenMessageColorBuffer[slot]),
					         percent);
				}
			}
			else
			{
				theApp.screenMessage[slot] = false;
			}
		}
	}
}

void directXMessage(const char *msg)
{
	systemMessage(
	    IDS_DIRECTX_7_REQUIRED,
	    "DirectX 7.0 or greater is required to run.\nDownload at http://www.microsoft.com/directx.\n\nError found at: %s",
	    msg);
}

// The time a line was written, as a prefix for it.  A trace covers a whole
// session, and what matters in it is the order of the steps around a change and
// how far apart they were; milliseconds, because a renderer switch writes
// several lines inside one second.
static CString winlogStamp()
{
	SYSTEMTIME now;
	GetLocalTime(&now);

	CString stamp;
	stamp.Format(_T("[%02d:%02d:%02d.%03d] "), (int)now.wHour, (int)now.wMinute,
	             (int)now.wSecond, (int)now.wMilliseconds);
	return stamp;
}

// The CPU core this build carries.  The v7 and the v8 core are shipped as two
// builds and both can run at the same time, so each writes its own trace, named
// after the core it belongs to.
static const char *winlogCoreVersion()
{
#if defined(USE_GB_CORE_V7) || defined(USE_GBA_CORE_V7)
	return "v7";
#else
	return "v8";
#endif
}

// The trace file, opened on the first line and kept open for the whole run:
// opening and closing it around every line was a whole open, seek and close
// per line, and a line a reader could not see while the program was running.
static FILE *winlogFile = NULL;
static bool  winlogOff  = false;

static bool winlogOpen()
{
	char name[64];
	sprintf(name, "vba-trace-%s.log", winlogCoreVersion());

	// Denying writes makes a second instance of the same build fail to open the
	// file, so the two do not interleave into it; that instance then writes no
	// trace at all instead of half of one.  A second instance of the other core
	// has its own file and is unaffected.
	winlogFile = _fsopen(name, "w", _SH_DENYWR);
	if (winlogFile == NULL)
	{
		winlogOff = true;
		return false;
	}

	// The trace is UTF-8.  Windows hands back localized text (encoder names,
	// FormatMessage output) in the ANSI code page, and anything that reads the
	// file as UTF-8 turns that into mojibake.  The byte order mark goes in
	// once, when the file is made, so editors that guess the encoding guess
	// right.
	fwrite("\xEF\xBB\xBF", 1, 3, winlogFile);
	return true;
}

// The file stays open until the program ends, so closing it is a step of the
// shutdown rather than something a line does.
void winlogClose()
{
	if (winlogFile != NULL)
	{
		fclose(winlogFile);
		winlogFile = NULL;
	}

	// Nothing is logged after this, even if a late message comes through.
	winlogOff = true;
}

// Writes one trace line, stamped and in UTF-8.  The text arrives as TCHAR
// because every caller is in this layer; the core does not write the trace.
static void winlogWrite(const CString &text)
{
	if (winlogFile == NULL && !winlogOpen())
		return;

	// One stamp per line, and exactly one line break per line.  Most formats
	// already end with "\n", and a driver log can end with two, so a message is
	// split on its own line breaks and rebuilt instead of being written as it
	// came: that is what used to leave a blank line after every entry.
	CString stamped;

	const int length = text.GetLength();
	int       line   = 0;

	while (line < length)
	{
		int lineEnd = line;
		while (lineEnd < length && text[lineEnd] != L'\n' && text[lineEnd] != L'\r')
			lineEnd++;

		stamped += winlogStamp();
		stamped.Append(text.Mid(line, lineEnd - line));
		stamped += L"\n";

		line = lineEnd;
		while (line < length && (text[line] == L'\n' || text[line] == L'\r'))
			line++;
	}

	// The trace is UTF-8, so a localized name from a driver, or a path with a
	// character outside the ANSI code page in it, reads back as it was written.
	const int utf8Length = WideCharToMultiByte(CP_UTF8, 0, stamped, stamped.GetLength(),
	                                           NULL, 0, NULL, NULL);
	if (utf8Length > 0)
	{
		char *utf8 = new char[utf8Length];

		if (WideCharToMultiByte(CP_UTF8, 0, stamped, stamped.GetLength(),
		                        utf8, utf8Length, NULL, NULL) == utf8Length)
			fwrite(utf8, 1, (size_t)utf8Length, winlogFile);

		delete[] utf8;
	}

	// The handle stays open, but the line is pushed out now: a trace that is
	// followed while the program runs, or read after it crashed, is worth more
	// than the buffer the flush would save.
	fflush(winlogFile);
}

void winlog(LPCTSTR msg, ...)
{
	if (winlogOff)
		return;

	va_list valist;
	va_start(valist, msg);
	CString text;
	text.FormatV(msg, valist);
	va_end(valist);

	winlogWrite(text);
}

// The image writers live in src/common and only know a one argument callback,
// so wrap the variadic trace logger for them.  The "%s" keeps a file name with
// a percent sign in it from being read as a format.
static void winImageWriteLog(const char *message)
{
	winlog(_T("%hs"), message);
}

// code from SDL_main.c for Windows
/* Parse a command line buffer into arguments */

// The command line is TCHAR: a ROM dropped on the executable arrives here, and
// its path is not limited to what the ANSI code page can hold.
static int parseCommandLine(TCHAR *cmdline, TCHAR * *argv)
{
	TCHAR *bufp;
	int	   argc;

	argc = 0;
	for (bufp = cmdline; *bufp; )
	{
		/* Skip leading whitespace */
		while (_istspace(*bufp))
		{
			++bufp;
		}
		/* Skip over argument */
		if (*bufp == '"')
		{
			++bufp;
			if (*bufp)
			{
				if (argv)
				{
					argv[argc] = bufp;
				}
				++argc;
			}
			/* Skip over word */
			while (*bufp && (*bufp != '"'))
			{
				++bufp;
			}
		}
		else
		{
			if (*bufp)
			{
				if (argv)
				{
					argv[argc] = bufp;
				}
				++argc;
			}
			/* Skip over word */
			while (*bufp && !_istspace(*bufp))
			{
				++bufp;
			}
		}
		if (*bufp)
		{
			if (argv)
			{
				*bufp = '\0';
			}
			++bufp;
		}
	}
	if (argv)
	{
		argv[argc] = NULL;
	}
	return(argc);
}

static void debugSystemScreenMessage1(const char *msg)
{
	systemScreenMessage(msg, 3);
}

static void debugSystemScreenMessage2(const char *msg)
{
	systemScreenMessage(msg, 4);
}

/////////////////////////////////////////////////////////////////////////////
// VBA

BEGIN_MESSAGE_MAP(VBA, CWinApp)
//{{AFX_MSG_MAP(VBA)
// NOTE - the ClassWizard will add and remove mapping macros here.
//    DO NOT EDIT what you see in these blocks of generated code!
//}}AFX_MSG_MAP
END_MESSAGE_MAP()

/////////////////////////////////////////////////////////////////////////////
// The one and only VBA object

VBA theApp;

/////////////////////////////////////////////////////////////////////////////
// VBA construction

VBA::VBA() : emulator(::theEmulator)
{
	// important
	{
#ifdef MULTITHREAD_STDLOCALE_WORKAROUND
		// Note: there's a known threading bug regarding std::locale with MSVC according to
		// http://connect.microsoft.com/VisualStudio/feedback/details/492128/std-locale-constructor-modifies-global-locale-via-setlocale
		int iPreviousFlag = ::_configthreadlocale(_ENABLE_PER_THREAD_LOCALE);
#endif
		using std::locale;
		locale::global(locale(locale::classic(), "", locale::collate | locale::ctype));

#ifdef MULTITHREAD_STDLOCALE_WORKAROUND
		if (iPreviousFlag > 0 )
			::_configthreadlocale(iPreviousFlag);
#endif
	}

	// The image writers report every step and every encoder refusal to the
	// trace; the message box only points at it.
	imageWriteSetLogger(winImageWriteLog);

	windowPositionX		 = 0;
	windowPositionY		 = 0;
	filterFunction		 = NULL;
	ifbFunction			 = NULL;
	ifbType				 = 0;
	filterType			 = 0;
	filterWidth			 = 0;
	filterHeight		 = 0;
	lcdFilterGba		 = LCD_FILTER_OFF;
	lcdFilterGbc		 = LCD_FILTER_OFF;
	lcdFilterColorMode	 = 0;
	fsWidth				 = 0;
	fsHeight			 = 0;
	fsColorDepth		 = 0;
	fsForceChange		 = false;
	surfaceSizeX		 = 0;
	surfaceSizeY		 = 0;
	sizeX				 = 0;
	sizeY				 = 0;
	scale				 = 1.0;
	videoOption			 = 0;
	screenScaling		 = SCREEN_SCALING_ASPECT;
	disableStatusMessage = false;
	screenTextFont		 = _T("Consolas");
	gameTextFont		 = GAME_TEXT_FONT_BUILTIN;
	screenTextSize		 = 12;
	gameTextSize		 = 8;
	textMarginLeft		 = 0;
	textMarginBottom	 = 0;
	showSpeed			 = 1;
	showSpeedTransparent = true;
	showRenderedFrames	 = 0;
	for (int j = 0; j < SCREEN_MESSAGE_SLOTS; j++)
	{
		screenMessage[j]		 = false;
		screenMessageTime[j]	 = 0;
		screenMessageDuration[j] = 0;
	}
	menuToggle		 = true;
	display			 = NULL;
	menu			 = NULL;
	popup			 = NULL;
	soundInitialized = false;
	soundApi		 = SOUND_API_DIRECTSOUND;
	useBiosFile		 = false;
	skipBiosIntro	 = false;
	active			 = true;
	paused			 = false;
	recentFreeze	 = false;
	autoSaveLoadCheatList	  = false;
	pauseDuringCheatSearch	  = false;
	modelessCheatDialogIsOpen = false;
//	winout						= NULL;
//	removeIntros				= false;
	autoIPS = true;
	winGbBorderOn	  = 0;
	hideMovieBorder	  = false;
	winFlashSize	  = 0x10000;
	winRtcEnable	  = false;
	winSaveType		  = 0;
	rewindMemory	  = NULL;
	frameSearchMemory = NULL;
	rewindPos		  = 0;
	rewindTopPos	  = 0;
	rewindCounter	  = 0;
	rewindCount		  = 0;
	rewindSaveNeeded  = false;
	rewindTimer		  = 0;
	autoHideMenu	  = false;
	throttle		  = 100;
	throttleLastTime  = 0;
///  autoFrameSkipLastTime		= 0;
///  autoFrameSkip				= false;
	vsync = false;
	changingVideoSize = false;
	pVideoDriverGUID  = NULL;
	renderMethod	  = defaultRenderMethod();
	iconic = false;
	ddrawEmulationOnly		= false;
	ddrawDebug				= false;
	ddrawUseVideoMemory		= false;
	d3dFilter				= 0;
	upscaler				= UPSCALER_OFF;
	upscalerSharpness		= 50;
	upscalerMaxScale		= 0;
	glFilter				= 0;
	glType					= 0;
#if VBA_ENABLE_WGPU
	wgpuFilter				= 0;
	wgpuBackend				= VBA_WGPU_BACKEND_AUTO;
#endif
	regEnabled				= false;
	pauseWhenInactive		= true;
	muteWhenInactive		= true;
	enableBackgroundInput	= false;
	alwaysOnTop				= false;
	useModernFont			= true;
	filenamePreference		= true;
	frameCounter			= false;
	lagCounter				= false;
	extraCounter			= false;
	inputDisplay			= false;
	nextInputDisplay		= false;
	speedupToggle			= false;
	useOldSync				= false;
	allowLeftRight			= false;
	autofireAccountForLag	= false;
	nextframeAccountForLag	= false;
	muteFrameAdvance		= false;
	muteWhenInactive		= false;
	winMuteForNow		= false;
	winGbPrinterEnabled		= false;
	threadPriority			= 2;
	languageOption			= 0;
	languageModule			= NULL;
	languageVersionMismatch	= false;
	languageName			= "";
	renderedFrames			= 0;
	input					= NULL;
	joypadDefault			= 0;
	autoFire				= 0;
	autoFire2				= 0;
	autoHold				= 0;
	autoFireToggle			= false;
	winPauseNextFrame		= false;
	soundRecording			= false;
	soundRecorder			= NULL;
	sound					= NULL;
	aviRecording			= false;
	aviRecorder				= NULL;
	recordingBackend		= RECORDING_NONE;
	ZeroMemory(&recordParams, sizeof(recordParams));
	recordParams.videoBitrate	= VIDEO_BITRATE_DEFAULT;
	recordParams.audioBitrate	= AUDIO_BITRATE_DEFAULT;
	recordParams.profile		= VIDEO_PROFILE_AUTO;
	recordParams.frameRate		= FRAMERATE_DEFAULT;
	recordParams.async			= true;
	painting				= false;
	mouseCounter			= 0;
	movieReadOnly			= true;
	movieEditMode			= false;
	movieOnEndPause			= false;
	movieOnEndBehavior		= 0;
	wasPaused				= false;
	fsMaxScale				= 0;
	romSize					= 0;
	autoLoadMostRecent		= false;
	loadMakesRecent			= false;
	loadMakesCurrent		= false;
	saveMakesCurrent		= false;
	currentSlot				= 0;
	showSlotTime			= false;
	frameSearchLoadValid	= false;
	frameSearching			= false;
	frameSearchSkipping		= false;
	nvVideoLog				= false;
	nvAudioLog				= false;
	LoggingEnabled			= 0;

	updateCount = 0;

	systemSaveUpdateCounter = SYSTEM_SAVE_NOT_UPDATED;

	ZeroMemory(&emulator, sizeof(emulator));

	hAccel = NULL;

	for (int i = 0; i < 24; )
	{
		systemGbPalette[i++] = (0x1f) | (0x1f << 5) | (0x1f << 10);
		systemGbPalette[i++] = (0x15) | (0x15 << 5) | (0x15 << 10);
		systemGbPalette[i++] = (0x0c) | (0x0c << 5) | (0x0c << 10);
		systemGbPalette[i++] = 0;
	}

	VBAMovieInit();

	TIMECAPS tc;
	if (timeGetDevCaps(&tc, sizeof(TIMECAPS)) == TIMERR_NOERROR)
	{
		wmTimerRes = min(max(tc.wPeriodMin, 1), tc.wPeriodMax);
		timeBeginPeriod(wmTimerRes);
	}
	else
	{
		wmTimerRes = 5;
		timeBeginPeriod(wmTimerRes);
	}
}

VBA::~VBA()
{
	if (VBAMovieIsActive())
		VBAMovieStop(true);

	saveSettings();

	InterframeCleanup();

	if (aviRecorder)
	{
		delete aviRecorder;
		aviRecorder	 = NULL;
		aviRecording = false;
	}

	if (soundRecorder)
	{
		delete soundRecorder;
		soundRecorder = NULL;
	}
	soundRecording = false;
	systemSoundPause();
	systemSoundShutdown();

	((MainWnd *)(m_pMainWnd))->winFileClose();

	if (input)
		delete input;

	shutdownDisplay();

	if (rewindMemory)
		free(rewindMemory);

	if (frameSearchMemory)
		free(frameSearchMemory);

	timeEndPeriod(wmTimerRes);

	// The trace was open for the whole run; this is what closes and flushes it.
	winlogClose();
}

/////////////////////////////////////////////////////////////////////////////
// VBA initialization

#include <afxdisp.h>

BOOL VBA::InitInstance()
{
	AfxEnableControlContainer();

	// The trace begins with what build it is: a report that only carries the
	// log then still says which revision, build type and core produced it.
	winlog(_T("%s\n"), _T(VBA_NAME_AND_VERSION));
	winlog(_T("%hs build, %hs core, %hs\n"), VBA_BUILDTYPE_STRING, winlogCoreVersion(),
	       sizeof(void *) == 8 ? "64-bit" : "32-bit");

	SetRegistryKey(_T("VBA"));

	remoteSetProtocol(0);

	systemVerbose = GetPrivateProfileInt(_T("config"), _T("verbose"), 0, _T("VBA.ini"));
	systemDebug = GetPrivateProfileInt(_T("config"), _T("debug"), 0, _T("VBA.ini"));
	ddrawDebug = GetPrivateProfileInt(_T("config"), _T("ddrawDebug"), 0, _T("VBA.ini")) ? true : false;

	wndClass = AfxRegisterWndClass(0, LoadCursor(IDC_ARROW), (HBRUSH)GetStockObject(BLACK_BRUSH), LoadIcon(IDI_ICON));

	TCHAR winBuffer[2048];
	GetModuleFileName(NULL, winBuffer, 2048);
	TCHAR *p = _tcsrchr(winBuffer, _T('\\'));
	if (p)
		*p = 0;
	exeDir = winBuffer;

	regInit(winBuffer);

	// Before the first window: dark mode is a process wide setting and the
	// visual styles have to be in place before the controls are created.
	darkMode::init();

	// Also before the first dialog: this is where the system UI font is
	// picked up and the hook that applies it to every dialog is installed.
	uiFont::init();

	// Also before the first window: the display and the in-frame text renderer
	// draw with fonts that live in the executable's resources, and a resource that
	// is missing is written to the trace here rather than turning into blank text
	// when the first message arrives.
	winBitmapFontInit();

	loadSettings();
	theApp.LuaFastForward = -1;
	if (!initInput())
		return FALSE;

	// The renderer lives with the game: nothing is started here, only the
	// window is sized for the frame the settings ask for.  The render method
	// is started when a ROM is opened (MainWnd::winFileRun), on a picture
	// window made for it, and let go of again when the game is closed.
	updateWindowSize(videoOption);

	hAccel = LoadAccelerators(AfxGetInstanceHandle(), MAKEINTRESOURCE(IDR_ACCELERATOR));

	winAccelMgr.Connect((MainWnd *)m_pMainWnd);

	extern void winAccelAddCommandsFromMenu(CAcceleratorManager & mgr, CMenu * pMenu, const CString &parentStr);
	extern void winAccelAddCommandsFromTable(CAcceleratorManager & mgr);

	winAccelAddCommandsFromMenu(winAccelMgr, &m_menu, CString());
	winAccelAddCommandsFromTable(winAccelMgr);

	winAccelMgr.CreateDefaultTable();
	winAccelMgr.Load();
	winAccelMgr.UpdateWndTable();
	winAccelMgr.UpdateMenu(menu);

	if (m_lpCmdLine[0])
	{
		int		argc = parseCommandLine(m_lpCmdLine, NULL);
		TCHAR * *argv = (TCHAR * *)malloc((argc + 1) * sizeof(TCHAR *));
		parseCommandLine(m_lpCmdLine, argv);

		bool gotFlag = false, enoughArgs = false;
		for (int i = 0; i < argc; i++)
		{
			if (argv[i][0] == '-' || gotFlag)
			{
				if (!gotFlag)
					loadSettings();
				gotFlag = true;
				if (_tcsicmp(argv[i], _T("-rom")) == 0)
				{
					if (i + 1 >= argc || argv[i + 1][0] == '-')
						goto invalidArgument;
					romFilename = argv[++i];
					winCorrectPath(romFilename);
					gameFilename = romFilename;
				}
				else if (_tcsicmp(argv[i], _T("-bios")) == 0)
				{
					if (i + 1 >= argc || argv[i + 1][0] == '-')
						goto invalidArgument;
					biosFileName = argv[++i];
					winCorrectPath(biosFileName);

					//systemLoadBIOS();
				}
				else if (_tcsicmp(argv[i], _T("-frameskip")) == 0)
				{
					if (i + 1 >= argc || argv[i + 1][0] == '-')
						goto invalidArgument;
					frameSkip = _ttoi(argv[++i]);
					if (frameSkip < 0)
						frameSkip = 0;
					if (frameSkip > 9)
						frameSkip = 9;
					gbFrameSkip = frameSkip;
				}
				else if (_tcsicmp(argv[i], _T("-throttle")) == 0)
				{
					if (i + 1 >= argc || argv[i + 1][0] == '-')
						goto invalidArgument;
					throttle = _ttoi(argv[++i]);
					if (throttle < 5)
						throttle = 5;
					if (throttle > 1000)
						throttle = 1000;
				}
				else if (_tcsicmp(argv[i], _T("-throttleKeepPitch")) == 0)
				{
					if (i + 1 >= argc || argv[i + 1][0] == '-')
						goto invalidArgument;
					accuratePitchThrottle = _ttoi(argv[++i]) != 0;
				}
				else if (_tcsicmp(argv[i], _T("-synchronize")) == 0)
				{
					if (i + 1 >= argc || argv[i + 1][0] == '-')
						goto invalidArgument;
					synchronize = _ttoi(argv[++i]) != 0;
				}
				else if (_tcsicmp(argv[i], _T("-hideborder")) == 0)
				{
					if (i + 1 >= argc || argv[i + 1][0] == '-')
						goto invalidArgument;
					hideMovieBorder = _ttoi(argv[++i]) != 0;
				}
				else if (_tcsicmp(argv[i], _T("-play")) == 0)
				{
					playMovieFile = true;
					if (i + 1 >= argc || argv[i + 1][0] == '-')
						goto invalidArgument;
					movieFileToPlay = argv[++i];
					winCorrectPath(movieFileToPlay);
					if (i + 1 >= argc || argv[i + 1][0] == '-') { --i; goto invalidArgument; }
					playMovieFileReadOnly = _ttoi(argv[++i]) != 0;
				}
				else if (_tcsicmp(argv[i], _T("-hashtrace")) == 0)
				{
					// Per-frame state hash written to CSV, for proving that a
					// refactor did not change behaviour.
					// e.g. vba.exe -rom game.gba -play movie.vbm 1 -hashtrace D:\trace.csv
					if (i + 1 >= argc || argv[i + 1][0] == '-')
						goto invalidArgument;
					hashTraceEnable(winAnsiFromWide(argv[++i]));
				}
				else if (_tcsicmp(argv[i], _T("-videoLog")) == 0)
				{
					nvVideoLog	   = true;
					nvAudioLog	   = true;
					LoggingEnabled = 2;
					if (i + 1 >= argc || argv[i + 1][0] == '-') {}
					else
						NESVideoSetVideoCmd(winAnsiFromWide(argv[++i]));
				}
				else if (_tcsicmp(argv[i], _T("-logDebug")) == 0)
				{
					NESVideoEnableDebugging(debugSystemScreenMessage1, debugSystemScreenMessage2);
				}
				else if (_tcsicmp(argv[i], _T("-logToFile")) == 0)
				{
					NESVideoSetFileFuncs(fopen, fclose);
				}
				else if (_tcsicmp(argv[i], _T("-outputWAV")) == 0)
				{
					outputWavFile = true;
					if (i + 1 >= argc || argv[i + 1][0] == '-')
						goto invalidArgument;
					wavFileToOutput = argv[++i];
				}
				else if (_tcsicmp(argv[i], _T("-outputAVI")) == 0)
				{
					outputAVIFile = true;
				}
				else if (_tcsicmp(argv[i], _T("-quitAfter")) == 0)
				{
					if (i + 1 >= argc || argv[i + 1][0] == '-')
						goto invalidArgument;
					quitAfterTime = _ttoi(argv[++i]);
				}
				else if (_tcsicmp(argv[i], _T("-pauseAt")) == 0)
				{
					if (i + 1 >= argc || argv[i + 1][0] == '-')
						goto invalidArgument;
					pauseAfterTime = _ttoi(argv[++i]);
				}
				else if (_tcsicmp(argv[i], _T("-videoScale")) == 0)
				{
					if (i + 1 >= argc || argv[i + 1][0] == '-')
						goto invalidArgument;
					int size = _ttoi(argv[++i]);
					if (size < 1)
						size = 1;
					if (size > 12)
						size = 12;
					switch (size)
					{
					case 1:
						videoOption = VIDEO_1X; break;
					case 2:
						videoOption = VIDEO_2X; break;
					case 3:
						videoOption = VIDEO_3X; break;
					case 4:
						videoOption = VIDEO_4X; break;
					case 5:
						videoOption = VIDEO_5X; break;
					case 6:
						videoOption = VIDEO_6X; break;
					case 7:
						videoOption = VIDEO_7X; break;
					case 8:
						videoOption = VIDEO_8X; break;
					case 9:
						videoOption = VIDEO_9X; break;
					case 10:
						videoOption = VIDEO_10X; break;
					case 11:
						videoOption = VIDEO_11X; break;
					case 12:
						videoOption = VIDEO_12X; break;
					}
				}
				else if (_tcsicmp(argv[i], _T("-hideMenu")) == 0)
				{
					flagHideMenu = true;
				}
				else
				{
					enoughArgs = true;
invalidArgument:
					TCHAR str[2048];    // the string is larger than 1024 bytes
					_tcscpy(str, _T(""));
					if (_tcsicmp(argv[i], _T("-h")) != 0)
						if (enoughArgs)
							_stprintf(str, _T("Invalid commandline argument %d: %s\n"), i, argv[i]);
						else
							_stprintf(str, _T("Not enough arguments for arg %d: %s\n"), i, argv[i]);
					_tcscat(str, L"Valid commands:\n"
					            L"-h \t\t\t displays this help\n"
					            L"-rom filename \t\t opens the given ROM\n"
					            L"-bios filename \t\t use the given GBA BIOS\n"
					            L"-play filename val \t\t plays the given VBM movie (val: 1 = read-only, 0 = editable)\n"
					            L"-outputWAV filename \t outputs WAV audio to the given file\n"
					            L"-outputAVI \t\t outputs an AVI (you are prompted for location and codec)\n"
					            L"-frameskip val \t\t sets the frameskip amount to the given value\n"
					            L"-synchronize val \t\t limits running speed to sound playing speed, (0 = off, 1 = on)\n"
					            L"-throttle val \t\t sets the throttle speed to the given percentage\n"
					            L"-hideborder val \t\t hides SGB border, if any (0 = show, 1 = hide)\n"
					            L"-throttleKeepPitch val \t if throttle and synch, don't change sound freq (0 = off, 1 = on)\n"
					            L"-quitAfter val \t\t close program when frame counter == val\n"
					            L"-pauseAt val \t\t pause (movie) once when frame counter == val\n"
					            L"-videoScale val \t\t sets the video size (val = 1..8 for 1X..8X)\n"
					            L"-hideMenu \t\t hides the menu until program exit\n"
					            L"\n"
					            L"-videoLog args \t does (nesvideos) video+audio logging with the given arguments\n"
					            L"-logToFile \t tells logging to use fopen/fclose of args, if logging is enabled\n"
					            L"-logDebug  \t tells logging to output debug info to screen, if logging is enabled\n"
					       );
					AfxGetApp()->m_pMainWnd->MessageBox(str, _T("Commandline Help"), MB_OK | MB_ICONINFORMATION);
					exit(0);
				}
			}
			else
			{
				// assume anything else is a ROM, for backward compatibility
				romFilename	 = argv[i++];
				gameFilename = romFilename;
				loadSettings();
			}
		}

/*
        int index = filename.ReverseFind('.');

        if (index != -1)
            filename = filename.Left(index);
 */
		if (romFilename.GetLength() > 0)
		{
			((MainWnd *)theApp.m_pMainWnd)->winFileRun();
		}
		free(argv);
	}

	// The window is up, the display is running and the command line has been
	// obeyed, so the message loop takes over from here.
	winlog(_T("ready, handing control to the user"));

	// input was set up before the window existed, so the controllers it found
	// could not be reported to the user yet
	if (input)
		input->announceDevices();

	return TRUE;
}

CWnd *VBA::winRenderWnd()
{
	// The picture is the child in both modes.  Full screen used to draw into
	// the main window itself, which made it impossible to leave anything of
	// the screen unused; the child is sized and centred there just like it
	// is windowed, and the main window around it stays black.
	if (m_pMainWnd != NULL)
	{
		RenderWnd *render = ((MainWnd *)m_pMainWnd)->renderWnd();
		if (render != NULL && render->GetSafeHwnd() != NULL)
			return render;
	}

	return m_pMainWnd;
}

bool VBA::statusBarActive() const
{
	// Fullscreen has no status bar; the on screen display comes back there.
	return statusBarHud && !isFullscreenOption(videoOption);
}

void VBA::updateStatusBar()
{
	if (m_pMainWnd != NULL)
		((MainWnd *)m_pMainWnd)->updateStatusBar();
}

void VBA::adjustDestRect()
{
	POINT point;

	CWnd *pWnd = winRenderWnd();
	if (pWnd == NULL)
		return;

	point.x = 0;
	point.y = 0;

	pWnd->ClientToScreen(&point);
	dest.top  = point.y;
	dest.left = point.x;

	point.x = surfaceSizeX;
	point.y = surfaceSizeY;

	pWnd->ClientToScreen(&point);
	dest.bottom = point.y;
	dest.right	= point.x;
}

// Size of the picture for the area it is given.  The aspect ratio comes from
// the emulated frame and not from theApp.rect: a pixel filter can make the
// texture larger than the frame, and the picture would then be fitted to the
// filter's extra rows.
void VBA::fitSurfaceTo(int width, int height, bool fullscreen)
{
	if (width < 1)
		width = 1;
	if (height < 1)
		height = 1;

	if (sizeX < 1 || sizeY < 1)
	{
		scale		 = 1.0;
		surfaceSizeX = width;
		surfaceSizeY = height;
		return;
	}

	if (screenScaling == SCREEN_SCALING_STRETCH)
	{
		scale		 = (double)width / (double)sizeX;
		surfaceSizeX = width;
		surfaceSizeY = height;
		return;
	}

	const double fitX = (double)width / (double)sizeX;
	const double fitY = (double)height / (double)sizeY;
	double		 fit  = fitX < fitY ? fitX : fitY;

	// The full screen maximum scale is a cap on the picture and not a size,
	// so it belongs to the two modes that choose their own scale.
	if (fullscreen && fsMaxScale > 0 && fit > fsMaxScale)
		fit = fsMaxScale;

	if (screenScaling == SCREEN_SCALING_INTEGER)
	{
		// Whole multiples only, and an area too small for one is fitted
		// exactly instead, which is better than a picture that is cropped.
		const int whole = (int)(fit + 1e-6);
		if (whole >= 1)
			fit = whole;
	}

	if (fit <= 0.0)
		fit = 1.0;

	scale = fit;

	// Rounded rather than truncated: an area that matches the aspect ratio
	// exactly works out to a whole number of pixels, and truncating the
	// smallest error of the division would leave a black line on two sides.
	surfaceSizeX = (int)(sizeX * fit + 0.5);
	surfaceSizeY = (int)(sizeY * fit + 0.5);

	if (surfaceSizeX < 1)
		surfaceSizeX = 1;
	if (surfaceSizeY < 1)
		surfaceSizeY = 1;
}

void VBA::updateIFB()
{
	if (systemColorDepth == 16)
	{
		switch (ifbType)
		{
		case 0:
		default:
			ifbFunction = NULL;
			break;
		case 1:
			ifbFunction = MotionBlurIB;
			break;
		case 2:
			ifbFunction = SmartIB;
			break;
		}
	}
	else if (systemColorDepth == 32)
	{
		switch (ifbType)
		{
		case 0:
		default:
			ifbFunction = NULL;
			break;
		case 1:
			ifbFunction = MotionBlurIB32;
			break;
		case 2:
			ifbFunction = SmartIB32;
			break;
		}
	}
	else
		ifbFunction = NULL;
}

void VBA::updateFilter()
{
	filterWidth	 = sizeX;
	filterHeight = sizeY;

	if (systemColorDepth == 16 && (videoOption > VIDEO_1X &&
	                               videoOption != VIDEO_320x240))
	{
		switch (filterType)
		{
		default:
		case 0:
			filterFunction = NULL;
			break;
		case 1:
			filterFunction = ScanlinesTV;
			break;
		case 2:
			filterFunction = _2xSaI;
			break;
		case 3:
			filterFunction = Super2xSaI;
			break;
		case 4:
			filterFunction = SuperEagle;
			break;
		case 5:
			filterFunction = Pixelate2x16;
			break;
		case 6:
			filterFunction = MotionBlur;
			break;
		case 7:
			filterFunction = AdMame2x;
			break;
		case 8:
			filterFunction = Simple2x16;
			break;
		case 9:
			filterFunction = Bilinear;
			break;
		case 10:
			filterFunction = BilinearPlus;
			break;
		case 11:
			filterFunction = Scanlines;
			break;
		case 12:
			filterFunction = hq2xS;
			break;
		case 13:
			filterFunction = hq2x;
			break;
		case 14:
			filterFunction = lq2x;
			break;
		case 15:
			filterFunction = hq3xS;
			break;
		case 16:
			filterFunction = hq3x;
			break;
		case 17:
			filterFunction = Simple3x16;
			break;
		case 18:
			filterFunction = Simple4x16;
			break;
		case 19:
			filterFunction = Pixelate3x16;
			break;
		case 20:
			filterFunction = Pixelate4x16;
			break;
		case 21: // xBRZ is 32-bit only
		case 22:
		case 23:
		case 24: // xBRZ 5x is 32-bit only
		case 26: // ScaleFX is 32-bit only
			filterFunction = NULL;
			break;
		case 25:
			filterFunction = hq4x;
			break;
		}
		switch (filterType)
		{
		case 0: // normal -> 1x texture
		case 21: // xBRZ has no 16-bit variant -> no filter
		case 22:
		case 23:
		case 24:
		case 26:
			rect.right	= sizeX;
			rect.bottom = sizeY;
			break;
		default: // other -> 2x texture
			rect.right	= sizeX * 2;
			rect.bottom = sizeY * 2;
			memset(delta, 255, sizeof(delta));
			break;
		case 15: // hq3x -> 3x texture
		case 16:
		case 17:
		case 19:
			rect.right	= sizeX * 3;
			rect.bottom = sizeY * 3;
			memset(delta, 255, sizeof(delta));
			break;
		case 18: // Simple4x -> 4x texture
		case 20:
		case 25: // hq4x -> 4x texture
			rect.right	= sizeX * 4;
			rect.bottom = sizeY * 4;
			memset(delta, 255, sizeof(delta));
			break;
		}
	}
	else
	{
		if (systemColorDepth == 32 && videoOption > VIDEO_1X &&
		    videoOption != VIDEO_320x240)
		{
			switch (filterType)
			{
			default:
			case 0:
				filterFunction = NULL;
				break;
			case 1:
				filterFunction = ScanlinesTV32;
				break;
			case 2:
				filterFunction = _2xSaI32;
				break;
			case 3:
				filterFunction = Super2xSaI32;
				break;
			case 4:
				filterFunction = SuperEagle32;
				break;
			case 5:
				filterFunction = Pixelate2x32;
				break;
			case 6:
				filterFunction = MotionBlur32;
				break;
			case 7:
				filterFunction = AdMame2x32;
				break;
			case 8:
				filterFunction = Simple2x32;
				break;
			case 9:
				filterFunction = Bilinear32;
				break;
			case 10:
				filterFunction = BilinearPlus32;
				break;
			case 11:
				filterFunction = Scanlines32;
				break;
			case 12:
				filterFunction = hq2xS32;
				break;
			case 13:
				filterFunction = hq2x32;
				break;
			case 14:
				filterFunction = lq2x32;
				break;
			case 15:
				filterFunction = hq3xS32;
				break;
			case 16:
				filterFunction = hq3x32;
				break;
			case 17:
				filterFunction = Simple3x32;
				break;
			case 18:
				filterFunction = Simple4x32;
				break;
			case 19:
				filterFunction = Pixelate3x32;
				break;
			case 20:
				filterFunction = Pixelate4x32;
				break;
			case 21:
				filterFunction = xbrz2x32;
				break;
			case 22:
				filterFunction = xbrz3x32;
				break;
			case 23:
				filterFunction = xbrz4x32;
				break;
			case 24:
				filterFunction = xbrz5x32;
				break;
			case 25:
				filterFunction = hq4x32;
				break;
			case 26:
				filterFunction = scalefx3x32;
				break;
			}
			switch (filterType)
			{
			case 0: // normal -> 1x texture
				rect.right	= sizeX;
				rect.bottom = sizeY;
				memset(delta, 255, sizeof(delta));
				break;
			default: // other -> 2x texture
				rect.right	= sizeX * 2;
				rect.bottom = sizeY * 2;
				memset(delta, 255, sizeof(delta));
				break;
			case 15: // hq3x -> 3x texture
			case 16:
			case 17:
			case 19:
			case 22: // xBRZ 3x
			case 26: // ScaleFX 3x
				rect.right	= sizeX * 3;
				rect.bottom = sizeY * 3;
				memset(delta, 255, sizeof(delta));
				break;
			case 18: // Simple4x -> 4x texture
			case 20:
			case 23: // xBRZ 4x
			case 25: // hq4x
				rect.right	= sizeX * 4;
				rect.bottom = sizeY * 4;
				memset(delta, 255, sizeof(delta));
				break;
			case 24: // xBRZ 5x -> 5x texture
				rect.right	= sizeX * 5;
				rect.bottom = sizeY * 5;
				memset(delta, 255, sizeof(delta));
				break;
			}
		}
		else
			filterFunction = NULL;
	}

	if (display)
		display->changeRenderSize(rect.right, rect.bottom);
}

#if VBA_ENABLE_WGPU
// The popup the command lives in, and where in it, so the entries the resource
// carries can be taken out by position: the separators around them have no id
// to be found by.
static bool findMenuCommand(HMENU menu, UINT id, HMENU *parent, int *position)
{
	int count = GetMenuItemCount(menu);

	for (int i = 0; i < count; i++)
	{
		HMENU popup = GetSubMenu(menu, i);
		if (popup != NULL)
		{
			if (findMenuCommand(popup, id, parent, position))
				return true;
		}
		else if (GetMenuItemID(menu, i) == (int)id)
		{
			*parent   = menu;
			*position = i;
			return true;
		}
	}

	return false;
}

// wgpu_native.dll is part of no Windows, and a build made with the WebGPU
// renderer in it lists the entries whether the library is installed or not.
// Where it is not there, the family is taken out of the menu rather than left
// greyed: an entry that can never be chosen only makes the list longer.
static void removeWgpuMenuItems(HMENU menu)
{
	HMENU parent;
	int   position;

	if (!findMenuCommand(menu, ID_OPTIONS_VIDEO_RENDERMETHOD_WEBGPU, &parent, &position))
		return;

	// The backend menu is the popup right under the entry, and it goes first:
	// taking the entry out by command would leave the popup behind, and its
	// position would then be the one the entry had.
	if (position + 1 < GetMenuItemCount(parent) &&
	    (GetMenuState(parent, position + 1, MF_BYPOSITION) & MF_POPUP) != 0)
		DeleteMenu(parent, position + 1, MF_BYPOSITION);

	// The separator that opens the family has no id of its own, so it is
	// found by the position of the entry it stands in front of.
	if (position > 0 &&
	    (GetMenuState(parent, position - 1, MF_BYPOSITION) & MF_SEPARATOR) != 0)
		DeleteMenu(parent, position - 1, MF_BYPOSITION);

	DeleteMenu(parent, ID_OPTIONS_VIDEO_RENDERMETHOD_WEBGPU, MF_BYCOMMAND);
}
#endif

void VBA::recreateMenuBar()
{
	m_menu.Detach();
	m_menu.Attach(winResLoadMenu(MAKEINTRESOURCE(IDR_MENU)));

#if VBA_ENABLE_WGPU
	// The entries leave the menu with the library, on every rebuild, so a
	// library dropped next to the executable brings them back on the next
	// start.
	if (!wgpuLibraryAvailable())
		removeWgpuMenuItems(m_menu.GetSafeHmenu());
#endif

	if (m_pMainWnd && menuToggle)   // assuming that whether the menu has been set is always kept tracked
	{
		m_pMainWnd->SetMenu(&m_menu);
	}

	if (menu != NULL)
	{
		DestroyMenu(menu);
	}

	menu = m_menu.GetSafeHmenu();
}

void VBA::updateMenuBar()
{
	//if (flagHideMenu)
	//	return;

	recreateMenuBar();

	if (popup != NULL)
	{
		// force popup recreation if language changed
		DestroyMenu(popup);
		popup = NULL;
	}
}

void VBA::saveRewindStateIfNecessary()
{
	if (rewindSaveNeeded && rewindMemory && emulator.emuWriteMemState)
	{
		rewindCount++;
		if (rewindCount > rewindSlots)
			rewindCount = rewindSlots;
		assert(rewindPos >= 0 && rewindPos < rewindSlots);
		if (emulator.emuWriteMemState(&rewindMemory[rewindPos * REWIND_SIZE], REWIND_SIZE))
		{
			rewindPos = ++rewindPos % rewindSlots;
			assert(rewindPos >= 0 && rewindPos < rewindSlots);
			if (rewindCount == rewindSlots)
				rewindTopPos = ++rewindTopPos % rewindSlots;
		}
	}

	// also update/cache some frame search stuff
	if (frameSearching)
	{
		extern SMovie Movie;
		int curFrame = (Movie.state == MOVIE_STATE_NONE) ? systemCounters.frameCount : Movie.currentFrame;
		int endFrame = theApp.frameSearchStart + theApp.frameSearchLength;
		frameSearchSkipping	 = (curFrame < endFrame);
		frameSearchFirstStep = false;

		if (curFrame == endFrame)
		{
			// cache intermediate state to speed up searching forward
			emulator.emuWriteMemState(&frameSearchMemory[REWIND_SIZE * 1], REWIND_SIZE);
		}

		if (curFrame == endFrame + 1)
		{
			emulator.emuWriteMemState(&frameSearchMemory[REWIND_SIZE * 2], REWIND_SIZE);
			frameSearchLoadValid = true;
		}
	}
	else
	{
		frameSearchFirstStep = false;

		assert(!frameSearchSkipping);
		// just in case
		frameSearchSkipping = false;
	}
}

BOOL VBA::OnIdle(LONG lCount)
{
	if (emulating && debugger)
	{
		MSG msg;
		remoteStubMain();
		if (debugger)
			return TRUE;  // continue loop
		return !::PeekMessage(&msg, NULL, NULL, NULL, PM_NOREMOVE);
	}
	else if (emulating && active && !paused)
	{
///    for(int i = 0; i < 2; i++)
		{
			emulator.emuMain(emulator.emuCount);

			// save the state for rewinding, if necessary
			saveRewindStateIfNecessary();

			rewindSaveNeeded = false;
		}

		if (mouseCounter)
		{
			if (--mouseCounter == 0)
			{
				SetCursor(NULL);
			}
		}
		return TRUE;
	}
	else if (emulating) // this fixes display if resetting while paused
	{
		VBAUpdateButtonPressDisplay();
		VBAUpdateFrameCountDisplay();
		systemRefreshScreen();
	}

	return FALSE;

	//  return CWinApp::OnIdle(lCount);
}

void VBA::addRecentFile(const CString &file)
{
	// Do not change recent list if frozen
	if (recentFreeze)
		return;
	int i = 0;
	for (i = 0; i < 10; ++i)
	{
		if (recentFiles[i].GetLength() == 0)
			break;

		if (recentFiles[i].Compare(file) == 0)
		{
			if (i == 0)
				return;
			CString p = recentFiles[i];
			for (int j = i; j > 0; --j)
			{
				recentFiles[j] = recentFiles[j - 1];
			}
			recentFiles[0] = p;
			return;
		}
	}
	int num = 0;
	for (i = 0; i < 10; ++i)
	{
		if (recentFiles[i].GetLength() != 0)
			++num;
	}
	if (num == 10)
	{
		--num;
	}

	for (i = num; i >= 1; --i)
	{
		recentFiles[i] = recentFiles[i - 1];
	}
	recentFiles[0] = file;
}

void VBA::updateFrameSkip()
{
	switch (systemCartridgeType)
	{
	case 0:
		systemFrameSkip = frameSkip;
		break;
	case 1:
		systemFrameSkip = gbFrameSkip;
		break;
	}
}

void VBA::updateVideoSize(UINT id)
{
	int	 value		 = 0;
	bool forceUpdate = false;

	switch (id)
	{
	case ID_OPTIONS_VIDEO_X1:
		value		= VIDEO_1X;
		forceUpdate = true;
		break;
	case ID_OPTIONS_VIDEO_X2:
		value		= VIDEO_2X;
		forceUpdate = true;
		break;
	case ID_OPTIONS_VIDEO_X3:
		value		= VIDEO_3X;
		forceUpdate = true;
		break;
	case ID_OPTIONS_VIDEO_X4:
		value		= VIDEO_4X;
		forceUpdate = true;
		break;
	case ID_OPTIONS_VIDEO_X5:
		value		= VIDEO_5X;
		forceUpdate = true;
		break;
	case ID_OPTIONS_VIDEO_X6:
		value		= VIDEO_6X;
		forceUpdate = true;
		break;
	case ID_OPTIONS_VIDEO_X7:
		value		= VIDEO_7X;
		forceUpdate = true;
		break;
	case ID_OPTIONS_VIDEO_X8:
		value		= VIDEO_8X;
		forceUpdate = true;
		break;
	case ID_OPTIONS_VIDEO_X9:
		value		= VIDEO_9X;
		forceUpdate = true;
		break;
	case ID_OPTIONS_VIDEO_X10:
		value		= VIDEO_10X;
		forceUpdate = true;
		break;
	case ID_OPTIONS_VIDEO_X11:
		value		= VIDEO_11X;
		forceUpdate = true;
		break;
	case ID_OPTIONS_VIDEO_X12:
		value		= VIDEO_12X;
		forceUpdate = true;
		break;
	case ID_OPTIONS_VIDEO_FULLSCREEN320X240:
		value		 = VIDEO_320x240;
		fsWidth		 = 320;
		fsHeight	 = 240;
		fsColorDepth = 16;
		break;
	case ID_OPTIONS_VIDEO_FULLSCREEN640X480:
		value		 = VIDEO_640x480;
		fsWidth		 = 640;
		fsHeight	 = 480;
		fsColorDepth = 16;
		break;
	case ID_OPTIONS_VIDEO_FULLSCREEN800X600:
		value		 = VIDEO_800x600;
		fsWidth		 = 800;
		fsHeight	 = 600;
		fsColorDepth = 16;
		break;
	case ID_OPTIONS_VIDEO_FULLSCREEN:
		value		= VIDEO_OTHER;
		forceUpdate = true;
		break;
	}

	if (videoOption != value || forceUpdate)
		updateWindowSize(value);
}

void VBA::updateWindowSize(int value)
{
	regSetDwordValue(_T("video"), value);

	if (value == VIDEO_OTHER)
	{
		regSetDwordValue(_T("fsWidth"), fsWidth);
		regSetDwordValue(_T("fsHeight"), fsHeight);
		regSetDwordValue(_T("fsColorDepth"), fsColorDepth);
	}

	if (display &&
	    (((isFullscreenOption(value) || isFullscreenOption(videoOption)) && videoOption != value) ||
	     fsForceChange))
	{
		fsForceChange = false;
		videoOption	  = value;
		initDisplay();
	}

	videoOption = value;

	systemGetLCDResolution(sizeX, sizeY);
	systemGetLCDBaseOffset(gbBorderColumnSkip, gbBorderRowSkip);
	gbBorderLineSkip = sizeX;

	switch (videoOption)
	{
	case VIDEO_1X:
		scale = 1;
		surfaceSizeX = sizeX;
		surfaceSizeY = sizeY;
		break;
	case VIDEO_2X:
		scale = 2;
		surfaceSizeX = sizeX * 2;
		surfaceSizeY = sizeY * 2;
		break;
	case VIDEO_3X:
		scale = 3;
		surfaceSizeX = sizeX * 3;
		surfaceSizeY = sizeY * 3;
		break;
	case VIDEO_4X:
		scale = 4;
		surfaceSizeX = sizeX * 4;
		surfaceSizeY = sizeY * 4;
		break;
	case VIDEO_5X:
		scale = 5;
		surfaceSizeX = sizeX * 5;
		surfaceSizeY = sizeY * 5;
		break;
	case VIDEO_6X:
		scale = 6;
		surfaceSizeX = sizeX * 6;
		surfaceSizeY = sizeY * 6;
		break;
	case VIDEO_7X:
		scale = 7;
		surfaceSizeX = sizeX * 7;
		surfaceSizeY = sizeY * 7;
		break;
	case VIDEO_8X:
		scale = 8;
		surfaceSizeX = sizeX * 8;
		surfaceSizeY = sizeY * 8;
		break;
	case VIDEO_9X:
		scale = 9;
		surfaceSizeX = sizeX * 9;
		surfaceSizeY = sizeY * 9;
		break;
	case VIDEO_10X:
		scale = 10;
		surfaceSizeX = sizeX * 10;
		surfaceSizeY = sizeY * 10;
		break;
	case VIDEO_11X:
		scale = 11;
		surfaceSizeX = sizeX * 11;
		surfaceSizeY = sizeY * 11;
		break;
	case VIDEO_12X:
		scale = 12;
		surfaceSizeX = sizeX * 12;
		surfaceSizeY = sizeY * 12;
		break;
	case VIDEO_320x240:
	case VIDEO_640x480:
	case VIDEO_800x600:
	case VIDEO_OTHER:
		// The window covers the whole monitor and the picture is fitted
		// into it; MainWnd::layoutRenderArea() repeats this once the
		// window is up, with the part of the client area that is really on
		// the screen.
		fitSurfaceTo(fsWidth, fsHeight, true);
		break;
	}

	rect.left	= 0;
	rect.top	= 0;
	rect.right	= sizeX;
	rect.bottom = sizeY;

	int winSizeX = 0;
	int winSizeY = 0;
	int x		 = 0;
	int y		 = 0;

	DWORD style	  = WS_POPUP | WS_VISIBLE | WS_CLIPCHILDREN;
	DWORD styleEx = alwaysOnTop ? WS_EX_TOPMOST : 0;

	if (!isFullscreenOption(videoOption))
	{
		style |= WS_OVERLAPPEDWINDOW;

		dest.left	= 0;
		dest.top	= 0;
		dest.right	= surfaceSizeX;
		dest.bottom = surfaceSizeY;

		RECT workAreaRect;
		SystemParametersInfo(SPI_GETWORKAREA, 0, &workAreaRect, 0);
		if (windowPositionX + surfaceSizeX < workAreaRect.left || windowPositionX > workAreaRect.right)
			windowPositionX = (workAreaRect.left + workAreaRect.right - surfaceSizeX) / 2;
		if (windowPositionY + surfaceSizeY < workAreaRect.top || windowPositionY > workAreaRect.bottom)
			windowPositionY = (workAreaRect.top + workAreaRect.bottom - surfaceSizeY) / 2;

		x = windowPositionX;
		y = windowPositionY;
	}
	else
	{
		dest.left	= 0;
		dest.top	= 0;
		dest.right	= fsWidth;
		dest.bottom = fsHeight;
	}

	AdjustWindowRectEx(&dest, style, flagHideMenu ? FALSE : TRUE, styleEx);
	winSizeX = dest.right  - dest.left;
	winSizeY = dest.bottom - dest.top;

	if (m_pMainWnd == NULL)
	{
		// Create a new window.  It stays hidden until its client area has
		// been painted black: a window created visible shows the window
		// manager's default surface first, which reads as a white flash
		// whenever the render method changes.
		m_pMainWnd = new MainWnd;
		m_pMainWnd->CreateEx(styleEx,
		                     theApp.wndClass,
		                     _T(VBA_NAME_AND_VERSION),
		                     style & ~WS_VISIBLE,
		                     x, y, winSizeX, winSizeY,
		                     NULL,
		                     0);

		if (!(HWND)*m_pMainWnd)
		{
			winlog(_T("Error creating Window %08x\n"), GetLastError());
			AfxPostQuitMessage(0);
			return;
		}
	}
	else
	{
		m_pMainWnd->SetWindowPos(0, //HWND_TOPMOST,
								 x,
								 y,
								 winSizeX,
								 winSizeY,
								 SWP_NOMOVE | SWP_SHOWWINDOW);
	}

	// The menu bar is part of the window frame, so it has to be there before
	// the size below is corrected to the client area the picture needs.
	updateMenuBar();
	winAccelMgr.UpdateMenu(menu);

	if (!isFullscreenOption(videoOption))
	{
		RECT clientRect;
		m_pMainWnd->GetClientRect(&clientRect);

		// The status bar is below the picture, so the window has to be that
		// much taller for the render area to keep its size.
		const int statusHeight = statusBarActive() ?
		                             ((MainWnd *)m_pMainWnd)->statusBarHeight() : 0;

		winSizeX += int(sizeX * scale) - clientRect.right;
		winSizeY += int(sizeY * scale) - clientRect.bottom + statusHeight;
	}

	m_pMainWnd->SetWindowPos(0, //HWND_TOPMOST,
							x,
							y,
							winSizeX,
							winSizeY,
							SWP_NOMOVE);

	// The size is final now, so the surface can be made black before the
	// window is shown: this is what the display would draw anyway, and it
	// removes the white frame the window manager would show first.
	if (!m_pMainWnd->IsWindowVisible())
	{
		HWND wnd    = m_pMainWnd->GetSafeHwnd();
		HDC  dc     = ::GetDC(wnd);
		RECT client;
		::GetClientRect(wnd, &client);
		::FillRect(dc, &client, (HBRUSH)::GetStockObject(BLACK_BRUSH));
		::ReleaseDC(wnd, dc);

		m_pMainWnd->ShowWindow(SW_SHOW);
	}

	adjustDestRect();

	updateIFB();
	updateFilter();

	m_pMainWnd->RedrawWindow(NULL, NULL, RDW_INVALIDATE | RDW_ERASE | RDW_ALLCHILDREN);
}

bool VBA::initDisplay(bool resizeWindow)
{
	if (display)
	{
		changingVideoSize = true;
		shutdownDisplay();
		if (input)
		{
			delete input;
			input = NULL;
		}

		// Rebuilding the window makes the whole frame flash on every render
		// method switch, even when no game is running.  Only the frame style
		// differs between windowed and fullscreen, so the window is kept when
		// the switch does not cross that line.  A windowed window carries both
		// WS_POPUP and WS_OVERLAPPEDWINDOW (it is created that way further
		// down), so it is the overlapped bits, not the popup one, that tell
		// the two styles apart.
		CWnd *pWnd   = m_pMainWnd;
		bool  rebuild = pWnd == NULL ||
		                ((pWnd->GetStyle() & WS_OVERLAPPEDWINDOW) == 0) !=
		                     isFullscreenOption(videoOption);

		if (pWnd != NULL)
		{
			pWnd->DragAcceptFiles(FALSE);

			if (rebuild)
			{
				m_pMainWnd = NULL;
				pWnd->DestroyWindow();
				delete pWnd;
			}
		}

		display = NULL;

		// A renderer that presents through a swap chain in the flip model -
		// Direct3D 11 and Direct3D 9Ex both do - hands its frames straight to
		// the desktop window manager instead of drawing into the window, and
		// the last frame it presented keeps covering that window once the
		// chain is released.  GDI and DirectDraw paint into the window, so
		// everything they draw would stay hidden behind that frame and the
		// picture would look frozen from the moment of the switch.  Painting
		// the window black once takes it back out of that mode; the renderer
		// that comes next fills it with the frame a moment later.
		const bool paintsIntoWindow = renderMethod == GDI
#if VBA_ENABLE_DDRAW
		                              || renderMethod == DIRECT_DRAW
#endif
		                              ;

		if (!rebuild && paintsIntoWindow && pWnd != NULL)
		{
			RenderWnd *render = ((MainWnd *)pWnd)->renderWnd();
			CRect      area;

			render->GetClientRect(&area);

			if (render->GetSafeHwnd() != NULL && !area.IsRectEmpty())
			{
				CClientDC dc(render);
				dc.FillSolidRect(&area, RGB(0, 0, 0));
				render->RedrawWindow(NULL, NULL,
				                     RDW_INVALIDATE | RDW_ERASE | RDW_UPDATENOW);
			}
		}
	}

	if (display == NULL)
	{
		// A render method change keeps the window as it is: resizing and
		// repainting it here is what made the whole frame flash.  The first
		// display of the session still has to create it, so that case is
		// covered even when the caller asked for no resize.
		if (resizeWindow || m_pMainWnd == NULL)
			updateWindowSize(videoOption);

		// Every renderer gets a picture window of its own.  What a renderer
		// leaves on the window it drew into - a GL context, the last frame of
		// a flip model swap chain - is what makes the next one either refuse
		// the window or paint behind that frame, and neither can be undone on
		// the window itself.  See MainWnd::recreateRenderWnd().
		if (m_pMainWnd != NULL)
			((MainWnd *)m_pMainWnd)->recreateRenderWnd();

		switch (renderMethod)
		{
#if VBA_ENABLE_GDI
		case GDI:
			display = newGDIDisplay();
			break;
#endif
#if VBA_ENABLE_DDRAW
		case DIRECT_DRAW:
			display = newDirectDrawDisplay();
			break;
#endif
		case DIRECT_3D:
			display = newDirect3DDisplay();
			break;
#if VBA_ENABLE_OPENGL
		case OPENGL:
			display = newOpenGLDisplay();
			break;
		case OPENGL33:
			display = newOpenGL33Display();
			break;
#endif
		case DIRECT_3D11:
			display = newDirect3D11Display();
			break;
#if VBA_ENABLE_WGPU
		case WGPU:
			display = newWgpuDisplay();
			break;
#endif
		}

		// A render method this build does not have leaves nothing to
		// initialize, which is the same as one that does not start: the
		// caller falls back to the renderer below it.
		if (display != NULL && display->initialize())
		{
			if (input == NULL)
			{
				if (!initInput())
				{
					changingVideoSize = false;
					AfxPostQuitMessage(0);
					return false;
				}
			}

			input->checkKeys();

			changingVideoSize = false;

			// A window built during this call was sized while video changes
			// were in progress, so OnSize skipped the layout: the picture
			// area and the status bar would stay at their creation size.  A
			// window that was kept can be stale the same way, since its
			// WM_SIZE arrived while the display it belongs to was being
			// replaced.  Laying out now covers both, and costs nothing when
			// the geometry did not change.
			if (m_pMainWnd != NULL)
				((MainWnd *)m_pMainWnd)->layoutNow();
		}
		else
		{
			if (videoOption == VIDEO_320x240 ||
			    videoOption == VIDEO_640x480 ||
			    videoOption == VIDEO_800x600 ||
			    videoOption == VIDEO_OTHER)
			{
				regSetDwordValue(_T("video"), VIDEO_1X);
				if (pVideoDriverGUID)
					regSetDwordValue(_T("defaultVideoDriver"), TRUE);
			}
			changingVideoSize = false;
			return false;
		}
	}
	changingVideoSize = false;
	return true;
}

bool VBA::updateRenderMethod(bool force)
{
	bool res = true;
	if (force || (display && display->getType() != renderMethod))
	{
		res = initDisplay(false);

		while (!res && renderMethod > 0)
		{
			int failed = renderMethod;

			// The renderer below the one that failed, in the order of the
			// menu.  The OpenGL 3.3 renderer falls back to the fixed
			// function one, which is only built when it is; a build without
			// either of them falls back to the renderer below them in one
			// step.  The same goes for the WebGPU renderer, so that a
			// setting left over from a build that had it does not send the
			// loop around again and again.  GDI and DirectDraw are skipped
			// the same way when their backends are not built, and GDI comes
			// first: it is the one that still starts where Direct3D does not,
			// which is the whole point of the last step of the chain.
			if (renderMethod == OPENGL33)
			{
#if VBA_ENABLE_OPENGL
				renderMethod = OPENGL;
#else
				renderMethod = DIRECT_3D;
#endif
			}
			else if (renderMethod == WGPU)
				renderMethod = DIRECT_3D11;
			else if (renderMethod == DIRECT_3D11)
				renderMethod = DIRECT_3D;
			else if (renderMethod == OPENGL)
				renderMethod = DIRECT_3D;
			else if (renderMethod == DIRECT_3D)
			{
#if VBA_ENABLE_GDI
				renderMethod = GDI;
#elif VBA_ENABLE_DDRAW
				renderMethod = DIRECT_DRAW;
#else
				// Neither GDI nor DirectDraw is built, so there is nothing
				// below Direct3D to try and the chain ends here.
				winlog(_T("VBA: no renderer is left to try\n"));
				break;
#endif
			}
			else if (renderMethod == DIRECT_DRAW)
			{
#if VBA_ENABLE_GDI
				renderMethod = GDI;
#else
				// The GDI renderer is the last one of the chain, and it is
				// not built, so there is nothing below the DirectDraw
				// renderer to try and the chain ends here.
				winlog(_T("VBA: no renderer is left to try\n"));
				break;
#endif
			}

			winlog(_T("VBA: render method %d did not start, trying %d\n"), failed, renderMethod);

			res = initDisplay(false);
		}

		// The upscalers are shaders, and which ones a backend runs is up to it:
		// the ones it does not have are greyed out in the menu, and a greyed
		// out item must not stay selected, or the frame would come out
		// unfiltered while the menu said it was filtered.  Falling back to Off
		// keeps the two in step; the setting is saved below like any other.
		if (res && display != NULL && upscaler != UPSCALER_OFF &&
		    !display->upscalerAvailable(upscaler))
		{
			winlog(_T("VBA: upscaler %d is not available with render method %d, Off is selected\n"),
			       upscaler, renderMethod);

			upscaler = UPSCALER_OFF;
		}
	}

	updateIFB();
	updateFilter();

	// Only the picture changes when the renderer does.  Repainting the main
	// window as well was what made the menu area and the frame flash.
	CWnd *pRedraw = winRenderWnd();
	if (pRedraw != NULL)
		pRedraw->RedrawWindow(NULL, NULL, RDW_INVALIDATE | RDW_ERASE);

	updateStatusBar();

	regSetDwordValue(_T("renderMethod"), renderMethod);

	return res;
}

void VBA::shutdownDisplay()
{
	if (display != NULL)
	{
		display->cleanup();
		delete display;
		display = NULL;
	}
}

void VBA::updatePriority()
{
	switch (threadPriority)
	{
	case 0:
		SetThreadPriority(THREAD_PRIORITY_HIGHEST);
		break;
	case 1:
		SetThreadPriority(THREAD_PRIORITY_ABOVE_NORMAL);
		break;
	case 3:
		SetThreadPriority(THREAD_PRIORITY_BELOW_NORMAL);
		break;
	default:
		SetThreadPriority(THREAD_PRIORITY_NORMAL);
	}
}


// The two ways a language module can fail to load look the same to the
// caller, but only one of them is worth explaining: a missing file is the
// ordinary state of a build without translations, a module that exists but
// was made for another revision of the resources is a mismatch that the user
// can fix by updating the language files.
static void winLanguageLoadFailed(bool versionMismatch, const CString &languageName)
{
	if (versionMismatch)
		systemMessage(IDS_LANGUAGE_MODULE_MISMATCH,
		              _T("The language module was built for a different version of this program and cannot be loaded.\n\n")
		              _T("Update the language files to match this build, or remove the module to use the built-in English."));
	else
		systemMessage(IDS_FAILED_TO_LOAD_LIBRARY,
		              _T("Failed to load library %s"),
		              (LPCTSTR)languageName);
}


void VBA::winSetLanguageOption(int option, bool force)
{
	if (((option == languageOption) && option != 2) && !force)
		return;

	languageVersionMismatch = false;

	switch (option)
	{
	case 0:
	{
		TCHAR     lbuffer[10];
		HINSTANCE l = NULL;

		if (GetLocaleInfo(LOCALE_SYSTEM_DEFAULT, LOCALE_SABBREVLANGNAME,
		                  lbuffer, 10))
		{
			l = winLoadLanguage(lbuffer);
			if (l == NULL)
			{
				LCID locIdBase = MAKELCID(MAKELANGID(PRIMARYLANGID(GetSystemDefaultLangID()), SUBLANG_NEUTRAL), SORT_DEFAULT);
				if (GetLocaleInfo(locIdBase, LOCALE_SABBREVLANGNAME,
				                  lbuffer, 10))
					l = winLoadLanguage(lbuffer);
			}
		}
		else
		{
			systemMessage(IDS_FAILED_TO_GET_LOCINFO,
			              "Failed to get locale information");
			return;
		}

		// No module for the system language is the ordinary state of a build
		// that ships without translations, not an error: fall back to the
		// resources in the executable and say nothing about it.  Complaining
		// here would put a message on screen at every start of every such
		// build on a non English Windows.  A module that exists but was built
		// for another revision is different: it was found and rejected, so
		// say why the UI is about to fall back to English.
		if (l == NULL && languageVersionMismatch)
			winLanguageLoadFailed(true, languageName);
		if (languageModule != NULL)
			/**/ ::FreeLibrary(languageModule);
		languageModule = l;
		AfxSetResourceHandle(l != NULL ? l : AfxGetInstanceHandle());
		break;
	}
	case 1:
		if (languageModule != NULL)
			/**/ ::FreeLibrary(languageModule);
		languageModule = NULL;
		AfxSetResourceHandle(AfxGetInstanceHandle());
		break;
	case 2:
	{
		if (!force)
		{
			LangSelect dlg;
			if (dlg.DoModal())
			{
				HINSTANCE l = winLoadLanguage(languageName);
				if (l == NULL)
				{
					winLanguageLoadFailed(languageVersionMismatch, languageName);
					return;
				}
				AfxSetResourceHandle(l);
				if (languageModule != NULL)
					/**/ ::FreeLibrary(languageModule);
				languageModule = l;
			}
		}
		else
		{
			if (languageName.IsEmpty())
				return;
			HINSTANCE l = winLoadLanguage(languageName);
			if (l == NULL)
			{
				winLanguageLoadFailed(languageVersionMismatch, languageName);
				return;
			}
			AfxSetResourceHandle(l);
			if (languageModule != NULL)
				FreeLibrary(languageModule);
			languageModule = l;
		}
		break;
	}
	}
	languageOption = option;
	updateMenuBar();
	theApp.winAccelMgr.UpdateMenu(theApp.menu);
}

HINSTANCE VBA::winLoadLanguage(LPCTSTR name)
{
	CString buffer;

	buffer.Format(_T("vba_%s.dll"), name);

	HINSTANCE l = /**/ ::LoadLibrary(buffer);

	if (l == NULL)
	{
		// The locale name comes back as three letters, "CHS" for Simplified
		// Chinese; the language module is named after the first two.
		if (_tcslen(name) == 3)
		{
			TCHAR buffer2[3];
			buffer2[0] = name[0];
			buffer2[1] = name[1];
			buffer2[2] = 0;
			buffer.Format(_T("vba_%s.dll"), buffer2);

			l = /**/ ::LoadLibrary(buffer);
		}
	}

	// A module made from another revision of the English resources may be
	// missing strings, controls or whole dialogs this build expects.  Refuse
	// it and let the caller fall back to the executable's English rather than
	// put an incomplete translation on screen.
	if (l != NULL && !winResL10nVersionMatches(l))
	{
		languageVersionMismatch = true;

		const CString actual   = winResLoadL10nVersion(l);
		const CString expected = winResLoadL10nVersion(AfxGetInstanceHandle());

		winlog(_T("Language module %s rejected: resource version %s, expected %s\n"),
		       (LPCTSTR)buffer,
		       actual.IsEmpty() ? _T("(missing)") : (LPCTSTR)actual,
		       expected.IsEmpty() ? _T("(missing)") : (LPCTSTR)expected);

		/**/ ::FreeLibrary(l);
		return NULL;
	}

	return l;
}

bool VBA::initInput()
{
	if (input)
		delete input;
	input = newDirectInput();
	if (input->initialize())
	{
		input->loadSettings();
		input->checkKeys();
		return true;
	}
	delete input;
	return false;
}

void VBA::winAddUpdateListener(IUpdateListener *l)
{
	updateList.AddTail(l);
	updateCount++;
}

void VBA::winRemoveUpdateListener(IUpdateListener *l)
{
	POSITION pos = updateList.Find(l);
	if (pos)
	{
		updateList.RemoveAt(pos);
		updateCount--;
		if (updateCount < 0)
			updateCount = 0;
	}
}

// Whether the render method named is one this build has.  The DirectDraw,
// GDI, OpenGL and WebGPU renderers sit behind a compile time switch, so a
// build without them cannot run them at all, and a setting left over from a
// build that had them has to fall back to one it has.
// noinline: with all four switches on, the ARM64 link time code generator
// crashes (C1001) when this is inlined into loadSettings().
__declspec(noinline) static bool renderMethodBuilt(DISPLAY_TYPE method)
{
	switch (method)
	{
#if VBA_ENABLE_DDRAW
	case DIRECT_DRAW:
		return true;
#endif
#if VBA_ENABLE_GDI
	case GDI:
		return true;
#endif
	case DIRECT_3D:
	case DIRECT_3D11:
		return true;
#if VBA_ENABLE_OPENGL
	case OPENGL:
	case OPENGL33:
		return true;
#endif
#if VBA_ENABLE_WGPU
	case WGPU:
		return true;
#endif
	default:
		return false;
	}
}

// The settings file keeps the game font as a family and a size under it
// ("gse" and "7x11").  A system face has no such name for its sizes, so its
// entry is the character height CreateFont is given, possibly still written
// as "9x20" by hand, in which case the number after the x is the height.
static int parseTextSize(const CString &text)
{
	const int x = text.Find(_T('x'));
	const int value = _ttoi(x >= 0 ? text.Mid(x + 1) : text);
	return value > 0 ? value : 0;
}

void VBA::loadSettings()
{
	CString	buffer;

	// What a build with no stored settings starts on.  A modern build starts
	// on Direct3D 11, XAudio2, a 2x window and 44 kHz audio; a build that
	// targets Windows XP keeps the older defaults, since neither Direct3D 11
	// nor XAudio2 exists there.
	const DISPLAY_TYPE defaultMethod = defaultRenderMethod();
#if defined(WINXP)
	const DWORD defaultVideoOption = VIDEO_1X;
	const DWORD defaultSoundApi = SOUND_API_DIRECTSOUND;
	const DWORD defaultSoundQuality = 2;
#else
	const DWORD defaultVideoOption = VIDEO_2X;
	const DWORD defaultSoundApi = SOUND_API_XAUDIO2;
	const DWORD defaultSoundQuality = 1;
#endif

	// video
	bool defaultVideoDriver = regQueryDwordValue(_T("defaultVideoDriver"), true) ? true : false;
	if (!regQueryBinaryValue(_T("videoDriverGUID"), (char *)&videoDriverGUID, sizeof(GUID)))
	{
		defaultVideoDriver = TRUE;
	}
	if (defaultVideoDriver)
		pVideoDriverGUID = NULL;
	else
		pVideoDriverGUID = &videoDriverGUID;

	videoOption = regQueryDwordValue(_T("video"), defaultVideoOption);
	if (videoOption < 0 || videoOption > VIDEO_12X)
		videoOption = defaultVideoOption;
	switch (videoOption)
	{
	case VIDEO_320x240:
		fsWidth		 = 320;
		fsHeight	 = 240;
		fsColorDepth = 16;
		break;
	case VIDEO_640x480:
		fsWidth		 = 640;
		fsHeight	 = 480;
		fsColorDepth = 16;
		break;
	case VIDEO_800x600:
		fsWidth		 = 800;
		fsHeight	 = 600;
		fsColorDepth = 16;
		break;
	}
	if (videoOption == VIDEO_OTHER)
	{
		if (fsWidth < 0 || fsWidth > 4095 || fsHeight < 0 || fsHeight > 4095)
			videoOption = defaultVideoOption;
		if (fsColorDepth != 16 && fsColorDepth != 24 && fsColorDepth != 32)
			videoOption = defaultVideoOption;
	}

	fsWidth = regQueryDwordValue(_T("fsWidth"), 0);
	fsHeight = regQueryDwordValue(_T("fsHeight"), 0);
	fsColorDepth	  = regQueryDwordValue(_T("fsColorDepth"), 0);
	fsMaxScale		  = regQueryDwordValue(_T("fsMaxScale"), 0);

	// The three scaling modes replaced the full screen "ignore ratio"
	// switch, which was a boolean and only applied to full screen.  A
	// settings file written by an older build still has that key, and its
	// two values are the two ends of the new setting; without either the
	// picture keeps its aspect ratio.
	const DWORD stretch = regQueryDwordValue(_T("stretch"), 0xFFFFFFFF);
	screenScaling = (stretch == 0xFFFFFFFF || stretch == 0) ?
	                    SCREEN_SCALING_ASPECT : SCREEN_SCALING_STRETCH;
	screenScaling = regQueryDwordValue(_T("screenScaling"), screenScaling);
	if (screenScaling < SCREEN_SCALING_STRETCH || screenScaling > SCREEN_SCALING_LAST)
		screenScaling = SCREEN_SCALING_ASPECT;

	renderMethod = (DISPLAY_TYPE)regQueryDwordValue(_T("renderMethod"), defaultMethod);
	if (renderMethod < GDI || renderMethod > WGPU)
		renderMethod = defaultMethod;

	// A render method this build does not have cannot be used, so a setting
	// left over from a build that had it falls back to the default one.
	if (!renderMethodBuilt(renderMethod))
	{
		winlog(_T("VBA: render method %d is not in this build, %d is used instead\n"),
		       renderMethod, defaultMethod);

		renderMethod = defaultMethod;
	}

	ddrawEmulationOnly	= regQueryDwordValue(_T("ddrawEmulationOnly"), false) ? true : false;
	ddrawUseVideoMemory = regQueryDwordValue(_T("ddrawUseVideoMemory"), false) ? true : false;
	vsync = regQueryDwordValue(_T("vsync"), false) ? true : false;

	d3dFilter = regQueryDwordValue(_T("d3dFilter"), 0);
	if (d3dFilter < 0 || d3dFilter > 2)
		d3dFilter = 0;
	upscaler = regQueryDwordValue(_T("upscaler"), UPSCALER_OFF);
	// Against the end of the list, or every shader added after xBRZ would be
	// read back as Off on the next start.
	if (upscaler < UPSCALER_OFF || upscaler > UPSCALER_LAST)
		upscaler = UPSCALER_OFF;
	upscalerSharpness = regQueryDwordValue(_T("upscalerSharpness"), 50);
	if (upscalerSharpness < 0 || upscalerSharpness > 100)
		upscalerSharpness = 50;
	upscalerMaxScale = regQueryDwordValue(_T("upscalerMaxScale"), 0);
	if (upscalerMaxScale < 0 || upscalerMaxScale > 8)
		upscalerMaxScale = 0;
	glFilter = regQueryDwordValue(_T("glFilter"), 0);
	if (glFilter < 0 || glFilter > 2)
		glFilter = 0;
	glType = regQueryDwordValue(_T("glType"), 0);
	if (glType < 0 || glType > 1)
		glType = 0;
#if VBA_ENABLE_WGPU
	wgpuFilter = regQueryDwordValue(_T("wgpuFilter"), 0);
	if (wgpuFilter < 0 || wgpuFilter > 2)
		wgpuFilter = 0;
	wgpuBackend = regQueryDwordValue(_T("wgpuBackend"), VBA_WGPU_BACKEND_AUTO);
	if (wgpuBackend < 0 || wgpuBackend >= VBA_WGPU_BACKEND_COUNT)
		wgpuBackend = VBA_WGPU_BACKEND_AUTO;
#endif

	// pixel filter & ifb
	filterType = regQueryDwordValue(_T("filter"), 0);
	if (filterType < 0 || filterType > 26)
		filterType = 0;
	lcdFilterGba = (int)regQueryDwordValue(_T("lcdFilterGba"), (DWORD)LCD_FILTER_OFF);
	if (lcdFilterGba < LCD_FILTER_OFF || lcdFilterGba >= kLcdGbaVariantCount)
		lcdFilterGba = LCD_FILTER_OFF;
	lcdFilterGbc = (int)regQueryDwordValue(_T("lcdFilterGbc"), (DWORD)LCD_FILTER_OFF);
	if (lcdFilterGbc < LCD_FILTER_OFF || lcdFilterGbc >= kLcdGbcVariantCount)
		lcdFilterGbc = LCD_FILTER_OFF;
	lcdFilterColorMode = (int)regQueryDwordValue(_T("lcdFilterColorMode"), 0);
	if (lcdFilterColorMode < 0 || lcdFilterColorMode > 2)
		lcdFilterColorMode = 0;
	ifbType	   = regQueryDwordValue(_T("ifbType"), 0);
	if (ifbType < 0 || ifbType > 2)
		ifbType = 0;

	// frame skipping
	frameSkip = regQueryDwordValue(_T("frameSkip"), /*2*/ 0);
	if (frameSkip < 0 || frameSkip > 9)
		frameSkip = 1;
	gbFrameSkip = regQueryDwordValue(_T("gbFrameSkip"), 0);
	if (gbFrameSkip < 0 || gbFrameSkip > 9)
		gbFrameSkip = 0;
///  autoFrameSkip = regQueryDwordValue("autoFrameSkip", FALSE) ? TRUE : FALSE;

	// input
	joypadDefault = regQueryDwordValue(_T("joypadDefault"), 0);
	if (joypadDefault < 0 || joypadDefault > 3)
		joypadDefault = 0;
	allowLeftRight		   = regQueryDwordValue(_T("allowLeftRight"), false) ? true : false;
	autofireAccountForLag  = regQueryDwordValue(_T("autofireAccountForLag"), false) ? true : false;
	nextframeAccountForLag = regQueryDwordValue(_T("nextframeAccountForLag"), false) ? true : false;
	theApp.AsscWithSaveState = regQueryDwordValue(_T("AsscWithSaveState"), false) ? true : false;

	// speed
	throttle = regQueryDwordValue(_T("throttle"), 0);
	if (throttle < 5 || throttle > 1000)
		throttle = 100;

	synchronize = regQueryDwordValue(_T("synchronize"), 1) ? true : false;
	accuratePitchThrottle = regQueryDwordValue(_T("accuratePitchThrottle"), FALSE) ? TRUE : FALSE;

	// sound
	int resChannels = regQueryDwordValue(_T("soundEnable"), 0x30f);
	systemSoundEnableChannels(resChannels);
	systemSoundDisableChannels(~resChannels);
	soundOffFlag = (regQueryDwordValue(_T("soundOff"), 0)) ? true : false;
	soundQuality = regQueryDwordValue(_T("soundQuality"), defaultSoundQuality);
	soundApi	 = regQueryDwordValue(_T("soundApi"), defaultSoundApi);
	if (soundApi != SOUND_API_XAUDIO2)
		soundApi = SOUND_API_DIRECTSOUND;
	soundEcho	 = regQueryDwordValue(_T("soundEcho"), 0) ? true : false;
	soundLowPass = regQueryDwordValue(_T("soundLowPass"), 0) ? true : false;
	soundReverse = regQueryDwordValue(_T("soundReverse"), 0) ? true : false;
	soundVolume	 = regQueryDwordValue(_T("soundVolume"), 0);
	if (soundVolume < 0 || soundVolume > 5)
		soundVolume = 0;
	muteFrameAdvance = regQueryDwordValue(_T("muteFrameAdvance"), 0) ? TRUE : FALSE;
	muteWhenInactive = regQueryDwordValue(_T("muteWhenInactive"), 0) ? TRUE : FALSE;

	// emulation
#ifdef USE_GBA_CORE_V7
	memLagEnabled	  = regQueryDwordValue(_T("memLagEnabled"), false) ? true : false;
	memLagTempEnabled = memLagEnabled;
#endif
#ifdef USE_GB_CORE_V7
	gbNullInputHackEnabled	   = regQueryDwordValue(_T("gbNullInputHackEnabled"), false) ? true : false;
	gbNullInputHackTempEnabled = gbNullInputHackEnabled;
#else
	gbV20GBFrameTimingHack	   = regQueryDwordValue(_T("gbV20GBFrameTimingHack"), false) ? true : false;
	gbV20GBFrameTimingHackTemp = gbV20GBFrameTimingHack;
#endif
	useOldSync		  = regQueryDwordValue(_T("useOldSync"), 0) ? TRUE : FALSE;
	useOldFrameTiming = regQueryDwordValue(_T("useOldGBTiming"), false) ? true : false;

	useBiosFile	  = regQueryDwordValue(_T("useBios"), 0) ? true : false;
	skipBiosIntro = regQueryDwordValue(_T("skipBios"), 0) ? true : false;
	buffer		  = regQueryStringValue(_T("biosFile"), _T(""));
	if (!buffer.IsEmpty())
	{
		biosFileName = buffer;
	}
//	removeIntros = regQueryDwordValue("removeIntros", false) ? true : false;

	autoIPS = regQueryDwordValue(_T("autoIPS"), true) ? true : false;

	winRtcEnable = regQueryDwordValue(_T("rtcEnabled"), 0) ? true : false;
	rtcEnable(winRtcEnable);

	winSaveType = regQueryDwordValue(_T("saveType"), 0);
	if (winSaveType < 0 || winSaveType > 5)
		winSaveType = 0;
	cpuEnhancedDetection = regQueryDwordValue(_T("enhancedDetection"), 1) ? true : false;
	winFlashSize		 = regQueryDwordValue(_T("flashSize"), 0x10000);
	if (winFlashSize != 0x10000 && winFlashSize != 0x20000)
		winFlashSize = 0x10000;

	cpuDisableSfx = regQueryDwordValue(_T("disableSfx"), 0) ? true : false;

	// GBx
	winGbPrinterEnabled = regQueryDwordValue(_T("gbPrinter"), false) ? true : false;
	if (winGbPrinterEnabled)
		gbSerialFunction = gbPrinterSend;
	else
		gbSerialFunction = NULL;
	gbEmulatorType = regQueryDwordValue(_T("emulatorType"), 0);
	if (gbEmulatorType < 0 || gbEmulatorType > 5)
		gbEmulatorType = 1;
	winGbBorderOn	  = regQueryDwordValue(_T("borderOn"), 0);
	gbBorderAutomatic = regQueryDwordValue(_T("borderAutomatic"), 0);

	gbColorOption	= regQueryDwordValue(_T("colorOption"), 0);
	gbPaletteOption = regQueryDwordValue(_T("gbPaletteOption"), 0);
	if (gbPaletteOption < 0)
		gbPaletteOption = 0;
	if (gbPaletteOption > 2)
		gbPaletteOption = 2;
	regQueryBinaryValue(_T("gbPalette"), (char *)systemGbPalette, 24 * sizeof(u16));

	// head-up display
	showSpeed = regQueryDwordValue(_T("showSpeed"), 1);
	if (showSpeed < 0 || showSpeed > 2)
		showSpeed = 1;
	showSpeedTransparent = regQueryDwordValue(_T("showSpeedTransparent"), TRUE) ? TRUE : FALSE;
	outlinedText		 = regQueryDwordValue(_T("outlinedText"), TRUE) != 0;
	transparentText		 = regQueryDwordValue(_T("transparentText"), FALSE) != 0;
	textColor			 = regQueryDwordValue(_T("textColor"), 0);
	textMethod			 = regQueryDwordValue(_T("textMethod"), 1);
	screenTextFont		 = regQueryStringValue(_T("screenTextFont"), _T("Consolas"));
	screenTextSize		 = regQueryDwordValue(_T("screenTextSize"), 12);
	textMarginLeft		 = (int)regQueryDwordValue(_T("textMarginLeft"), 0);
	textMarginBottom	 = (int)regQueryDwordValue(_T("textMarginBottom"), 0);

	// The game font is stored the way the text options dialog shows it: a
	// family and one of the sizes under it ("gse" and "7x11").  A bundled
	// family has to be turned back into the font that draws it; a system face
	// has only its pixel height to remember.
	{
		const CString family = regQueryStringValue(_T("gameTextFont"), GAME_TEXT_FONT_BUILTIN);
		const CString size   = regQueryStringValue(_T("gameTextSize"), GAME_TEXT_FONT_BUILTIN_SIZE);

		gameTextFont = GAME_TEXT_FONT_BUILTIN;
		gameTextSize = 8;

		if (!family.IsEmpty() && family.Compare(GAME_TEXT_FONT_BUILTIN) != 0)
		{
			CString concrete;
			const TextFont *bundled = winBitmapFontBySize(family, size, &concrete);

			if (bundled != NULL)
			{
				gameTextFont = concrete;
				gameTextSize = bundled->height;
			}
			else
			{
				gameTextFont = family;
				gameTextSize = parseTextSize(size);
			}
		}
	}

	// Older builds allowed signed margins; the dialog now accepts 0..9999.
	if (textMarginLeft < TEXT_MARGIN_MIN)
		textMarginLeft = TEXT_MARGIN_MIN;
	else if (textMarginLeft > TEXT_MARGIN_MAX)
		textMarginLeft = TEXT_MARGIN_MAX;
	if (textMarginBottom < TEXT_MARGIN_MIN)
		textMarginBottom = TEXT_MARGIN_MIN;
	else if (textMarginBottom > TEXT_MARGIN_MAX)
		textMarginBottom = TEXT_MARGIN_MAX;

	// The On Screen font is a TrueType face.  A value left by an earlier
	// build that still offered the built-in font there means Consolas now.
	if (screenTextFont.IsEmpty() ||
	    screenTextFont.Compare(GAME_TEXT_FONT_BUILTIN) == 0)
		screenTextFont = _T("Consolas");

	if (screenTextSize < SCREEN_TEXT_SIZE_MIN)
		screenTextSize = 12;
	if (screenTextSize > SCREEN_TEXT_SIZE_MAX)
		screenTextSize = SCREEN_TEXT_SIZE_MAX;
	if (gameTextSize < 1)
		gameTextSize = 8;
	if (gameTextSize > 128)
		gameTextSize = 128;

	// The built-in font has no other size than its native 8 pixels.
	if (gameTextFontIsBuiltin())
		gameTextSize = 8;
	frameCounter		 = regQueryDwordValue(_T("frameCounter"), false) ? true : false;
	lagCounter			 = regQueryDwordValue(_T("lagCounter"), false) ? true : false;
	extraCounter		 = regQueryDwordValue(_T("extraCounter"), false) ? true : false;
	inputDisplay		 = regQueryDwordValue(_T("inputDisplay"), false) ? true : false;
	nextInputDisplay	 = regQueryDwordValue(_T("nextInputDisplay"), false) ? true : false;
	disableStatusMessage = regQueryDwordValue(_T("disableStatus"), 0) ? true : false;
	statusBarHud			 = regQueryDwordValue(_T("statusBarHud"), 0) ? true : false;

	// UI
	windowPositionX = regQueryDwordValue(_T("windowX"), -32000);
	windowPositionY = regQueryDwordValue(_T("windowY"), -32000);

	autoHideMenu = regQueryDwordValue(_T("autoHideMenu"), 0) ? true : false;

	// Follow the system language by default: a build that ships a language
	// module then comes up in the user's language on its own, and a build
	// without one stays on the resources in the executable.
	languageOption = regQueryDwordValue(_T("language"), 0);
	if (languageOption < 0 || languageOption > 2)
		languageOption = 0;
	buffer = regQueryStringValue(_T("languageName"), _T(""));
	if (!buffer.IsEmpty())
	{
		languageName = buffer.Left(3);
	}
	else
		languageName = "";
	winSetLanguageOption(languageOption, true);

	// preferences
	alwaysOnTop = regQueryDwordValue(_T("alwaysOnTop"), false) ? true : false;
	// The modern UI font is the default; the menu entry exists so that the
	// 2004 era layout can be brought back if a dialog does not fit.
	useModernFont = regQueryDwordValue(_T("useModernFont"), 1) ? true : false;
	uiFont::setModern(useModernFont);
	pauseWhenInactive	  = regQueryDwordValue(_T("pauseWhenInactive"), 1) ? true : false;
	enableBackgroundInput = regQueryDwordValue(_T("enableBackgroundInput"), 0) ? true : false;
	threadPriority		  = regQueryDwordValue(_T("priority"), 2);
	if (threadPriority < 0 || threadPriority > 3)
		threadPriority = 2;
	updatePriority();

	filenamePreference = regQueryDwordValue(_T("filenamePreference"), 0);
	altAviRecordMethod = regQueryDwordValue(_T("altAviRecordMethod"), false) ? true : false;
	captureScaled = regQueryDwordValue(_T("captureScaled"), regQueryDwordValue(_T("aviRecordScaled"), false)) ? true : false;

	// What the recording dialog remembers.  An absent value means "never
	// chosen", so a first run starts on H.264 and AAC; the literal "none"
	// means the user switched that codec off.
	{
		recordParams.videoSubtype  = videoEncoders::defaultVideoCodec();
		recordParams.audioSubtype  = videoEncoders::defaultAudioCodec();
		recordParams.videoHardware = true;
		recordParams.videoBitrate	 = VIDEO_BITRATE_DEFAULT;
		recordParams.audioBitrate	 = AUDIO_BITRATE_DEFAULT;
		recordParams.profile		 = VIDEO_PROFILE_AUTO;
		recordParams.frameRate		 = FRAMERATE_DEFAULT;
		recordParams.async			 = true;

		const CString videoText = regQueryStringValue(_T("recordVideoCodec"), _T(""));
		if (!videoText.IsEmpty())
		{
			GUID video = { 0 };
			recordParams.videoSubtype = videoEncoders::guidFromString(
			                                std::wstring(videoText.GetString()), &video)
			                                ? video : GUID();
		}

		const CString audioText = regQueryStringValue(_T("recordAudioCodec"), _T(""));
		if (!audioText.IsEmpty())
		{
			GUID audio = { 0 };
			recordParams.audioSubtype = videoEncoders::guidFromString(
			                                std::wstring(audioText.GetString()), &audio)
			                                ? audio : GUID();
		}

		recordParams.videoHardware =
		    regQueryDwordValue(_T("recordVideoHardware"), 1) ? true : false;

		int videoBitrate = (int)regQueryDwordValue(_T("recordVideoBitrate"), VIDEO_BITRATE_DEFAULT);
		if (videoBitrate < VIDEO_BITRATE_MIN || videoBitrate > VIDEO_BITRATE_MAX)
			videoBitrate = VIDEO_BITRATE_DEFAULT;
		recordParams.videoBitrate = videoBitrate;

		int audioBitrate = (int)regQueryDwordValue(_T("recordAudioBitrate"), AUDIO_BITRATE_DEFAULT);
		if (audioBitrate < AUDIO_BITRATE_MIN || audioBitrate > AUDIO_BITRATE_MAX)
			audioBitrate = AUDIO_BITRATE_DEFAULT;
		recordParams.audioBitrate = audioBitrate;

		int profile = (int)regQueryDwordValue(_T("recordProfile"), VIDEO_PROFILE_AUTO);
		if (profile != H264_PROFILE_BASELINE && profile != H264_PROFILE_MAIN &&
		    profile != H264_PROFILE_HIGH)
			profile = VIDEO_PROFILE_AUTO;
		recordParams.profile = profile;

		// Only the rates the dialog offers, so a stale value cannot pick
		// something the box cannot show.
		int frameRate = (int)regQueryDwordValue(_T("recordFrameRate"), FRAMERATE_DEFAULT);
		if (frameRate != 0 && frameRate != 60 && frameRate != 30 && frameRate != 20 && frameRate != 15)
			frameRate = FRAMERATE_DEFAULT;
		recordParams.frameRate = frameRate;

		recordParams.async = regQueryDwordValue(_T("recordAsync"), 1) ? true : false;
	}
	imageCaptureSelect(winAnsiFromWide(regQueryStringValue(_T("captureFormat"), _T(".png"))));

	rewindTimer = regQueryDwordValue(_T("rewindTimer"), 0);
	rewindSlots = regQueryDwordValue(_T("rewindSlots"), 64);
	if (rewindTimer < 0 || rewindTimer > 600)
		rewindTimer = 0;
	if (rewindSlots <= 0)
		rewindTimer = rewindSlots = 0;
	if (rewindSlots > MAX_REWIND_SLOTS)
		rewindSlots = MAX_REWIND_SLOTS;
	if (rewindTimer != 0)
	{
		if (rewindMemory == NULL)
			rewindMemory = (char *)malloc(rewindSlots * REWIND_SIZE);
	}

	if (frameSearchMemory == NULL)
		frameSearchMemory = (char *)malloc(3 * REWIND_SIZE);

	recentFreeze = regQueryDwordValue(_T("recentFreeze"), false) ? true : false;
	for (int i = 0, j = 0; i < 10; ++i)
	{
		buffer.Format(_T("recent%d"), i);
		const CString s = regQueryStringValue(buffer, NULL);
		if (s.IsEmpty())
			continue;
		recentFiles[j] = s;
		++j;
	}

	autoLoadMostRecent = regQueryDwordValue(_T("autoLoadMostRecent"), false) ? true : false;
	loadMakesRecent	   = regQueryDwordValue(_T("loadMakesRecent"), false) ? true : false;
	loadMakesCurrent   = regQueryDwordValue(_T("loadMakesCurrent"), false) ? true : false;
	saveMakesCurrent   = regQueryDwordValue(_T("saveMakesCurrent"), false) ? true : false;
	currentSlot		   = regQueryDwordValue(_T("currentSlot"), 0);
	showSlotTime	   = regQueryDwordValue(_T("showSlotTime"), 0) ? true : false;

	cheatsEnabled = regQueryDwordValue(_T("cheatsEnabled"), true) ? true : false;
	autoSaveLoadCheatList  = regQueryDwordValue(_T("autoSaveCheatList"), 0) ? true : false;
	pauseDuringCheatSearch = regQueryDwordValue(_T("pauseDuringCheatSearch2"), 0) ? true : false;

	int movieEditMode  = regQueryDwordValue(_T("movieEditMode"), 0);
	if (movieEditMode < MovieEditMode::MOVIE_EDIT_MODE_DISCARD || movieEditMode >= MovieEditMode::MOVIE_EDIT_MODE_COUNT)
		movieEditMode = MovieEditMode::MOVIE_EDIT_MODE_DISCARD;
	VBAMovieSetEditMode(MovieEditMode(movieEditMode));

	movieOnEndBehavior = regQueryDwordValue(_T("movieOnEndBehavior"), 0);
	movieOnEndPause	   = regQueryDwordValue(_T("movieOnEndPause"), 0) ? true : false;

	extern bool autoConvertMovieWhenPlaying;    // from movie.cpp
	autoConvertMovieWhenPlaying = regQueryDwordValue(_T("autoConvertMovieWhenPlaying"), 0) ? true : false;

	// RamWatch Settings
	AutoRWLoad		= regQueryDwordValue(AUTORWLOAD, false);
	RWSaveWindowPos = regQueryDwordValue(RWSAVEPOS, false);
	ramw_x = regQueryDwordValue(RAMWX, 0);
	ramw_y = regQueryDwordValue(RAMWY, 0);

	// this is FILO
	for (int i = MAX_RECENT_WATCHES; i > 0; --i)
	{
		buffer.Format(_T("recentWatch%d"), i);
		const CString s = regQueryStringValue(buffer, NULL);
		if (s.IsEmpty())
			continue;
		RWAddRecentFile(winAnsiFromWide(s));
	}
}

void VBA::saveSettings()
{
	regSetDwordValue(_T("defaultVideoDriver"), pVideoDriverGUID == NULL);
	if (pVideoDriverGUID)
	{
		regSetBinaryValue(_T("videoDriverGUID"), (char *)&videoDriverGUID,
		                  sizeof(GUID));
	}
	regSetDwordValue(_T("video"), videoOption);

	regSetDwordValue(_T("fsWidth"), fsWidth);
	regSetDwordValue(_T("fsHeight"), fsHeight);
	regSetDwordValue(_T("fsColorDepth"), fsColorDepth);
	regSetDwordValue(_T("fsMaxScale"), fsMaxScale);

	regSetDwordValue(_T("screenScaling"), screenScaling);

	regSetDwordValue(_T("renderMethod"), renderMethod);

	regSetDwordValue(_T("ddrawEmulationOnly"), ddrawEmulationOnly);
	regSetDwordValue(_T("ddrawUseVideoMemory"), ddrawUseVideoMemory);
	regSetDwordValue(_T("vsync"), vsync);

	regSetDwordValue(_T("d3dFilter"), d3dFilter);
	regSetDwordValue(_T("upscaler"), upscaler);
	regSetDwordValue(_T("upscalerSharpness"), upscalerSharpness);
	regSetDwordValue(_T("upscalerMaxScale"), upscalerMaxScale);
	regSetDwordValue(_T("glFilter"), glFilter);
	regSetDwordValue(_T("glType"), glType);

	// pixel filter & ifb
	regSetDwordValue(_T("filter"), filterType);
	regSetDwordValue(_T("lcdFilterGba"), lcdFilterGba);
	regSetDwordValue(_T("lcdFilterGbc"), lcdFilterGbc);
	regSetDwordValue(_T("lcdFilterColorMode"), lcdFilterColorMode);
	regSetDwordValue(_T("ifbType"), ifbType);

	// frame skipping
	regSetDwordValue(_T("frameSkip"), frameSkip);
	regSetDwordValue(_T("gbFrameSkip"), gbFrameSkip);
///  regSetDwordValue("autoFrameSkip", autoFrameSkip);

	// input
	regSetDwordValue(_T("joypadDefault"), joypadDefault);
	regSetDwordValue(_T("allowLeftRight"), allowLeftRight);
	regSetDwordValue(_T("autofireAccountforLag"), autofireAccountForLag);
	regSetDwordValue(_T("nextframeAccountforLag"), nextframeAccountForLag);
	regSetDwordValue(_T("AsscWithSaveState"), theApp.AsscWithSaveState);

	// speed
	regSetDwordValue(_T("throttle"), throttle);
	regSetDwordValue(_T("synchronize"), synchronize);
	regSetDwordValue(_T("accuratePitchThrottle"), accuratePitchThrottle);

	// sound
	regSetDwordValue(_T("soundEnable"), systemSoundGetEnabledChannels() & 0x030f);
	regSetDwordValue(_T("soundOff"), soundOffFlag);
	regSetDwordValue(_T("soundQuality"), soundQuality);
	regSetDwordValue(_T("soundApi"), soundApi);
	regSetDwordValue(_T("soundEcho"), soundEcho);
	regSetDwordValue(_T("soundLowPass"), soundLowPass);
	regSetDwordValue(_T("soundReverse"), soundReverse);
	regSetDwordValue(_T("soundVolume"), soundVolume);
	regSetDwordValue(_T("muteFrameAdvance"), muteFrameAdvance);
	regSetDwordValue(_T("muteWhenInactive"), muteWhenInactive);

	// emulation
	regSetDwordValue(_T("useBios"), useBiosFile);
	regSetDwordValue(_T("skipBios"), skipBiosIntro);
	if (!biosFileName.IsEmpty())
		regSetStringValue(_T("biosFile"), biosFileName);
//	regSetDwordValue("removeIntros", removeIntros);

	regSetDwordValue(_T("autoIPS"), autoIPS);

#ifdef USE_GBA_CORE_V7
	regSetDwordValue(_T("memLagEnabled"), memLagEnabled);
#endif
#ifdef USE_GB_CORE_V7
	regSetDwordValue(_T("gbNullInputHackEnabled"), gbNullInputHackEnabled);
#else
	regSetDwordValue(_T("gbV20GBFrameTimingHack"), gbV20GBFrameTimingHack);
#endif
	regSetDwordValue(_T("useOldGBTiming"), useOldFrameTiming);
	regSetDwordValue(_T("useOldSync"), useOldSync);

	regSetDwordValue(_T("rtcEnabled"), winRtcEnable);

	regSetDwordValue(_T("saveType"), winSaveType);
	regSetDwordValue(_T("enhancedDetection"), cpuEnhancedDetection);
	regSetDwordValue(_T("flashSize"), winFlashSize);

	regSetDwordValue(_T("disableSfx"), cpuDisableSfx);

	// GBx
	regSetDwordValue(_T("emulatorType"), gbEmulatorType);
	regSetDwordValue(_T("gbPrinter"), winGbPrinterEnabled);
	regSetDwordValue(_T("borderOn"), winGbBorderOn);
	regSetDwordValue(_T("borderAutomatic"), gbBorderAutomatic);

	regSetDwordValue(_T("colorOption"), gbColorOption);
	regSetDwordValue(_T("gbPaletteOption"), gbPaletteOption);
	regSetBinaryValue(_T("gbPalette"), (char *)systemGbPalette, 24 * sizeof(u16));

	// head-up display
	regSetDwordValue(_T("showSpeed"), showSpeed);
	regSetDwordValue(_T("showSpeedTransparent"), showSpeedTransparent);

	regSetDwordValue(_T("outlinedText"), outlinedText);
	regSetDwordValue(_T("transparentText"), transparentText);
	regSetDwordValue(_T("textColor"), textColor);
	regSetDwordValue(_T("textMethod"), textMethod);
	regSetStringValue(_T("screenTextFont"), screenTextFont);
	regSetDwordValue(_T("screenTextSize"), screenTextSize);

	// The game font goes out as the family and the size the dialog shows, not
	// as the one name the renderer knows, so that "gse" and "7x11" are what a
	// person finds in the settings file.  A system face has no sizes to name,
	// so its character height is written on its own.
	{
		CString family, size;

		if (gameTextFontIsBuiltin())
		{
			family = GAME_TEXT_FONT_BUILTIN;
			size   = GAME_TEXT_FONT_BUILTIN_SIZE;
		}
		else if (!winBitmapFontSplit(gameTextFont, &family, &size))
		{
			family = gameTextFont;
			size.Format(_T("%d"), gameTextSize);
		}

		regSetStringValue(_T("gameTextFont"), family);
		regSetStringValue(_T("gameTextSize"), size);
	}
	regSetDwordValue(_T("textMarginLeft"), (DWORD)textMarginLeft);
	regSetDwordValue(_T("textMarginBottom"), (DWORD)textMarginBottom);
	regSetDwordValue(_T("frameCounter"), frameCounter);
	regSetDwordValue(_T("lagCounter"), lagCounter);
	regSetDwordValue(_T("extraCounter"), extraCounter);
	regSetDwordValue(_T("inputDisplay"), inputDisplay);
	regSetDwordValue(_T("nextInputDisplay"), nextInputDisplay);
	regSetDwordValue(_T("disableStatus"), disableStatusMessage);
	regSetDwordValue(_T("statusBarHud"), statusBarHud);

	// UI
	regSetDwordValue(_T("windowX"), windowPositionX);
	regSetDwordValue(_T("windowY"), windowPositionY);

	regSetDwordValue(_T("autoHideMenu"), autoHideMenu);

	regSetDwordValue(_T("language"), languageOption);
	regSetStringValue(_T("languageName"), languageName);

	// preferences
	regSetDwordValue(_T("alwaysOnTop"), alwaysOnTop);
	regSetDwordValue(_T("useModernFont"), useModernFont);
	regSetDwordValue(_T("pauseWhenInactive"), pauseWhenInactive);
	regSetDwordValue(_T("enableBackgroundInput"), enableBackgroundInput);
	regSetDwordValue(_T("priority"), threadPriority);

	regSetDwordValue(_T("filenamePreference"), filenamePreference);
	regSetDwordValue(_T("altAviRecordMethod"), altAviRecordMethod);
	regSetDwordValue(_T("captureScaled"), captureScaled);
	regSetStringValue(_T("captureFormat"), winWideFromAnsi(imageCaptureExtension()));

	// "none" rather than an empty string: regSetStringValue() writes an empty
	// value as "not present", which reads back as the default, not as "off".
	regSetStringValue(_T("recordVideoCodec"),
	                  videoEncoders::isNullGuid(recordParams.videoSubtype)
	                      ? _T("none")
	                      : CString(videoEncoders::guidToString(recordParams.videoSubtype).c_str()));
	regSetDwordValue(_T("recordVideoHardware"), recordParams.videoHardware ? 1 : 0);
	regSetStringValue(_T("recordAudioCodec"),
	                  videoEncoders::isNullGuid(recordParams.audioSubtype)
	                      ? _T("none")
	                      : CString(videoEncoders::guidToString(recordParams.audioSubtype).c_str()));
	regSetDwordValue(_T("recordVideoBitrate"), recordParams.videoBitrate);
	regSetDwordValue(_T("recordAudioBitrate"), recordParams.audioBitrate);
	regSetDwordValue(_T("recordProfile"), recordParams.profile);
	regSetDwordValue(_T("recordFrameRate"), recordParams.frameRate);
	regSetDwordValue(_T("recordAsync"), recordParams.async ? 1 : 0);

	regSetDwordValue(_T("cheatsEnabled"), cheatsEnabled);
	regSetDwordValue(_T("autoSaveCheatList"), autoSaveLoadCheatList);
	regSetDwordValue(_T("pauseDuringCheatSearch2"), pauseDuringCheatSearch);

	regSetDwordValue(_T("rewindTimer"), rewindTimer);
	regSetDwordValue(_T("rewindSlots"), rewindSlots);

	regSetDwordValue(_T("recentFreeze"), recentFreeze);
	CString buffer;
	for (int i = 0; i < 10; i++)
	{
		buffer.Format(_T("recent%d"), i);
		regSetStringValue(buffer, recentFiles[i]);
	}

	regSetDwordValue(_T("autoLoadMostRecent"), autoLoadMostRecent);
	regSetDwordValue(_T("loadMakesRecent"), loadMakesRecent);
	regSetDwordValue(_T("loadMakesCurrent"), loadMakesCurrent);
	regSetDwordValue(_T("saveMakesCurrent"), saveMakesCurrent);
	regSetDwordValue(_T("currentSlot"), currentSlot);
	regSetDwordValue(_T("showSlotTime"), showSlotTime);

	regSetDwordValue(_T("movieEditMode"), VBAMovieGetEditMode());
	regSetDwordValue(_T("movieOnEndBehavior"), movieOnEndBehavior);
	regSetDwordValue(_T("movieOnEndPause"), movieOnEndPause);

	extern bool autoConvertMovieWhenPlaying;    // from movie.cpp
	regSetDwordValue(_T("autoConvertMovieWhenPlaying"), autoConvertMovieWhenPlaying);
}

