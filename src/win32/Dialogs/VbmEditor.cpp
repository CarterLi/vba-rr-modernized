// VbmEditor.cpp : the recorded key sequence as a frame/button grid.
//
// Read the contract at the top of VbmEditor.h and above VBAMovieSetInputAt() in
// common/movie.cpp: this window owns the movie's key timeline and nothing else.
// It does not seek, it does not pause, it does not change the movie's mode and
// it never writes a reset or motion bit - it only says "on frame N these pad
// buttons were held", which is exactly what a VBM file is.

#include "../stdafx.h"
#include "../resource.h"
#include "VbmEditor.h"
#include "../VBA.h"
#include "../UiFont.h"
#include "../WinResUtil.h"
#include "../WinText.h"

#include "../../common/System.h"
#include "../../common/Util.h"
#include "../../common/inputGlobal.h"
#include "../../common/movie.h"

#include <math.h>	// modf/fmod for the timestamp column

// The transport buttons paint themselves; a theme handle makes them look like
// the pushbuttons next to them instead of like a Windows 95 control.  It is a
// /DEFAULTLIB in the object file, so no project setting has to change.  Both
// the API and the library exist on XP, where the same call returns the Luna
// button; without a theme (classic desktop) OpenThemeData fails and the code
// falls back to DrawFrameControl.
#include <uxtheme.h>
#pragma comment(lib, "uxtheme.lib")

#ifndef LVS_EX_DOUBLEBUFFER
#	define LVS_EX_DOUBLEBUFFER 0x00010000	// Vista and later; ignored before
#endif

namespace
{

VbmEditor *g_editor = NULL;
HTHEME	   g_buttonTheme = NULL;

// The window size survives closing and reopening the editor within one run of
// the program.  It is deliberately not written to the settings file: how much
// of the screen the grid should cover is not a preference, and adding it to
// the settings would mean a new key for every window that becomes resizable.
CSize g_lastSize(0, 0);

// Every other dialog in this program is a modal one, and the dialog manager
// gives those WS_EX_DLGMODALFRAME (from DS_MODALFRAME in the template).  That
// frame style draws a caption with no icon at all, which is why they look the
// way they do without anyone asking for it.  This window cannot use it: it is
// modeless and has a sizing frame, and WS_EX_DLGMODALFRAME would draw the
// dialog border around it instead.
//
// Clearing the icon with WM_SETICON is not enough either: a window with no icon
// of its own borrows the owner's, and the main window's class was registered
// around LoadIcon(IDI_ICON) - so the caption went on showing the GBA icon
// (measured: window icon and class icon both NULL, the caption still drew one).
// A one pixel, fully transparent icon is the only thing that says "this window
// has no icon" to the title bar and to the Alt+Tab list.  It is created once
// and never freed - the process lives as long as it is needed.
HICON blankIcon()
{
	static HICON icon = NULL;

	if (icon == NULL)
	{
		// Monochrome masks, one bit per pixel and a row padded to a WORD: 1 in
		// the AND mask keeps the background, 0 in the XOR mask draws nothing.
		BYTE andMask[2] = { 0xFF, 0x00 };
		BYTE xorMask[2] = { 0x00, 0x00 };
		icon = ::CreateIcon(::GetModuleHandle(NULL), 1, 1, 1, 1, andMask, xorMask);
	}

	return icon;
}

// BP_PUSHBUTTON and PBS_* from vssym32.h.  Spelled out because an XP toolset
// still carries tmschema.h instead of that header; the values are part of the
// theme ABI and do not change between versions.
const int kButtonPart	  = 1;
const int kButtonNormal	  = 1;
const int kButtonPressed  = 2;
const int kButtonDisabled = 4;

// The pad bits in the order the on-screen display shows them (left, up, right,
// down, A, B, L, R, start, select), so a column here always means the same
// thing as the character in the OSD's input line.
struct KeyColumn
{
	uint16		 mask;
	const TCHAR *label;
};

const KeyColumn kKeys[] = {
	{ BUTTON_MASK_LEFT, _T("<") },
	{ BUTTON_MASK_UP, _T("^") },
	{ BUTTON_MASK_RIGHT, _T(">") },
	{ BUTTON_MASK_DOWN, _T("v") },
	{ BUTTON_MASK_A, _T("A") },
	{ BUTTON_MASK_B, _T("B") },
	{ BUTTON_MASK_L, _T("L") },
	{ BUTTON_MASK_R, _T("R") },
	{ BUTTON_MASK_START, _T("S") },
	{ BUTTON_MASK_SELECT, _T("s") },
};

// Bits the format records that are not pad buttons.  They get a column only
// when the movie actually contains one, and they are never editable: the
// emulator acts on them (a reset frame restarts the game), so they are not
// "a key the user pressed" and the editor has no business inventing them.
const KeyColumn kExtras[] = {
	{ BUTTON_MASK_NEW_RESET, _T("?") },
	{ BUTTON_MASK_OLD_RESET, _T("!") },
	{ BUTTON_MASK_LEFT_MOTION, _T("M{") },
	{ BUTTON_MASK_RIGHT_MOTION, _T("M}") },
	{ BUTTON_MASK_DOWN_MOTION, _T("Mv") },
	{ BUTTON_MASK_UP_MOTION, _T("M^") },
};

const int kKeyCount	 = int(sizeof(kKeys) / sizeof(kKeys[0]));
const int kExtraCount = int(sizeof(kExtras) / sizeof(kExtras[0]));

// Grid columns that are not a pad bit.  Everything from COL_FIRST_KEY on is one
// key per column.
enum { COL_PLAYHEAD = 0, COL_FRAME = 1, COL_TIME = 2, COL_FIRST_KEY = 3 };

// HH:mm:ss.SS for a frame number, computed the way the movie-open dialog
// computes the movie length: seconds = frames / systemGetFrameRate().  That rate
// is the GBA clock ratio (262144/4389 = 59.7275...), not 60 and not 59.94, and
// the fraction is truncated to hundredths exactly like that dialog does.
void formatStamp(TCHAR *buffer, uint32 row)
{
	const double total = double(row) / systemGetFrameRate();

	double		 wholeSec = 0.0;
	const uint32 frac	  = (uint32)(modf(total, &wholeSec) * 100.0);
	const uint32 seconds  = (uint32)fmod(wholeSec, 60.0);
	const uint32 minutes  = (uint32)fmod(wholeSec / 60.0, 60.0);
	const uint32 hours	  = (uint32)(wholeSec / 3600.0);

	_stprintf(buffer, _T("%02u:%02u:%02u.%02u"), hours, minutes, seconds, frac);
}

// Light colours on purpose: the dialog family in this fork does not follow the
// system dark mode (see DarkMode.cpp), so a dark grid would be the odd one out.
const COLORREF kPlayheadColor = RGB(198, 224, 255);
const COLORREF kMarkColor	  = RGB(255, 240, 190);

// Icons for the transport buttons.  They are drawn rather than loaded so they
// scale with the dialog units, follow the system button colour, and cost no
// bitmap resources in either language module.
enum Glyph { GLYPH_PLAY, GLYPH_PAUSE, GLYPH_STEP, GLYPH_PLAYHEAD };

void drawGlyph(CDC &dc, const CRect &box, int kind, COLORREF color)
{
	const int cx = (box.left + box.right) / 2;
	const int cy = (box.top + box.bottom) / 2;
	const int qh = max(6, box.Height() * 9 / 16);	// glyph height
	const int h2 = qh / 2;
	const int t	 = max(1, qh / 6);				// stroke thickness

	switch (kind)
	{
	case GLYPH_PLAY:
	{
		const int w = max(3, qh * 5 / 6);
		CPoint	  tri[3] = { CPoint(cx - w / 2, cy - h2), CPoint(cx + w / 2, cy), CPoint(cx - w / 2, cy + h2) };

		CBrush brush(color);
		CPen   pen(PS_SOLID, 1, color);
		CBrush *oldBrush = dc.SelectObject(&brush);
		CPen   *oldPen	 = dc.SelectObject(&pen);
		dc.Polygon(tri, 3);
		dc.SelectObject(oldPen);
		dc.SelectObject(oldBrush);
		break;
	}

	case GLYPH_PAUSE:
	{
		const int bar = max(2, qh / 3);
		const int gap = max(1, bar / 2);
		dc.FillSolidRect(cx - gap - bar, cy - h2, bar, qh, color);
		dc.FillSolidRect(cx + gap, cy - h2, bar, qh, color);
		break;
	}

	case GLYPH_STEP:
	{
		// the play triangle with a stop bar after it
		const int triW = max(3, qh * 3 / 5);
		const int left = cx - (triW + t + 1) / 2;
		CPoint	  tri[3] = { CPoint(left, cy - h2), CPoint(left + triW, cy), CPoint(left, cy + h2) };

		CBrush brush(color);
		CPen   pen(PS_SOLID, 1, color);
		CBrush *oldBrush = dc.SelectObject(&brush);
		CPen   *oldPen	 = dc.SelectObject(&pen);
		dc.Polygon(tri, 3);
		dc.SelectObject(oldPen);
		dc.SelectObject(oldBrush);

		dc.FillSolidRect(left + triW + 1, cy - h2, t, qh, color);
		break;
	}

	default:	// GLYPH_PLAYHEAD: an arrow landing on a line
	{
		const int lineY = cy + h2;
		const int aw	= max(3, qh / 3);	// arrow half width
		const int ah	= max(3, qh / 3);	// arrow height
		const int top	= lineY - t - ah;

		CPoint tri[3] = { CPoint(cx - aw, top), CPoint(cx + aw, top), CPoint(cx, lineY - t) };

		CBrush brush(color);
		CPen   pen(PS_SOLID, 1, color);
		CBrush *oldBrush = dc.SelectObject(&brush);
		CPen   *oldPen	 = dc.SelectObject(&pen);
		dc.Polygon(tri, 3);
		dc.SelectObject(oldPen);
		dc.SelectObject(oldBrush);

		if (top > cy - h2)
			dc.FillSolidRect(cx - t / 2, cy - h2, t, top - (cy - h2), color);
		dc.FillSolidRect(cx - qh * 2 / 3, lineY - t, qh * 4 / 3, t, color);
		break;
	}
	}
}

} // namespace

