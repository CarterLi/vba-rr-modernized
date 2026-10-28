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

#ifndef WM_DPICHANGED
#	define WM_DPICHANGED 0x02E0			// Vista headers do not have it
#endif

namespace
{

VbmEditor *g_editor = NULL;
HTHEME	   g_buttonTheme = NULL;

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
	, m_selectionRow(-1)
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
ON_WM_DRAWITEM()
ON_MESSAGE(WM_DPICHANGED, OnDpiChanged)
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
END_MESSAGE_MAP()

BOOL VbmEditor::OnInitDialog()
{
	CDialog::OnInitDialog();

	GetWindowText(m_titleBase);

	// A virtual list keeps no per-item state, which is the whole point: a
	// two-hour movie is one SetItemCountEx() and nothing else.
	m_list.SetExtendedStyle(m_list.GetExtendedStyle() | LVS_EX_FULLROWSELECT
	                        | LVS_EX_GRIDLINES | LVS_EX_DOUBLEBUFFER);

	SetDlgItemText(IDC_VBM_HINT, winResLoadString(IDS_VBMEDITOR_HINT));

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

LRESULT VbmEditor::OnDpiChanged(WPARAM, LPARAM)
{
	// By the time this arrives UiFont.cpp has already rescaled the controls
	// (see its dialogProc).  The column widths are plain pixels and were fitted
	// to the old client area, so they are measured again for the new one.
	m_list.SetRedraw(FALSE);
	fitColumns();
	m_list.SetRedraw(TRUE);
	m_list.Invalidate(FALSE);
	return 0;
}

/////////////////////////////////////////////////////////////////////////////
// state

void VbmEditor::refresh()
{
	const uint32 oldHead = playheadRow();

	// Whatever selected the row - a click, the context menu, a jump - the frame
	// box shows the selection, so it is re-read here rather than in each place
	// that moves it.
	const int selection = selectedRow();
	if (selection != m_selectionRow)
	{
		m_selectionRow = selection;
		syncFrameBox();
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
		m_selectionRow = -1;

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

	m_frame = frame;

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
	m_generation   = 0xFFFFFFFFu;
	m_usedMask	   = 0;
	m_length	   = NO_ROW;
	m_frame		   = NO_ROW;
	m_selectionRow = -1;

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

	m_list.SetRedraw(TRUE);
	m_list.Invalidate(FALSE);
}

void VbmEditor::fitColumns()
{
	const int count = (int)m_columns.size();
	if (count <= COL_FIRST_KEY)
		return;

	CSize digit(6, 12), frameHeader(30, 12), stamp(60, 12), timeHeader(20, 12);
	int	  pad = 6;

	CDC *dc = m_list.GetDC();
	if (dc)
	{
		CFont	  *font = m_list.GetFont();
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

uint32 VbmEditor::playheadRow() const
{
	if (!m_active || m_length == 0 || m_length == NO_ROW)
		return NO_ROW;

	// While recording, currentFrame is the frame that is about to run and does
	// not exist in the list yet, so the last recorded row is the playhead.
	return (m_frame < m_length) ? m_frame : m_length - 1;
}

int VbmEditor::selectedRow()
{
	POSITION pos = m_list.GetFirstSelectedItemPosition();
	if (!pos)
		return -1;

	return m_list.GetNextSelectedItem(pos);
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
	if (m_selectionRow >= 0)
		text.Format(_T("%u"), (unsigned)m_selectionRow);

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
	m_selectionRow = (int)row;
	syncFrameBox();
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

	m_list.SetItemState(target, LVIS_SELECTED | LVIS_FOCUSED, LVIS_SELECTED | LVIS_FOCUSED);
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

	m_selectionRow = (int)head;
	syncFrameBox();
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

	if ((info->uChanged & LVIF_STATE) && ((info->uNewState ^ info->uOldState) & LVIS_SELECTED))
	{
		m_selectionRow = selectedRow();
		syncFrameBox();
	}

	*result = 0;
}

/////////////////////////////////////////////////////////////////////////////
// editing

void VbmEditor::OnListClick(NMHDR *header, LRESULT *result)
{
	LPNMITEMACTIVATE info = (LPNMITEMACTIVATE)header;

	LVHITTESTINFO hit;
	ZeroMemory(&hit, sizeof(hit));
	hit.pt = info->ptAction;

	const int row = m_list.SubItemHitTest(&hit);
	if (row >= 0)
		toggleCell((uint32)row, hit.iSubItem);

	*result = 0;
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
	if (!editable())
	{
		setStatusRes(m_readOnlyMode == 2 ? IDS_VBMEDITOR_FILE_READONLY : IDS_VBMEDITOR_CANNOT_EDIT);
		return;
	}
	if (VBAMovieIsRecording())
	{
		setStatusRes(IDS_VBMEDITOR_RECORD_STATE);
		return;
	}
	if (m_state == MOVIE_STATE_END)
	{
		setStatusRes(IDS_VBMEDITOR_END_STATE);
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

	std::set<uint32> moved;
	for (std::set<uint32>::const_iterator it = m_marks.begin(); it != m_marks.end(); ++it)
		moved.insert(*it >= row ? *it + 1 : *it);
	m_marks.swap(moved);

	refresh();

	CString text;
	text.Format(winResLoadString(IDS_VBMEDITOR_INSERTED), row);
	if (shiftsPlayhead)
	{
		text += _T(' ');
		text += winResLoadString(IDS_VBMEDITOR_SHIFT_WARNING);
	}
	setStatus(text);
}

void VbmEditor::deleteRow(uint32 row)
{
	if (!editable())
	{
		setStatusRes(m_readOnlyMode == 2 ? IDS_VBMEDITOR_FILE_READONLY : IDS_VBMEDITOR_CANNOT_EDIT);
		return;
	}
	if (VBAMovieIsRecording())
	{
		setStatusRes(IDS_VBMEDITOR_RECORD_STATE);
		return;
	}
	if (m_state == MOVIE_STATE_END)
	{
		setStatusRes(IDS_VBMEDITOR_END_STATE);
		return;
	}

	const bool shiftsPlayhead = (row <= VBAMovieGetFrameCounter());

	if (VBAMovieDeleteFrameAt(row) != MOVIE_SUCCESS)
	{
		setStatusRes(IDS_VBMEDITOR_CANNOT_EDIT);
		return;
	}

	std::set<uint32> moved;
	for (std::set<uint32>::const_iterator it = m_marks.begin(); it != m_marks.end(); ++it)
	{
		if (*it == row)
			continue;	 // the marked frame itself is gone
		moved.insert(*it > row ? *it - 1 : *it);
	}
	m_marks.swap(moved);

	refresh();

	CString text;
	text.Format(winResLoadString(IDS_VBMEDITOR_DELETED), row);
	if (shiftsPlayhead)
	{
		text += _T(' ');
		text += winResLoadString(IDS_VBMEDITOR_SHIFT_WARNING);
	}
	setStatus(text);
}

/////////////////////////////////////////////////////////////////////////////
// marks

void VbmEditor::toggleMark(uint32 row)
{
	CString text;

	if (m_marks.erase(row))
	{
		text.Format(winResLoadString(IDS_VBMEDITOR_UNMARKED), row);
	}
	else
	{
		m_marks.insert(row);
		text.Format(winResLoadString(IDS_VBMEDITOR_MARKED), row);
	}

	repaintRow(row);
	updateControls();
	setStatus(text);
}

void VbmEditor::gotoMark(uint32 row)
{
	if (row == NO_ROW || (int)row >= m_list.GetItemCount())
		return;

	revealRow(row, true);

	m_selectionRow = (int)row;
	syncFrameBox();

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

	const int	 selected = selectedRow();
	const uint32 start	  = (selected >= 0) ? (uint32)selected : playheadRow();

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
	const int	 row = selectedRow();

	if (row >= 0)
	{
		switch (key->wVKey)
		{
		case VK_DELETE:
			deleteRow((uint32)row);
			break;
		case VK_INSERT:
			insertRow((uint32)row);
			break;
		case 'M':
			toggleMark((uint32)row);
			break;
		case VK_F2:
			gotoNeighbourMark(GetKeyState(VK_SHIFT) < 0 ? -1 : +1);
			break;
		}
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
	if (row >= 0)
	{
		m_list.SetItemState(row, LVIS_SELECTED | LVIS_FOCUSED, LVIS_SELECTED | LVIS_FOCUSED);
		showContextMenu(CPoint(info->ptAction), (uint32)row);
	}

	*result = 0;
}

void VbmEditor::showContextMenu(CPoint point, uint32 row)
{
	const bool marked		  = m_marks.find(row) != m_marks.end();
	const bool canRestructure = editable() && !VBAMovieIsRecording() && m_state != MOVIE_STATE_END;

	CMenu menu;
	menu.CreatePopupMenu();

	menu.AppendMenu(MF_STRING, CMD_MARK,
	                winResLoadString(marked ? IDS_VBMEDITOR_UNMARK : IDS_VBMEDITOR_MARK));
	menu.AppendMenu(MF_SEPARATOR);
	menu.AppendMenu(MF_STRING | (canRestructure ? 0 : MF_GRAYED), CMD_INSERT,
	                winResLoadString(IDS_VBMEDITOR_INSERT_ROW));
	menu.AppendMenu(MF_STRING | (canRestructure ? 0 : MF_GRAYED), CMD_DELETE,
	                winResLoadString(IDS_VBMEDITOR_DELETE_ROW));
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
		toggleMark(row);
		break;
	case CMD_INSERT:
		insertRow(row);
		break;
	case CMD_DELETE:
		deleteRow(row);
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