VbmEditor::VbmEditor(CWnd *parent)
	: CDialog(VbmEditor::IDD, parent)
	, m_active(false)
	, m_readOnlyMode(0)
	, m_state(0)
	, m_length(NO_ROW)
	, m_frame(NO_ROW)
	, m_generation(0xFFFFFFFFu)
	, m_usedMask(0)
	, m_editLock(true)
	, m_follow(true)
	, m_uiChecks(-1)
	, m_uiEnable(-1)
	, m_focusRow(-1)
	, m_layoutDpi(0)
	, m_minTrack(0, 0)
	, m_fixedHeight(0)
	, m_fittedWidth(0)
	, m_fittedFont(NULL)
	, m_clipSequence(0)
	, m_clipResult(vbmcsv::Result::Empty)
	, m_clipBadLine(0)
{
	m_cellText[0] = 0;
}

void VbmEditor::open(CWnd *parent)
{
	if (g_editor)
	{
		if (g_editor->GetSafeHwnd() != NULL)
		{
			if (g_editor->IsIconic())
				g_editor->ShowWindow(SW_RESTORE);
			g_editor->SetForegroundWindow();
			return;
		}

		g_editor = NULL;	// the window went away behind our back
	}

	VbmEditor *editor = new VbmEditor(parent);
	if (!editor->Create(IDD_VBM_EDITOR, parent))
	{
		delete editor;
		return;
	}

	g_editor = editor;
	editor->ShowWindow(SW_SHOW);
}

void VbmEditor::DoDataExchange(CDataExchange *pDX)
{
	CDialog::DoDataExchange(pDX);
	DDX_Control(pDX, IDC_VBM_LIST, m_list);
}

BEGIN_MESSAGE_MAP(VbmEditor, CDialog)
ON_WM_DESTROY()
ON_WM_TIMER()
ON_WM_SIZE()
ON_WM_GETMINMAXINFO()
ON_WM_DRAWITEM()
ON_BN_CLICKED(IDC_VBM_READONLY, OnReadOnlyClicked)
ON_BN_CLICKED(IDC_VBM_FOLLOW, OnFollowClicked)
ON_BN_CLICKED(IDC_VBM_PREVMARK, OnPrevMark)
ON_BN_CLICKED(IDC_VBM_NEXTMARK, OnNextMark)
ON_BN_CLICKED(IDC_VBM_PLAY, OnPlay)
ON_BN_CLICKED(IDC_VBM_PAUSE, OnPause)
ON_BN_CLICKED(IDC_VBM_STEP, OnStepFrame)
ON_BN_CLICKED(IDC_VBM_GOTOPLAYHEAD, OnGoToPlayhead)
ON_NOTIFY(LVN_GETDISPINFO, IDC_VBM_LIST, OnListGetDispInfo)
ON_NOTIFY(NM_CUSTOMDRAW, IDC_VBM_LIST, OnListCustomDraw)
ON_NOTIFY(LVN_ITEMCHANGED, IDC_VBM_LIST, OnListItemChanged)
ON_NOTIFY(NM_CLICK, IDC_VBM_LIST, OnListClick)
ON_NOTIFY(NM_RCLICK, IDC_VBM_LIST, OnListRightClick)
ON_NOTIFY(LVN_KEYDOWN, IDC_VBM_LIST, OnListKeyDown)
// The three standard edit commands, so the clipboard is reachable without a
// keyboard: a WM_COMMAND with these ids is what the accelerators would send.
ON_COMMAND(ID_EDIT_CUT, OnCut)
ON_COMMAND(ID_EDIT_COPY, OnCopy)
ON_COMMAND(ID_EDIT_PASTE, OnPaste)
END_MESSAGE_MAP()

BOOL VbmEditor::OnInitDialog()
{
	CDialog::OnInitDialog();

	GetWindowText(m_titleBase);

	// The template carries no icon, so the caption would inherit the
	// application's and put it in front of the title.  This window stands for a
	// timeline, not for a document, and the icon only eats caption width.
	HICON blank = blankIcon();
	if (blank)
	{
		SetIcon(blank, TRUE);	// Alt+Tab and the task list
		SetIcon(blank, FALSE);	// the caption bar
	}

	// A virtual list keeps no per-item state, which is the whole point: a
	// two-hour movie is one SetItemCountEx() and nothing else.
	m_list.SetExtendedStyle(m_list.GetExtendedStyle() | LVS_EX_FULLROWSELECT
	                        | LVS_EX_GRIDLINES | LVS_EX_DOUBLEBUFFER);

	SetDlgItemText(IDC_VBM_HINT, winResLoadString(IDS_VBMEDITOR_HINT));

	// The size the template was drawn at gives the narrowest window the frame
	// allows; taking it from the live window rather than from dialog units
	// keeps it right at any DPI.  captureLayout() then adds the height floor,
	// which depends on the header and so cannot be known before the columns
	// are filled in.
	CRect window;
	GetWindowRect(&window);
	m_minTrack = window.Size();
	captureLayout();

	// Reopening the editor in the same run keeps the size the user dragged it
	// to, but never below the floor: a size remembered from before the floor
	// existed would bring the window back with the grid squeezed out of shape.
	int restoreWidth	= g_lastSize.cx;
	int restoreHeight	= g_lastSize.cy;
	if (restoreWidth > 0 && restoreHeight > 0)
	{
		if (restoreWidth < m_minTrack.cx)
			restoreWidth = m_minTrack.cx;
		if (restoreHeight < m_minTrack.cy)
			restoreHeight = m_minTrack.cy;

		// SetWindowPos below arrives in OnSize() as an ordinary resize.
		if (restoreWidth != window.Width() || restoreHeight != window.Height())
		{
			SetWindowPos(NULL, 0, 0, restoreWidth, restoreHeight,
			             SWP_NOMOVE | SWP_NOZORDER | SWP_NOACTIVATE);
		}
	}

	// NULL when the desktop runs without themes; OnDrawItem() then falls back
	// to the classic frame control.
	g_buttonTheme = ::OpenThemeData(m_hWnd, L"BUTTON");

	// The transport buttons are drawn by OnDrawItem(), so the only thing they
	// can say to the user is a tooltip.
	if (m_tip.Create(this, TTS_ALWAYSTIP))
	{
		m_tip.SetMaxTipWidth(dlux(200));	// pixels: keep it in dialog-unit step with the font
		m_tip.AddTool(GetDlgItem(IDC_VBM_PLAY), winResLoadString(IDS_VBMEDITOR_TIP_PLAY));
		m_tip.AddTool(GetDlgItem(IDC_VBM_PAUSE), winResLoadString(IDS_VBMEDITOR_TIP_PAUSE));
		m_tip.AddTool(GetDlgItem(IDC_VBM_STEP), winResLoadString(IDS_VBMEDITOR_TIP_STEP));
		m_tip.AddTool(GetDlgItem(IDC_VBM_GOTOPLAYHEAD), winResLoadString(IDS_VBMEDITOR_TIP_PLAYHEAD));
		m_tip.AddTool(GetDlgItem(IDC_VBM_FRAMENO), winResLoadString(IDS_VBMEDITOR_TIP_FRAMENO));
		m_tip.Activate(TRUE);
	}

	SetTimer(TIMER_REFRESH, REFRESH_MS, NULL);

	refresh();

	return TRUE;
}

BOOL VbmEditor::PreTranslateMessage(MSG *msg)
{
	if (msg->message == WM_KEYDOWN && msg->wParam == VK_RETURN)
	{
		// Enter in the frame box confirms it; Enter anywhere else must not
		// close the window, so the default handler is not reached at all.
		CWnd *box = GetDlgItem(IDC_VBM_FRAMENO);
		if (box && msg->hwnd == box->GetSafeHwnd())
		{
			applyFrameBox();
			return TRUE;
		}
	}

	// Ctrl+X/C/V work wherever the focus is except in the frame box, where they
	// belong to the edit control (copying a frame number out is harmless and
	// should keep working).
	if (msg->message == WM_KEYDOWN && GetKeyState(VK_CONTROL) < 0)
	{
		CWnd *box = GetDlgItem(IDC_VBM_FRAMENO);
		if (!box || msg->hwnd != box->GetSafeHwnd())
		{
			switch (msg->wParam)
			{
			case 'C':
				copySelection(false);
				return TRUE;
			case 'X':
				copySelection(true);
				return TRUE;
			case 'V':
				paste();
				return TRUE;
			default:
				break;
			}
		}
	}

	// 'M' marks the selection, and it has to be taken away from the list
	// before the list sees it: a list view treats a typed character as an
	// incremental search, and for an owner-data list that means asking the
	// parent for the matching row (LVN_ODFINDITEM).  Nobody answers, the
	// control takes the default -1-as-0, and it *selects row 0* - so the
	// multi-selection was gone by the time the key was done.  Swallowing it
	// here costs nothing: this window owns the key, the grid never searches.
	if (msg->message == WM_KEYDOWN && msg->wParam == 'M'
	    && msg->hwnd == m_list.GetSafeHwnd())
	{
		toggleMarkSelection();
		return TRUE;
	}

	if (m_tip.GetSafeHwnd())
		m_tip.RelayEvent(msg);

	return CDialog::PreTranslateMessage(msg);
}

void VbmEditor::OnOK()
{
	// A modeless dialog would be destroyed by the base class.  Enter belongs to
	// the frame box, and there is nothing else here that wants it.
	applyFrameBox();
}

void VbmEditor::OnCancel()
{
	// CDialog::OnCancel() calls EndDialog(), which for a window created with
	// Create() only *hides* it: the object survives, PostNcDestroy never runs and
	// the next open finds a "singleton" whose window nobody can see.  Closing has
	// to destroy the window itself.
	DestroyWindow();
}

void VbmEditor::PostNcDestroy()
{
	delete this;
}

void VbmEditor::OnDestroy()
{
	KillTimer(TIMER_REFRESH);
	g_editor = NULL;

	CRect window;
	GetWindowRect(&window);
	g_lastSize = window.Size();

	if (g_buttonTheme)
	{
		::CloseThemeData(g_buttonTheme);
		g_buttonTheme = NULL;
	}

	CDialog::OnDestroy();
}

void VbmEditor::OnTimer(UINT_PTR event)
{
	if (event == TIMER_REFRESH)
		refresh();

	CDialog::OnTimer(event);
}

/////////////////////////////////////////////////////////////////////////////
// resizing
//
// The window has a thick frame (see the IDD_VBM_EDITOR style): the grid takes
// every pixel that is added or removed, the hint and the status line stay
// glued to the bottom edge, and the frame box with its caption and the two
// mark buttons follow the right edge.  Nothing is stretched sideways, so the
// layout at any size is the one the template describes plus a delta - which is
// also why captureLayout() only has to run again when the DPI changes.

CRect VbmEditor::controlRect(int id) const
{
	CRect rect(0, 0, 0, 0);

	CWnd *control = GetDlgItem(id);
	if (control)
	{
		control->GetWindowRect(&rect);
		ScreenToClient(&rect);
	}

	return rect;
}

void VbmEditor::captureLayout()
{
	CRect client;
	GetClientRect(&client);

	m_layout.client	= client.Size();
	m_layout.list	= controlRect(IDC_VBM_LIST);
	m_layout.hint	= controlRect(IDC_VBM_HINT);
	m_layout.status	= controlRect(IDC_VBM_STATUS);
	m_layout.gotoLabel = controlRect(IDC_VBM_GOTOLABEL);
	m_layout.frameBox  = controlRect(IDC_VBM_FRAMENO);
	m_layout.prevMark  = controlRect(IDC_VBM_PREVMARK);
	m_layout.nextMark  = controlRect(IDC_VBM_NEXTMARK);

	m_layoutDpi = uiFont::dpiOf(m_hWnd);

	// Everything that is not the grid keeps its height while the window is
	// dragged, so it is a constant that can be measured once here.  It is
	// measured as a difference rather than added up from the controls so the
	// caption, the frame and any gap below the status line come along for free,
	// and so a DPI change only has to take a new snapshot.
	CRect window;
	if (::GetWindowRect(m_hWnd, &window))
		m_fixedHeight = window.Height() - m_layout.list.Height();

	updateHeightFloor();
}

// The header of the list, in pixels: what the grid keeps when it is squeezed.
int VbmEditor::listHeaderHeight() const
{
	HWND header = (HWND)::SendMessage(m_list.GetSafeHwnd(), LVM_GETHEADER, 0, 0);
	RECT rect;
	if (!header || !::GetWindowRect(header, &rect))
		return 0;

	return rect.bottom - rect.top;
}

void VbmEditor::updateHeightFloor()
{
	// The grid may give up every row it has, but not the header: below that
	// the columns are a strip of unlabelled numbers.  Because m_fixedHeight
	// already holds the caption, the transport row, the checkboxes and the two
	// bottom lines, this floor also keeps all of them on screen - the hint and
	// the status line land exactly under the shrunken grid, and the frame
	// would have to clip them for anything smaller.
	const int header = listHeaderHeight();
	if (header > 0 && m_fixedHeight > 0)
		m_minTrack.cy = m_fixedHeight + header;
}

void VbmEditor::moveControl(int id, const CRect &rect)
{
	if (CWnd *control = GetDlgItem(id))
		control->MoveWindow(&rect);
}

void VbmEditor::layoutChildren(int cx, int cy)
{
	const int dx = cx - m_layout.client.cx;
	const int dy = cy - m_layout.client.cy;

	// Every control is placed from the captured baseline plus the *total*
	// delta, never from where it happens to be now.  That keeps the result
	// identical no matter how many WM_SIZE messages the drag produced, and it
	// makes the round trip exact: shrinking back to the template size gives
	// dx = dy = 0, which has to put the controls back where they started
	// rather than be skipped as "nothing changed".

	CRect list = m_layout.list;
	list.right += dx;
	list.bottom += dy;

	// The frame keeps the window above both floors (see OnGetMinMaxInfo), so
	// these only guard against a SetWindowPos() from somewhere else.
	if (list.Width() < 1)
		list.right = list.left + 1;
	if (list.Height() < 1)
		list.bottom = list.top + 1;

	// The two bottom lines follow the frame, but they must not climb over the
	// grid.  At the minimum height the grid is exactly one header tall and they
	// sit right under it, so this only bites when something shrank the window
	// past the floor.
	int bottomDy = dy;
	if (m_layout.hint.top + bottomDy < list.top + 1)
		bottomDy = list.top + 1 - m_layout.hint.top;

	CRect hint	 = m_layout.hint;
	CRect status = m_layout.status;
	hint.top += bottomDy;
	hint.bottom += bottomDy;
	hint.right += dx;
	status.top += bottomDy;
	status.bottom += bottomDy;
	status.right += dx;

	CRect label = m_layout.gotoLabel;
	CRect box	= m_layout.frameBox;
	CRect prev	= m_layout.prevMark;
	CRect next	= m_layout.nextMark;
	label.OffsetRect(dx, 0);
	box.OffsetRect(dx, 0);
	prev.OffsetRect(dx, 0);
	next.OffsetRect(dx, 0);

	moveControl(IDC_VBM_LIST, list);
	moveControl(IDC_VBM_HINT, hint);
	moveControl(IDC_VBM_STATUS, status);
	moveControl(IDC_VBM_GOTOLABEL, label);
	moveControl(IDC_VBM_FRAMENO, box);
	moveControl(IDC_VBM_PREVMARK, prev);
	moveControl(IDC_VBM_NEXTMARK, next);
}

void VbmEditor::OnSize(UINT type, int cx, int cy)
{
	CDialog::OnSize(type, cx, cy);

	// Nothing has been measured yet (the dialog manager sizes the window
	// before WM_INITDIALOG) or there is nothing to lay out.
	if (type == SIZE_MINIMIZED || cx <= 0 || cy <= 0 || m_layout.client.cx <= 0)
		return;

	// A per-monitor DPI change resizes the window before anything is told
	// about it: UiFont.cpp scales the children, then applies the suggested
	// rect (and swallows the WM_DPICHANGED itself, so this is the only place
	// that sees it).  The controls are already in their new size here, so this
	// size is taken as the new baseline instead of being applied as a delta on
	// top of the scaling, and the smallest allowed window follows the ratio.
	const UINT dpi = uiFont::dpiOf(m_hWnd);
	if (dpi != m_layoutDpi)
	{
		// The width floor is the template width, which scales with the DPI;
		// the height floor is measured from the new header instead of scaled,
		// because a font can change height by more than the ratio suggests.
		if (dpi != 0 && m_layoutDpi != 0)
			m_minTrack.cx = MulDiv(m_minTrack.cx, dpi, m_layoutDpi);
		captureLayout();
	}
	else
	{
		layoutChildren(cx, cy);
	}

	// Every key column shares whatever the three fixed ones leave over, so a
	// change in width has to be measured again.  A vertical drag - the reason
	// the window can be resized at all - does not, and re-measuring per pixel
	// would repaint the grid for nothing.
	CRect area;
	m_list.GetClientRect(&area);
	if (area.Width() != m_fittedWidth)
	{
		m_list.SetRedraw(FALSE);
		fitColumns();
		m_list.SetRedraw(TRUE);
		m_list.Invalidate(FALSE);
	}
}

void VbmEditor::OnGetMinMaxInfo(MINMAXINFO *info)
{
	CDialog::OnGetMinMaxInfo(info);

	// Below the template width the frame box, the mark buttons and the columns
	// would be clipped sideways, and there is no horizontal scrolling to fall
	// back on.  The height is floored at the point where the grid is down to
	// its column header - one more pixel and the columns lose their labels,
	// one less and the two bottom lines would be pushed out of the frame.
	if (m_minTrack.cx > 0)
		info->ptMinTrackSize.x = m_minTrack.cx;
	if (m_minTrack.cy > 0)
		info->ptMinTrackSize.y = m_minTrack.cy;
}

/////////////////////////////////////////////////////////////////////////////
// state

void VbmEditor::refresh()
{
	const uint32 oldHead = playheadRow();

	// A per-monitor DPI change and the "modern UI font" option both swap the
	// font under the list from outside this window (UiFont.cpp does it from a
	// subclass procedure), and the column widths are measured from that font.
	// The tick is the only place that reliably runs afterwards and it costs one
	// GetFont() when nothing changed.
	CFont *font = m_list.GetFont();
	if (!m_columns.empty() && (font ? (HFONT)font->GetSafeHandle() : NULL) != m_fittedFont)
	{
		// A taller font means a taller header, so the floor has to move with
		// it; fitColumns() records the new font, which is what makes this run
		// once per change rather than once per tick.
		updateHeightFloor();
		fitColumns();
	}

	const bool	 active = VBAMovieIsActive() != 0;
	const int	 mode	= active ? VBAMovieGetReadOnlyMode() : 0;
	const uint32 state	= active ? VBAMovieGetState() : 0;
	const uint32 length = active ? VBAMovieGetLength() : 0;
	const uint32 frame	= active ? VBAMovieGetFrameCounter() : 0;
	const uint32 gen	= active ? VBAMovieGetInputGeneration() : 0;

	CString identity;
	if (active)
		identity.Format(_T("%s|%u"), (LPCTSTR)winWideFromAnsi(VBAMovieGetFilename()), VBAMovieGetId());

	if (!active)
	{
		if (m_active || m_columns.size() || m_list.GetItemCount())
		{
			m_marks.clear();
			m_columns.clear();
			while (m_list.DeleteColumn(0))
				;
			m_list.SetItemCountEx(0);
			m_list.Invalidate(FALSE);
		}

		m_active	   = false;
		m_identity.Empty();
		m_readOnlyMode = 0;
		m_state		   = 0;
		m_length	   = NO_ROW;
		m_frame		   = NO_ROW;
		m_generation   = 0;
		m_usedMask	   = 0;
		m_focusRow	   = -1;

		syncFrameBox();
		updateTitle();
		updateControls();
		setStatusRes(IDS_VBMEDITOR_NO_MOVIE);
		return;
	}

	const bool switched = (identity != m_identity);
	// The movie's own lock is a menu toggle (Tools > Movie > Read-Only Mode),
	// so it can change while this window sits open on the same movie.  The
	// status line below has to follow that edge, not just a movie switch.
	const bool modeChanged = switched || (mode != m_readOnlyMode);

	m_active	   = true;	// before updateTitle(), which asks whether one is open
	m_readOnlyMode = mode;
	m_state		   = state;

	if (switched)
	{
		m_identity = identity;
		m_marks.clear();
		resetView();
	}

	// The item count comes first: a virtual list only grows a vertical scroll
	// bar once it has rows, and fitColumns() has to measure the client area it
	// will actually have.  Built the other way round, the columns were fitted
	// to a list without a scroll bar and the last key column ended up clipped
	// by its width.
	if (length != m_length)
	{
		if (m_length != NO_ROW && length == m_length + 1)
		{
			// one frame was appended (recording, or the playhead advancing):
			// tell the control about the new count without repainting the list
			m_list.SetItemCountEx((int)length, LVSICF_NOINVALIDATEALL);
			repaintRow(m_length);
		}
		else
		{
			m_list.SetItemCountEx((int)length);
			m_list.Invalidate(FALSE);
		}
		m_length = length;
	}

	// The column set only depends on the movie's contents (which extra bits
	// appear) and on the machine it was recorded on, so it is rebuilt only when
	// the buffer was rewritten, and even then only if the set really changed.
	if (gen != m_generation || m_columns.empty())
	{
		const uint16 used = VBAMovieGetUsedInputMask(0);
		if (used != m_usedMask || m_columns.empty())
		{
			m_usedMask = used;
			rebuildColumns(used);
			m_list.Invalidate(FALSE);
		}
		m_generation = gen;
	}

	m_frame = frame;

	// The focus is cached, so it has to be re-validated when the timeline
	// shrinks under it - a delete, a truncate or another movie.
	if (m_focusRow >= 0 && (uint32)m_focusRow >= length)
	{
		m_focusRow = -1;
		syncFrameBox();
	}

	const uint32 newHead = playheadRow();
	if (newHead != oldHead)
	{
		repaintRow(oldHead);
		repaintRow(newHead);
		followPlayhead(newHead);
	}

	if (modeChanged && mode == 2)
		setStatusRes(IDS_VBMEDITOR_FILE_READONLY);
	else if (modeChanged && mode != 0)
		setStatusRes(IDS_VBMEDITOR_READONLY_STATE);
	else if (modeChanged)
		setStatus(_T(""));	// unlocked: the old sentence would now be wrong

	updateControls();
}

void VbmEditor::resetView()
{
	// force the next refresh to rebuild the column set for the new movie
	m_generation = 0xFFFFFFFFu;
	m_usedMask	 = 0;
	m_length	 = NO_ROW;
	m_frame		 = NO_ROW;
	m_focusRow	 = -1;

	m_columns.clear();
	while (m_list.DeleteColumn(0))
		;
	m_list.SetItemCountEx(0);
	m_list.Invalidate(FALSE);

	syncFrameBox();
	updateTitle();
}

void VbmEditor::rebuildColumns(uint16 usedMask)
{
	std::vector<Column> columns;

	Column marker = { 0, _T(""), 0 };	// playhead arrow
	columns.push_back(marker);
	Column frameColumn = { 0, _T(""), 0 };
	columns.push_back(frameColumn);
	Column timeColumn = { 0, _T(""), 0 };	// HH:mm:ss.SS of that frame
	columns.push_back(timeColumn);

	const bool gba = (systemCartridgeType == IMAGE_GBA);
	for (int i = 0; i < kKeyCount; ++i)
	{
		// a plain GB has no shoulder buttons, so those bits are always zero
		if (!gba && (kKeys[i].mask & BUTTON_GBA_ONLY))
			continue;

		Column column = { kKeys[i].mask, _T(""), 0 };
		_tcscpy(column.label, kKeys[i].label);
		columns.push_back(column);
	}

	for (int i = 0; i < kExtraCount; ++i)
	{
		if (!(usedMask & kExtras[i].mask))
			continue;

		Column column = { kExtras[i].mask, _T(""), 0 };
		_tcscpy(column.label, kExtras[i].label);
		columns.push_back(column);
	}

	if (columns.size() == m_columns.size())
	{
		size_t i = 0;
		while (i < columns.size() && columns[i].mask == m_columns[i].mask)
			++i;
		if (i == columns.size())
			return;	 // same set: leave the control (and the user's widths) alone
	}

	// Widths are pixels, and dialog units are no longer a safe way to get them:
	// the template font is swapped for the system UI font after the dialog is
	// created (UiFont.cpp), and on a per-monitor DPI change the children are
	// rescaled without touching the units.  Everything is measured from the
	// list's own font instead, which is also what decides the row height.
	m_list.SetRedraw(FALSE);

	while (m_list.DeleteColumn(0))
		;

	for (size_t i = 0; i < columns.size(); ++i)
	{
		CString heading;
		if (i == COL_FRAME)
			heading = winResLoadString(IDS_VBMEDITOR_FRAME);
		else if (i == COL_TIME)
			heading = winResLoadString(IDS_VBMEDITOR_TIME);
		else if ((int)i >= COL_FIRST_KEY)
			heading = columns[i].label;

		const int align = (i == COL_PLAYHEAD) ? LVCFMT_LEFT
		                : (i == COL_FRAME || i == COL_TIME) ? LVCFMT_RIGHT
		                                                   : LVCFMT_CENTER;

		m_list.InsertColumn((int)i, heading, align, 0, (int)i);
	}

	m_columns.swap(columns);
	fitColumns();
	updateHeightFloor();		// the header only exists now that there are columns

	m_list.SetRedraw(TRUE);
	m_list.Invalidate(FALSE);
}

void VbmEditor::fitColumns()
{
	// Whichever way out of this function is taken, the font it measured (or
	// skipped measuring) with is the current one.
	CFont *font = m_list.GetFont();
	m_fittedFont = font ? (HFONT)font->GetSafeHandle() : NULL;

	const int count = (int)m_columns.size();
	if (count <= COL_FIRST_KEY)
		return;

	CSize digit(6, 12), frameHeader(30, 12), stamp(60, 12), timeHeader(20, 12);
	int	  pad = 6;

	CDC *dc = m_list.GetDC();
	if (dc)
	{
		CFont	  *old	= font ? dc->SelectObject(font) : NULL;
		TEXTMETRIC metrics;

		if (dc->GetTextMetrics(&metrics))
		{
			digit		= dc->GetTextExtent(_T("0"));
			frameHeader = dc->GetTextExtent(winResLoadString(IDS_VBMEDITOR_FRAME));
			timeHeader	= dc->GetTextExtent(winResLoadString(IDS_VBMEDITOR_TIME));
			stamp		= dc->GetTextExtent(_T("00:00:00.00"));
			pad			= max(4, (int)metrics.tmAveCharWidth);
		}

		if (old)
			dc->SelectObject(old);
		m_list.ReleaseDC(dc);
	}

	CRect client;
	m_list.GetClientRect(&client);

	int available = client.Width() - ::GetSystemMetrics(SM_CXVSCROLL) - pad;
	if (available < digit.cx * 14)
		available = digit.cx * 14;

	m_columns[COL_PLAYHEAD].width = max(digit.cx * 2, 8);	// the playhead arrow

	// The frame number is the column that has to stay readable at a glance, so
	// it gets seven digit widths plus air on both sides, and never less than its
	// heading.  Everything left over is shared by the key columns.
	int frameWidth = digit.cx * 7 + pad * 2;
	if (frameHeader.cx + pad > frameWidth)
		frameWidth = frameHeader.cx + pad;
	m_columns[COL_FRAME].width = frameWidth;

	// A little slack on both sides: the stamp is right aligned, so the spare room
	// keeps it away from the key columns instead of touching their header.
	int stampWidth = stamp.cx + pad * 2;
	if (timeHeader.cx + pad * 2 > stampWidth)
		stampWidth = timeHeader.cx + pad * 2;
	m_columns[COL_TIME].width = stampWidth;

	const int fixed = m_columns[COL_PLAYHEAD].width + m_columns[COL_FRAME].width
	                + m_columns[COL_TIME].width;
	const int keys = count - COL_FIRST_KEY;
	if (keys > 0)
	{
		int keyW = (available - fixed) / keys;
		if (keyW < digit.cx * 2)
			keyW = digit.cx * 2;

		for (int i = COL_FIRST_KEY; i < count; ++i)
			m_columns[i].width = keyW;

		// the last column absorbs the rounding remainder, so the grid ends at
		// the right edge of the client area instead of leaving a gap
		const int rest = available - fixed - keyW * (keys - 1);
		if (rest >= digit.cx * 2)
			m_columns[count - 1].width = rest;
	}

	for (int i = 0; i < count; ++i)
		m_list.SetColumnWidth(i, m_columns[i].width);

	// Remembered so a WM_SIZE that only changes the height can skip this.
	m_fittedWidth = client.Width();
}

int VbmEditor::dlux(int dlu) const
{
	RECT rect = { 0, 0, dlu, 1 };
	::MapDialogRect(m_hWnd, &rect);
	return max(1, (int)rect.right);
}

void VbmEditor::updateControls()
{
	// While the movie itself is read-only there is nothing this window could
	// do about it, so the box is checked and disabled rather than pretending to
	// be a switch the user can flip.
	const bool movieLocked = !m_active || m_readOnlyMode != 0;

	// Two different reasons put the check mark there: the movie is locked, or
	// the user locked this window.  Only the first one greys the box out, so
	// both have to reach the cache - with just the check bit, cancelling the
	// movie's read-only mode changed neither bit (m_editLock still held the
	// mark) and EnableWindow() was never called again: the box stayed checked
	// and disabled until the dialog was reopened.
	const int checks = (movieLocked || m_editLock ? 1 : 0) | (m_follow ? 2 : 0)
	                 | (movieLocked ? 4 : 0);
	if (checks != m_uiChecks)
	{
		m_uiChecks = checks;
		CheckDlgButton(IDC_VBM_READONLY, (checks & 1) ? BST_CHECKED : BST_UNCHECKED);
		CheckDlgButton(IDC_VBM_FOLLOW, (checks & 2) ? BST_CHECKED : BST_UNCHECKED);
		GetDlgItem(IDC_VBM_READONLY)->EnableWindow(movieLocked ? FALSE : TRUE);
	}

	// Transport: Play and Pause are the two states of File > Pause, so only the
	// one that would do something is offered.  Nothing here runs without a
	// movie, because there would be nothing to follow in the grid.
	const bool paused  = theApp.paused ? true : false;
	const bool canPlay = m_active && paused;
	const bool canStop = m_active && !paused;
	const bool hasMark = !m_marks.empty();

	int mask = 0;
	if (canPlay) mask |= 0x01;
	if (canStop) mask |= 0x02;
	if (m_active) mask |= 0x04;	 // frame advance
	if (m_active) mask |= 0x08;	 // go to playhead
	if (m_active) mask |= 0x10;	 // jump to a frame number
	if (hasMark) mask |= 0x20;
	if (hasMark) mask |= 0x40;

	if (mask == m_uiEnable)
		return;

	m_uiEnable = mask;
	GetDlgItem(IDC_VBM_PLAY)->EnableWindow((mask & 0x01) ? TRUE : FALSE);
	GetDlgItem(IDC_VBM_PAUSE)->EnableWindow((mask & 0x02) ? TRUE : FALSE);
	GetDlgItem(IDC_VBM_STEP)->EnableWindow((mask & 0x04) ? TRUE : FALSE);
	GetDlgItem(IDC_VBM_GOTOPLAYHEAD)->EnableWindow((mask & 0x08) ? TRUE : FALSE);
	GetDlgItem(IDC_VBM_FRAMENO)->EnableWindow((mask & 0x10) ? TRUE : FALSE);
	GetDlgItem(IDC_VBM_PREVMARK)->EnableWindow((mask & 0x20) ? TRUE : FALSE);
	GetDlgItem(IDC_VBM_NEXTMARK)->EnableWindow((mask & 0x40) ? TRUE : FALSE);
}

void VbmEditor::updateTitle()
{
	CString text = m_titleBase;

	if (m_active)
	{
		CString path = winWideFromAnsi(VBAMovieGetFilename());
		int		slash = path.ReverseFind(_T('\\'));
		if (slash < 0)
			slash = path.ReverseFind(_T('/'));
		if (slash >= 0)
			path = path.Mid(slash + 1);

		text += _T(" - ");
		text += path;
	}

	SetWindowText(text);
}

bool VbmEditor::editable() const
{
	return m_active && m_readOnlyMode == 0 && !m_editLock;
}

bool VbmEditor::canRestructure() const
{
	// The same three refusals the core applies, asked once so the menu and the
	// commands cannot disagree: a zero-frame or ENDed movie would undo the
	// insert on the next recorded frame, or cannot take one at all.
	return editable() && !VBAMovieIsRecording() && m_state != MOVIE_STATE_END;
}

void VbmEditor::denyRestructure()
{
	if (!editable())
		setStatusRes(m_readOnlyMode == 2 ? IDS_VBMEDITOR_FILE_READONLY : IDS_VBMEDITOR_CANNOT_EDIT);
	else if (VBAMovieIsRecording())
		setStatusRes(IDS_VBMEDITOR_RECORD_STATE);
	else
		setStatusRes(IDS_VBMEDITOR_END_STATE);
}

uint32 VbmEditor::playheadRow() const
{
	if (!m_active || m_length == 0 || m_length == NO_ROW)
		return NO_ROW;

	// While recording, currentFrame is the frame that is about to run and does
	// not exist in the list yet, so the last recorded row is the playhead.
	return (m_frame < m_length) ? m_frame : m_length - 1;
}

int VbmEditor::focusedRow() const
{
	// Cached rather than asked for: GetNextItem(LVNI_FOCUSED) walks the list,
	// and this would otherwise run on every tick of a 100 000-frame movie.
	return m_focusRow;
}

std::vector<uint32> VbmEditor::selectedRows() const
{
	std::vector<uint32> rows;

	// GetFirstSelectedItemPosition walks in display order, so the result is
	// already ascending - which is what VBAMovieDeleteFramesAt() requires.
	POSITION pos = m_list.GetFirstSelectedItemPosition();
	while (pos)
		rows.push_back((uint32)m_list.GetNextSelectedItem(pos));

	return rows;
}

void VbmEditor::selectOnlyRow(uint32 row)
{
	const int target = (int)row;
	if (target < 0 || target >= m_list.GetItemCount())
		return;

	// -1 with LVIS_SELECTED clears the whole selection, which is how a jump
	// turns a multi-selection back into the single row it landed on.
	m_list.SetItemState(-1, 0, LVIS_SELECTED);
	m_list.SetItemState(target, LVIS_SELECTED | LVIS_FOCUSED, LVIS_SELECTED | LVIS_FOCUSED);

	m_focusRow = target;
	syncFrameBox();
}

/////////////////////////////////////////////////////////////////////////////
// list contents

void VbmEditor::OnListGetDispInfo(NMHDR *header, LRESULT *result)
{
	NMLVDISPINFO *info = (NMLVDISPINFO *)header;

	m_cellText[0] = 0;

	if (info->item.mask & LVIF_TEXT)
	{
		const uint32 row	= (uint32)info->item.iItem;
		const int	 column = info->item.iSubItem;

		if (column >= 0 && column < (int)m_columns.size())
		{
			if (column == COL_PLAYHEAD)
			{
				if (row == playheadRow())
					_tcscpy(m_cellText, _T(">"));
			}
			else if (column == COL_FRAME)
			{
				_stprintf(m_cellText, _T("%06u"), row);
			}
			else if (column == COL_TIME)
			{
				formatStamp(m_cellText, row);
			}
			else
			{
				const uint16 mask = m_columns[column].mask;
				if (mask && (VBAMovieGetInputAt(row, 0) & mask))
					_tcscpy(m_cellText, m_columns[column].label);
			}
		}

		info->item.pszText = m_cellText;
	}

	*result = 0;
}

void VbmEditor::OnListCustomDraw(NMHDR *header, LRESULT *result)
{
	NMLVCUSTOMDRAW *draw = (NMLVCUSTOMDRAW *)header;

	switch (draw->nmcd.dwDrawStage)
	{
	case CDDS_PREPAINT:
		*result = CDRF_NOTIFYITEMDRAW;
		return;

	case CDDS_ITEMPREPAINT:
	{
		const uint32 row = (uint32)draw->nmcd.dwItemSpec;
		if (row == playheadRow())
			draw->clrTextBk = kPlayheadColor;
		else if (m_marks.find(row) != m_marks.end())
			draw->clrTextBk = kMarkColor;

		*result = CDRF_NEWFONT;	 // required for clrTextBk to be used
		return;
	}

	default:
		*result = CDRF_DODEFAULT;
		return;
	}
}

void VbmEditor::repaintRow(uint32 row)
{
	if (row == NO_ROW || (int)row >= m_list.GetItemCount())
		return;

	m_list.RedrawItems((int)row, (int)row);
}

/////////////////////////////////////////////////////////////////////////////
// the frame box and the playhead

void VbmEditor::syncFrameBox()
{
	CString text;
	if (m_focusRow >= 0)
		text.Format(_T("%u"), (unsigned)m_focusRow);

	SetDlgItemText(IDC_VBM_FRAMENO, text);
}

void VbmEditor::applyFrameBox()
{
	if (!m_active || m_length == NO_ROW || m_length == 0)
	{
		setStatusRes(IDS_VBMEDITOR_NO_MOVIE);
		syncFrameBox();
		return;
	}

	CString text;
	GetDlgItemText(IDC_VBM_FRAMENO, text);
	text.Trim();

	if (text.IsEmpty())
	{
		syncFrameBox();
		return;
	}

	unsigned long row = _tcstoul(text, NULL, 10);
	if (row >= m_length)
		row = m_length - 1;	 // typing past the end means the last frame

	revealRow((uint32)row, true);
}

int VbmEditor::rowHeight()
{
	int top = m_list.GetTopIndex();
	if (top < 0)
		top = 0;

	CRect rect;
	if (m_list.GetItemRect(top, &rect, LVIR_BOUNDS))
		return max(1, (int)rect.Height());

	return dlux(11);
}

void VbmEditor::revealRow(uint32 row, bool center)
{
	const int target = (int)row;
	if (target < 0 || target >= m_list.GetItemCount())
		return;

	// A jump always lands on exactly one row: leaving a multi-selection behind
	// would make the next Delete act on frames the user is no longer looking at.
	selectOnlyRow(row);
	m_list.EnsureVisible(target, FALSE);

	if (!center)
		return;

	const int perPage = max(1, m_list.GetCountPerPage());
	const int want	  = target - perPage / 2;
	if (want <= 0)
		return;

	const int dy = (want - m_list.GetTopIndex()) * rowHeight();
	if (dy != 0)
		m_list.Scroll(CSize(0, dy));
}

void VbmEditor::followPlayhead(uint32 row)
{
	if (!m_follow || row == NO_ROW)
		return;

	const int target = (int)row;
	if (target >= m_list.GetItemCount())
		return;

	const int perPage = max(1, m_list.GetCountPerPage());
	const int top	  = m_list.GetTopIndex();

	// On screen: leave the view where it is.  Once the playhead leaves, move a
	// whole page at a time so the grid does not creep one line per frame.
	if (target >= top && target < top + perPage)
		return;

	CRect client;
	m_list.GetClientRect(&client);
	const int pageH = max(rowHeight(), client.Height() - rowHeight());

	int pages = (target - top) / perPage;
	if (pages == 0)
		pages = (target > top) ? 1 : -1;

	m_list.Scroll(CSize(0, pages * pageH));
	m_list.EnsureVisible(target, FALSE);	// insurance for a partial last page
}

void VbmEditor::OnGoToPlayhead()
{
	const uint32 head = playheadRow();
	if (head == NO_ROW)
	{
		setStatusRes(IDS_VBMEDITOR_NO_MOVIE);
		return;
	}

	revealRow(head, true);
}

/////////////////////////////////////////////////////////////////////////////
// transport

void VbmEditor::sendMainCommand(UINT id)
{
	CWnd *main = AfxGetMainWnd();
	if (!main)
		return;

	// The main window calls itself inactive while this dialog holds the focus
	// (pauseWhenInactive), and then its idle loop would not run a single frame.
	// A deliberate Play or Frame Advance from here has to say otherwise; the
	// next real activation recomputes the flag anyway.
	theApp.active = true;

	main->SendMessage(WM_COMMAND, MAKEWPARAM(id, 0), 0);

	refresh();
}

void VbmEditor::OnPlay()
{
	// File > Pause is a toggle, so it is only sent when it means "play".  The
	// buttons mirror the state, so the other one is not clickable anyway.
	if (m_active && theApp.paused)
		sendMainCommand(ID_FILE_PAUSE);
}

void VbmEditor::OnPause()
{
	if (m_active && !theApp.paused)
		sendMainCommand(ID_FILE_PAUSE);
}

void VbmEditor::OnStepFrame()
{
	if (m_active)
		sendMainCommand(ID_DEBUG_NEXTFRAME);
}

void VbmEditor::OnDrawItem(int id, LPDRAWITEMSTRUCT item)
{
	if (item->CtlType != ODT_BUTTON)
		return;

	int glyph = GLYPH_PLAY;
	switch (id)
	{
	case IDC_VBM_PLAY: glyph = GLYPH_PLAY; break;
	case IDC_VBM_PAUSE: glyph = GLYPH_PAUSE; break;
	case IDC_VBM_STEP: glyph = GLYPH_STEP; break;
	case IDC_VBM_GOTOPLAYHEAD: glyph = GLYPH_PLAYHEAD; break;
	default: return;
	}

	const bool disabled = (item->itemState & ODS_DISABLED) != 0;
	const bool pressed	= (item->itemState & ODS_SELECTED) != 0;

	CRect frame = item->rcItem;

	if (g_buttonTheme)
	{
		int state = kButtonNormal;
		if (disabled)
			state = kButtonDisabled;
		else if (pressed)
			state = kButtonPressed;

		::DrawThemeBackground(g_buttonTheme, item->hDC, kButtonPart, state, &frame, NULL);
	}
	else
	{
		UINT flags = DFCS_BUTTONPUSH;
		if (disabled)
			flags |= DFCS_INACTIVE;
		if (pressed)
			flags |= DFCS_PUSHED;

		::DrawFrameControl(item->hDC, &frame, DFC_BUTTON, flags);
	}

	CRect inner = frame;
	inner.DeflateRect(max(1, frame.Width() / 10), max(1, frame.Height() / 8));
	if (pressed)
		inner.OffsetRect(1, 1);

	CDC *dc = CDC::FromHandle(item->hDC);

	const int mode = dc->SetBkMode(TRANSPARENT);
	drawGlyph(*dc, inner, glyph, ::GetSysColor(disabled ? COLOR_GRAYTEXT : COLOR_BTNTEXT));

	if (item->itemState & ODS_FOCUS)
	{
		CRect focus = item->rcItem;
		focus.DeflateRect(1, 1);
		dc->DrawFocusRect(&focus);
	}

	dc->SetBkMode(mode);
}

void VbmEditor::OnListItemChanged(NMHDR *header, LRESULT *result)
{
	NMLISTVIEW *info = (NMLISTVIEW *)header;

	// Only the focus moves the frame box.  A Ctrl+click changes LVIS_SELECTED
	// too, and enumerating the selection here would run on every click of a
	// 100 000-frame movie for nothing.  Focus loss is ignored on purpose: when
	// focus moves from A to B the control reports A losing it first, and
	// reacting to that would blank the box for a moment.
	if ((info->uChanged & LVIF_STATE) && (info->uNewState & LVIS_FOCUSED)
	    && !(info->uOldState & LVIS_FOCUSED))
	{
		if (info->iItem != m_focusRow)
		{
			m_focusRow = info->iItem;
			syncFrameBox();
		}
	}

	*result = 0;
}

/////////////////////////////////////////////////////////////////////////////
// editing

void VbmEditor::OnListClick(NMHDR *header, LRESULT *result)
{
	*result = 0;

	// Ctrl and Shift are selection gestures; a click that changes the
	// selection must not also flip a key.
	if (GetKeyState(VK_CONTROL) < 0 || GetKeyState(VK_SHIFT) < 0)
		return;

	// A cell edit means "this one frame", so it is only offered while exactly
	// one row is selected.
	if (selectedRows().size() != 1)
		return;

	LPNMITEMACTIVATE info = (LPNMITEMACTIVATE)header;

	LVHITTESTINFO hit;
	ZeroMemory(&hit, sizeof(hit));
	hit.pt = info->ptAction;

	const int row = m_list.SubItemHitTest(&hit);
	if (row >= 0)
		toggleCell((uint32)row, hit.iSubItem);
}

void VbmEditor::toggleCell(uint32 row, int column)
{
	if (column < COL_FIRST_KEY || column >= (int)m_columns.size())
		return;

	const uint16 mask = m_columns[column].mask;
	if (!(mask & BUTTON_REGULAR_MASK))
	{
		setStatusRes(IDS_VBMEDITOR_CANNOT_EDIT);	// reset/motion: shown, not editable
		return;
	}

	if (!editable())
	{
		setStatusRes(m_readOnlyMode == 2 ? IDS_VBMEDITOR_FILE_READONLY : IDS_VBMEDITOR_CANNOT_EDIT);
		return;
	}

	// While recording the core only owns the frames it has already flushed to
	// the file; the one being recorded is not ours to change.
	if (VBAMovieIsRecording() && row >= VBAMovieGetFrameCounter())
		return;

	const uint16 value = uint16(VBAMovieGetInputAt(row, 0) ^ mask);
	if (VBAMovieSetInputAt(row, 0, value) != MOVIE_SUCCESS)
		setStatusRes(IDS_VBMEDITOR_CANNOT_EDIT);

	repaintRow(row);
}

void VbmEditor::insertRow(uint32 row)
{
	if (!canRestructure())
	{
		denyRestructure();
		return;
	}

	// At or above the playhead the shift changes what the emulator reads next;
	// below it only the recording changes.  Both are allowed, as asked.
	const bool shiftsPlayhead = (row <= VBAMovieGetFrameCounter());

	if (VBAMovieInsertFrameAt(row) != MOVIE_SUCCESS)
	{
		setStatusRes(IDS_VBMEDITOR_CANNOT_EDIT);
		return;
	}

	marksInserted(row, 1);
	refresh();

	// the new blank row is what the user wants to fill in
	selectOnlyRow(row);
	m_list.EnsureVisible((int)row, FALSE);

	CString text;
	text.Format(winResLoadString(IDS_VBMEDITOR_INSERTED), row);
	if (shiftsPlayhead)
	{
		text += _T(' ');
		text += winResLoadString(IDS_VBMEDITOR_SHIFT_WARNING);
	}
	setStatus(text);
}

bool VbmEditor::deleteRows(const std::vector<uint32> &rows)
{
	if (!canRestructure())
	{
		denyRestructure();
		return false;
	}
	if (rows.empty())
		return false;

	const uint32 first		   = rows.front();
	const bool	 shiftsPlayhead = (first <= VBAMovieGetFrameCounter());

	// One call for the whole selection: the core compacts the timeline once and
	// truncates the file once, so deleting ten thousand rows is not ten
	// thousand rewrites of the file.
	if (VBAMovieDeleteFramesAt(&rows[0], (uint32)rows.size()) != MOVIE_SUCCESS)
	{
		setStatusRes(IDS_VBMEDITOR_CANNOT_EDIT);
		return false;
	}

	marksDeleted(rows);
	refresh();

	// Leave the focus where the first deleted row used to be, which is now the
	// row that followed it - or the last row, if the tail went away.
	if (m_length != NO_ROW && m_length > 0)
	{
		const uint32 target = (first < m_length) ? first : m_length - 1;
		selectOnlyRow(target);
		m_list.EnsureVisible((int)target, FALSE);
	}

	CString text;
	if (rows.size() == 1)
		text.Format(winResLoadString(IDS_VBMEDITOR_DELETED), first);
	else
		text.Format(winResLoadString(IDS_VBMEDITOR_DELETED_MANY), (unsigned)rows.size());
	if (shiftsPlayhead)
	{
		text += _T(' ');
		text += winResLoadString(IDS_VBMEDITOR_SHIFT_WARNING);
	}
	setStatus(text);

	return true;
}

/////////////////////////////////////////////////////////////////////////////
// clipboard
//
// Copy writes the selected rows as vbmcsv text, cut is copy-then-delete, and
// paste inserts the parsed rows after the focused frame.  Nothing here touches
// the movie except through the same calls the buttons use, so a paste is just
// a batch insert with the rows read from the clipboard.

bool VbmEditor::writeClipboard(const std::wstring &text)
{
	if (!OpenClipboard())
		return false;

	bool	ok	 = false;
	HGLOBAL block = ::GlobalAlloc(GMEM_MOVEABLE, (text.size() + 1) * sizeof(wchar_t));

	::EmptyClipboard();

	if (block)
	{
		if (void *target = ::GlobalLock(block))
		{
			memcpy(target, text.c_str(), (text.size() + 1) * sizeof(wchar_t));
			::GlobalUnlock(block);

			if (::SetClipboardData(CF_UNICODETEXT, block))
			{
				ok	  = true;
				block = NULL;	// the clipboard owns it now
			}
		}

		if (block)
			::GlobalFree(block);
	}

	CloseClipboard();
	return ok;
}

VbmEditor::Clip VbmEditor::readClipboard(std::wstring &text)
{
	text.clear();

	if (!OpenClipboard())
		return Clip::Unavailable;

	Clip result = Clip::Unavailable;

	// The character limit is checked before anything is copied, so a runaway
	// clipboard cannot be turned into a runaway allocation.
	const size_t maxChars = size_t(vbmcsv::MAX_CHARS);

	if (HANDLE handle = ::GetClipboardData(CF_UNICODETEXT))
	{
		const size_t bytes = ::GlobalSize(handle);
		const size_t limit = bytes / sizeof(wchar_t);

		if (limit > maxChars)
			result = Clip::TooLarge;
		else if (const wchar_t *source = (const wchar_t *)::GlobalLock(handle))
		{
			size_t length = 0;
			while (length < limit && source[length] != 0)
				++length;

			text.assign(source, length);
			::GlobalUnlock(handle);
			result = Clip::Ok;
		}
	}
	else if (HANDLE ansi = ::GetClipboardData(CF_TEXT))
	{
		// Older tools still put plain text on the clipboard; the core reads
		// its strings in the process code page, so this is the same door.
		const size_t bytes = ::GlobalSize(ansi);
		if (bytes > maxChars)
			result = Clip::TooLarge;
		else if (const char *source = (const char *)::GlobalLock(ansi))
		{
			text = winWideFromAnsi(source);
			::GlobalUnlock(ansi);
			result = Clip::Ok;
		}
	}

	CloseClipboard();
	return result;
}

void VbmEditor::refreshClipboard()
{
	// GetClipboardSequenceNumber() changes whenever any program writes to the
	// clipboard, so it is exactly the cache key this needs.
	const DWORD sequence = ::GetClipboardSequenceNumber();
	if (sequence == m_clipSequence)
		return;

	m_clipSequence = sequence;
	m_clipResult   = vbmcsv::Result::Empty;
	m_clipBadLine  = 0;
	m_clipButtons.clear();

	std::wstring text;
	switch (readClipboard(text))
	{
	case Clip::Ok:
		m_clipResult = vbmcsv::parse(text, m_clipButtons, &m_clipBadLine);
		break;
	case Clip::TooLarge:
		m_clipResult = vbmcsv::Result::TooLarge;
		break;
	default:
		break;	// Unavailable: nothing to paste, and nothing to report yet
	}
}

void VbmEditor::reportClipboardFailure()
{
	switch (m_clipResult)
	{
	case vbmcsv::Result::TooLarge:
	case vbmcsv::Result::TooManyRows:
		setStatusRes(IDS_VBMEDITOR_CLIPBOARD_LARGE);
		return;
	default:
		break;
	}

	// A line number is the only thing that makes a rejected paste actionable:
	// without it the user is looking for one bad field in five hundred rows.
	if (m_clipBadLine > 0)
	{
		CString text;
		text.Format(winResLoadString(IDS_VBMEDITOR_CLIPBOARD_LINE), m_clipBadLine);
		setStatus(text);
	}
	else
		setStatusRes(IDS_VBMEDITOR_CLIPBOARD_BAD);
}

void VbmEditor::copySelection(bool cut)
{
	const std::vector<uint32> rows = selectedRows();
	if (rows.empty())
	{
		setStatusRes(IDS_VBMEDITOR_NEED_SELECTION);
		return;
	}

	// Cut destroys rows, so it has to be refused before the clipboard is
	// touched; copy is always allowed, read-only movie or not.
	if (cut && !canRestructure())
	{
		denyRestructure();
		return;
	}

	std::vector<vbmcsv::Row> data;
	data.reserve(rows.size());
	for (size_t i = 0; i < rows.size(); ++i)
	{
		vbmcsv::Row row;
		row.frame	= rows[i];
		row.buttons = VBAMovieGetInputAt(rows[i], 0);
		data.push_back(row);
	}

	std::wstring text;
	if (vbmcsv::serialize(data, text) != vbmcsv::Result::Ok || !writeClipboard(text))
	{
		setStatusRes(IDS_VBMEDITOR_CLIPBOARD_BAD);
		return;
	}

	if (!cut)
	{
		CString message;
		message.Format(winResLoadString(IDS_VBMEDITOR_COPIED), (unsigned)rows.size());
		setStatus(message);
		return;
	}

	// Cut only reports success once the rows are actually gone; if the delete
	// is refused the clipboard keeps the data and the status says why.
	const size_t count = rows.size();
	if (!deleteRows(rows))
		return;

	CString message;
	message.Format(winResLoadString(IDS_VBMEDITOR_CUT_DONE), (unsigned)count);
	setStatus(message);
}

void VbmEditor::paste()
{
	const std::vector<uint32> rows = selectedRows();
	if (rows.size() != 1)
	{
		// Paste has to know where to put the rows, and "the whole selection"
		// is not a place.
		setStatusRes(IDS_VBMEDITOR_NEED_ONE_ROW);
		return;
	}

	if (!canRestructure())
	{
		denyRestructure();
		return;
	}

	refreshClipboard();
	if (m_clipResult != vbmcsv::Result::Ok)
	{
		reportClipboardFailure();
		return;
	}

	if (m_length == NO_ROW || m_length == 0)
	{
		setStatusRes(IDS_VBMEDITOR_NO_MOVIE);
		return;
	}

	if (m_clipButtons.size() > size_t(MOVIE_MAX_FRAMES - m_length))
	{
		setStatusRes(IDS_VBMEDITOR_PASTE_FULL);
		return;
	}

	const uint32 anchor = rows.front();
	const uint32 count	= (uint32)m_clipButtons.size();

	if (VBAMovieInsertFramesAt(anchor + 1, &m_clipButtons[0], count) != MOVIE_SUCCESS)
	{
		setStatusRes(IDS_VBMEDITOR_CANNOT_EDIT);
		return;
	}

	marksInserted(anchor + 1, count);
	refresh();

	const uint32 first = anchor + 1;
	if (m_length != NO_ROW && first < m_length)
	{
		selectOnlyRow(first);
		m_list.EnsureVisible((int)first, FALSE);
	}

	CString message;
	message.Format(winResLoadString(IDS_VBMEDITOR_PASTED), (unsigned)count, anchor);
	if (anchor <= VBAMovieGetFrameCounter())
	{
		message += _T(' ');
		message += winResLoadString(IDS_VBMEDITOR_SHIFT_WARNING);
	}
	setStatus(message);
}

void VbmEditor::OnCut()
{
	copySelection(true);
}

void VbmEditor::OnCopy()
{
	copySelection(false);
}

void VbmEditor::OnPaste()
{
	paste();
}

/////////////////////////////////////////////////////////////////////////////
// marks

void VbmEditor::marksInserted(uint32 at, uint32 count)
{
	// Every mark at or below the insertion point keeps its frame; everything
	// from there on moved down by count.
	std::set<uint32> moved;
	for (std::set<uint32>::const_iterator it = m_marks.begin(); it != m_marks.end(); ++it)
		moved.insert(*it >= at ? *it + count : *it);

	m_marks.swap(moved);
}

void VbmEditor::marksDeleted(const std::vector<uint32> &rows)
{
	// rows is ascending and unique, so one walk over both sets is enough: the
	// number of deleted rows below a mark is exactly how far that mark moved.
	std::set<uint32> moved;
	size_t			 removed = 0;

	for (std::set<uint32>::const_iterator it = m_marks.begin(); it != m_marks.end(); ++it)
	{
		while (removed < rows.size() && rows[removed] < *it)
			++removed;

		if (removed < rows.size() && rows[removed] == *it)
			continue;	// the marked frame itself is gone

		moved.insert(*it - (uint32)removed);
	}

	m_marks.swap(moved);
}

void VbmEditor::toggleMarkSelection()
{
	std::vector<uint32> rows = selectedRows();
	if (rows.empty())
	{
		// A click on a cell focuses the row without necessarily selecting it,
		// and the keyboard can act on that row; fall back to it so M still
		// works on the row the user is looking at.
		const int focus = focusedRow();
		if (focus < 0)
		{
			setStatusRes(IDS_VBMEDITOR_NEED_SELECTION);
			return;
		}
		rows.push_back((uint32)focus);
	}

	// One entry, two meanings: a selection that is already marked as a whole is
	// what the user wants to clear.  Anything else marks all of it, so a mixed
	// selection converges on "all marked" instead of flipping row by row.
	bool allMarked = true;
	for (size_t i = 0; i < rows.size() && allMarked; ++i)
	{
		if (m_marks.find(rows[i]) == m_marks.end())
			allMarked = false;
	}

	for (size_t i = 0; i < rows.size(); ++i)
	{
		if (allMarked)
			m_marks.erase(rows[i]);
		else
			m_marks.insert(rows[i]);
	}

	// The rows are ascending and all of them changed, so one band repaint does
	// the whole action - RedrawItems per row would be one message per frame of
	// a large selection.
	m_list.RedrawItems((int)rows.front(), (int)rows.back());
	updateControls();

	CString text;
	if (rows.size() == 1)
	{
		text.Format(winResLoadString(allMarked ? IDS_VBMEDITOR_UNMARKED : IDS_VBMEDITOR_MARKED),
		            rows.front());
	}
	else
	{
		text.Format(winResLoadString(allMarked ? IDS_VBMEDITOR_UNMARKED_MANY
		                                       : IDS_VBMEDITOR_MARKED_MANY),
		            (unsigned)rows.size());
	}
	setStatus(text);
}

void VbmEditor::gotoMark(uint32 row)
{
	if (row == NO_ROW || (int)row >= m_list.GetItemCount())
		return;

	revealRow(row, true);

	CString text;
	text.Format(winResLoadString(IDS_VBMEDITOR_MARKED), row);
	setStatus(text);
}

void VbmEditor::gotoNeighbourMark(int direction)
{
	if (m_marks.empty())
	{
		setStatusRes(IDS_VBMEDITOR_NO_MARKS);
		return;
	}

	// "next" means next after the row the user is looking at, which is the
	// focused one - not the first row of a multi-selection.
	const int	 focused = focusedRow();
	const uint32 start	 = (focused >= 0) ? (uint32)focused : playheadRow();

	uint32 found = NO_ROW;
	if (start == NO_ROW)
	{
		found = (direction > 0) ? *m_marks.begin() : *m_marks.rbegin();
	}
	else if (direction > 0)
	{
		std::set<uint32>::const_iterator it = m_marks.upper_bound(start);
		if (it != m_marks.end())
			found = *it;
	}
	else
	{
		std::set<uint32>::const_iterator it = m_marks.lower_bound(start);
		if (it != m_marks.begin())
		{
			--it;
			found = *it;
		}
	}

	if (found == NO_ROW)
		setStatusRes(IDS_VBMEDITOR_NO_MARKS);
	else
		gotoMark(found);
}

void VbmEditor::OnPrevMark()
{
	gotoNeighbourMark(-1);
}

void VbmEditor::OnNextMark()
{
	gotoNeighbourMark(+1);
}

void VbmEditor::OnListKeyDown(NMHDR *header, LRESULT *result)
{
	NMLVKEYDOWN *key = (NMLVKEYDOWN *)header;

	switch (key->wVKey)
	{
	case VK_DELETE:
	{
		const std::vector<uint32> rows = selectedRows();
		if (!rows.empty())
			deleteRows(rows);
		break;
	}

	case VK_INSERT:
	{
		// Insert before the focused row.  With several rows selected there is
		// no single "before", so the key does nothing but say so.
		const std::vector<uint32> rows = selectedRows();
		if (rows.size() != 1)
			setStatusRes(IDS_VBMEDITOR_NEED_ONE_ROW);
		else
			insertRow(rows.front());
		break;
	}

	case VK_F2:
		gotoNeighbourMark(GetKeyState(VK_SHIFT) < 0 ? -1 : +1);
		break;

	default:
		break;
	}

	*result = 0;
}

/////////////////////////////////////////////////////////////////////////////
// context menu

void VbmEditor::OnListRightClick(NMHDR *header, LRESULT *result)
{
	LPNMITEMACTIVATE info = (LPNMITEMACTIVATE)header;

	LVHITTESTINFO hit;
	ZeroMemory(&hit, sizeof(hit));
	hit.pt = info->ptAction;

	const int row = m_list.SubItemHitTest(&hit);
	if (row < 0)
	{
		*result = 0;
		return;
	}

	// Inside the selection the multi-selection survives (the menu acts on all
	// of it) and only the focus moves; outside it, the selection collapses to
	// the row under the cursor.
	if (m_list.GetItemState(row, LVIS_SELECTED) & LVIS_SELECTED)
	{
		m_list.SetItemState(row, LVIS_FOCUSED, LVIS_FOCUSED);
		if (row != m_focusRow)
		{
			m_focusRow = row;
			syncFrameBox();
		}
	}
	else
	{
		selectOnlyRow((uint32)row);
	}

	showContextMenu(CPoint(info->ptAction), (uint32)row);

	*result = 0;
}

void VbmEditor::showContextMenu(CPoint point, uint32 row)
{
	const bool				 restructurable = canRestructure();
	const std::vector<uint32> selection	 = selectedRows();
	// Inserting a row needs one unambiguous place to put it; deleting works on
	// the whole selection.
	const bool single = (selection.size() == 1);
	const bool any	  = !selection.empty();

	// The mark entry has two faces: it offers to clear the marks when every
	// selected row already has one, and to mark them otherwise.  A mixed
	// selection therefore offers "mark", which is also what the command does.
	// With nothing selected the entry is about the right-clicked row.
	bool allMarked = any;
	for (size_t i = 0; i < selection.size() && allMarked; ++i)
	{
		if (m_marks.find(selection[i]) == m_marks.end())
			allMarked = false;
	}
	if (!any)
		allMarked = m_marks.find(row) != m_marks.end();

	// Paste is only offered when it would actually work, which needs the
	// clipboard to be read and parsed - once per change, see refreshClipboard().
	refreshClipboard();
	const bool canPaste = restructurable && single && m_clipResult == vbmcsv::Result::Ok
	                   && m_length != NO_ROW && m_length != 0
	                   && m_clipButtons.size() <= size_t(MOVIE_MAX_FRAMES - m_length);

	CMenu menu;
	menu.CreatePopupMenu();

	menu.AppendMenu(MF_STRING, CMD_MARK,
	                winResLoadString(allMarked ? IDS_VBMEDITOR_UNMARK : IDS_VBMEDITOR_MARK));
	menu.AppendMenu(MF_SEPARATOR);
	menu.AppendMenu(MF_STRING | (restructurable && single ? 0 : MF_GRAYED), CMD_INSERT,
	                winResLoadString(IDS_VBMEDITOR_INSERT_ROW));
	menu.AppendMenu(MF_STRING | (restructurable && single ? 0 : MF_GRAYED), CMD_INSERT_AFTER,
	                winResLoadString(IDS_VBMEDITOR_INSERT_ROW_AFTER));
	menu.AppendMenu(MF_STRING | (restructurable && any ? 0 : MF_GRAYED), CMD_DELETE,
	                winResLoadString(IDS_VBMEDITOR_DELETE_ROW));
	menu.AppendMenu(MF_SEPARATOR);
	menu.AppendMenu(MF_STRING | (restructurable && any ? 0 : MF_GRAYED), CMD_CUT,
	                winResLoadString(IDS_VBMEDITOR_CUT_MENU));
	menu.AppendMenu(MF_STRING | (any ? 0 : MF_GRAYED), CMD_COPY,
	                winResLoadString(IDS_VBMEDITOR_COPY_MENU));
	menu.AppendMenu(MF_STRING | (canPaste ? 0 : MF_GRAYED), CMD_PASTE,
	                winResLoadString(IDS_VBMEDITOR_PASTE_MENU));
	menu.AppendMenu(MF_SEPARATOR);
	menu.AppendMenu(MF_STRING, CMD_PREV, winResLoadString(IDS_VBMEDITOR_GOTO_PREV));
	menu.AppendMenu(MF_STRING, CMD_NEXT, winResLoadString(IDS_VBMEDITOR_GOTO_NEXT));
	menu.AppendMenu(MF_STRING | (m_marks.empty() ? MF_GRAYED : 0), CMD_CLEAR,
	                winResLoadString(IDS_VBMEDITOR_CLEAR_MARKS));

	ClientToScreen(&point);

	int command = menu.TrackPopupMenu(TPM_LEFTALIGN | TPM_RETURNCMD | TPM_NONOTIFY,
	                                  point.x, point.y, this);

	switch (command)
	{
	case CMD_MARK:
		toggleMarkSelection();
		break;
	case CMD_INSERT:
		insertRow(row);
		break;
	case CMD_INSERT_AFTER:
		insertRow(row + 1);	// past the last row this appends, as it should
		break;
	case CMD_DELETE:
		deleteRows(selection);
		break;
	case CMD_CUT:
		copySelection(true);
		break;
	case CMD_COPY:
		copySelection(false);
		break;
	case CMD_PASTE:
		paste();
		break;
	case CMD_PREV:
		gotoNeighbourMark(-1);
		break;
	case CMD_NEXT:
		gotoNeighbourMark(+1);
		break;
	case CMD_CLEAR:
		m_marks.clear();
		m_list.Invalidate(FALSE);
		updateControls();
		break;
	default:
		break;
	}
}

/////////////////////////////////////////////////////////////////////////////
// checkboxes and status line

void VbmEditor::OnReadOnlyClicked()
{
	m_editLock = (IsDlgButtonChecked(IDC_VBM_READONLY) == BST_CHECKED);

	updateControls();
	refresh();
}

void VbmEditor::OnFollowClicked()
{
	m_follow = (IsDlgButtonChecked(IDC_VBM_FOLLOW) == BST_CHECKED);
}

void VbmEditor::setStatus(const CString &text)
{
	if (text == m_statusText)
		return;

	m_statusText = text;
	SetDlgItemText(IDC_VBM_STATUS, text);
}

void VbmEditor::setStatusRes(UINT id)
{
	// keep m_statusText in step, otherwise a later setStatus() with the same
	// text would think the line is already there and leave the resource
	// string on screen
	setStatus(winResLoadString(id));
}
