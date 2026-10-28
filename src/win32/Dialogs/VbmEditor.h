#if !defined(AFX_VBMEDITOR_H__4E1B7C60_9A3D_4C21_B0F5_6D8A2E17C934__INCLUDED_)
#define AFX_VBMEDITOR_H__4E1B7C60_9A3D_4C21_B0F5_6D8A2E17C934__INCLUDED_

#pragma once

#include "../resource.h"
#include "VbmFrameCsv.h"

#include <set>
#include <vector>

// Tools > Movie > VBM Editor: the recorded key sequence as a grid of frames.
//
// Rows are frames, columns are the pad buttons the on-screen display already
// uses ('^', 'v', '<', '>' ...).  The window is a view over the movie's
// timeline: it reads the input buffer and writes back only the ten pad bits of
// one frame at a time.  It never touches the emulator, the movie state, the
// edit mode or the read-only flag - see the comment block above
// VBAMovieSetInputAt() in common/movie.cpp for the exact contract.
//
// The list is a virtual list (LVS_OWNERDATA): only the rows on screen ever ask
// for their text, so a two-hour movie costs exactly as much as a two-second
// one and nothing here grows with the frame count.
//
// It is also multi-select, but the selection is only a *target set* for the row
// commands (delete, cut/copy, mark): a plain click on a cell still flips one
// key of one frame, and Ctrl/Shift are treated as selection gestures, never as
// a bulk edit.  The frame box always shows the focused row.
//
// The transport buttons at the top are not a second player: they forward the
// File menu's own Pause and Frame Advance commands to the main window, so the
// emulator keeps exactly one playback path.  The only thing this window has to
// add is theApp.active, because the main window marks itself inactive while
// this dialog has the focus (pauseWhenInactive).
class VbmEditor : public CDialog
{
public:
	enum { IDD = IDD_VBM_EDITOR };

	explicit VbmEditor(CWnd *parent);

	// Opens the editor, or brings the existing one to the front.
	static void open(CWnd *parent);

protected:
	virtual void DoDataExchange(CDataExchange *pDX);
	virtual BOOL OnInitDialog();
	virtual void PostNcDestroy();
	virtual BOOL PreTranslateMessage(MSG *msg);
	virtual void OnOK();
	virtual void OnCancel();

	afx_msg void OnDestroy();
	afx_msg void OnTimer(UINT_PTR event);
	afx_msg void OnSize(UINT type, int cx, int cy);
	afx_msg void OnGetMinMaxInfo(MINMAXINFO *info);
	afx_msg void OnReadOnlyClicked();
	afx_msg void OnFollowClicked();
	afx_msg void OnPrevMark();
	afx_msg void OnNextMark();
	afx_msg void OnPlay();
	afx_msg void OnPause();
	afx_msg void OnStepFrame();
	afx_msg void OnGoToPlayhead();
	afx_msg void OnDrawItem(int id, LPDRAWITEMSTRUCT item);
	afx_msg void OnListGetDispInfo(NMHDR *header, LRESULT *result);
	afx_msg void OnListCustomDraw(NMHDR *header, LRESULT *result);
	afx_msg void OnListItemChanged(NMHDR *header, LRESULT *result);
	afx_msg void OnListClick(NMHDR *header, LRESULT *result);
	afx_msg void OnListRightClick(NMHDR *header, LRESULT *result);
	afx_msg void OnListKeyDown(NMHDR *header, LRESULT *result);
	afx_msg void OnCut();
	afx_msg void OnCopy();
	afx_msg void OnPaste();

	DECLARE_MESSAGE_MAP()

private:
	// One grid column.  mask is 0 for the two fixed columns (playhead marker
	// and frame number); label is the character the OSD shows for that bit.
	struct Column
	{
		uint16 mask;
		TCHAR  label[8];
		int	   width;
	};

	void refresh();					// the 50 ms state machine
	void resetView();				// a different movie took over the window
	void rebuildColumns(uint16 usedMask);
	void fitColumns();				// widths from the list's own font and client area
	void updateControls();
	void updateTitle();

	// The window is resizable: the grid takes every pixel that is added or
	// removed, the hint and status lines are glued to the bottom edge, and the
	// right-hand half of the top row follows the right edge.  The snapshot is
	// read back from the live controls, so a DPI change only has to take a new
	// one instead of scaling numbers that are already in pixels.
	struct Layout
	{
		CSize client;
		CRect list;
		CRect hint;
		CRect status;
		CRect gotoLabel;
		CRect frameBox;
		CRect prevMark;
		CRect nextMark;
	};

	void  captureLayout();
	void  layoutChildren(int cx, int cy);
	void  updateHeightFloor();		// shortest window that still shows the column header
	int   listHeaderHeight() const;
	CRect controlRect(int id) const;
	void  moveControl(int id, const CRect &rect);

	bool   editable() const;		// may the grid change the movie right now?
	bool   canRestructure() const;	// may rows be inserted or deleted right now?
	void   denyRestructure();		// say on the status line why they may not
	uint32 playheadRow() const;		// row the arrow belongs on, or NO_ROW

	// Selection.  The frame box and the keyboard act on the *focused* row, so
	// it is cached instead of asked for: GetNextItem(LVNI_FOCUSED) walks the
	// list, and the 50 ms tick must not depend on the frame count.  Only
	// LVN_ITEMCHANGED and the jumps in this file write it.
	int	 focusedRow() const;
	std::vector<uint32> selectedRows() const;	// ascending, empty when nothing is selected
	void selectOnlyRow(uint32 row);				// drop any multi-selection first

	void setStatus(const CString &text);
	void setStatusRes(UINT id);
	void repaintRow(uint32 row);

	// playhead navigation and the frame box
	void followPlayhead(uint32 row);		// page-scrolls when it leaves the view
	void revealRow(uint32 row, bool center);
	void syncFrameBox();					// copy the selection into the edit box
	void applyFrameBox();					// the box was confirmed with Enter
	int	 rowHeight();

	// transport: forwards the File menu's own commands to the main window
	void sendMainCommand(UINT id);

	int	 dlux(int dlu) const;		// dialog units -> pixels, for column widths

	void toggleCell(uint32 row, int column);
	void insertRow(uint32 row);
	bool deleteRows(const std::vector<uint32> &rows);	// false when refused
	void toggleMarkSelection();		// mark (or unmark, when all are marked) every selected row
	void gotoMark(uint32 row);
	void gotoNeighbourMark(int direction);
	void showContextMenu(CPoint point, uint32 row);

	// The marks follow the timeline through an insert or a delete.
	void marksInserted(uint32 at, uint32 count);
	void marksDeleted(const std::vector<uint32> &rows);

	// Clipboard.  The format is vbmcsv's, so a block of frames can go through
	// Notepad and come back.  readClipboard() also has to say *why* it failed,
	// because "cannot paste" and "that is too big to paste" are different
	// things to tell the user.
	enum class Clip { Ok, Unavailable, TooLarge };

	bool writeClipboard(const std::wstring &text);
	Clip readClipboard(std::wstring &text);
	void refreshClipboard();	// read + parse, once per clipboard change
	void copySelection(bool cut);
	void paste();
	void reportClipboardFailure();

	enum { NO_ROW = 0xFFFFFFFFu };
	enum { TIMER_REFRESH = 1 };
	enum { REFRESH_MS = 50 };
	enum
	{
		CMD_MARK = 1,
		CMD_INSERT,
		CMD_INSERT_AFTER,
		CMD_DELETE,
		CMD_CUT,
		CMD_COPY,
		CMD_PASTE,
		CMD_PREV,
		CMD_NEXT,
		CMD_CLEAR
	};

	std::vector<Column> m_columns;
	CListCtrl			m_list;
	CToolTipCtrl		m_tip;
	std::set<uint32>	m_marks;

	// Last seen movie state; every tick is diffed against this.
	CString m_identity;
	bool	m_active;
	int		m_readOnlyMode;
	uint32	m_state;
	uint32	m_length;
	uint32	m_frame;
	uint32	m_generation;
	uint16	m_usedMask;

	bool m_editLock;	// the "Read-only" checkbox this window owns
	bool m_follow;

	// What updateControls() applied last time, so the tick leaves the controls
	// alone when nothing changed (calling EnableWindow twenty times a second on
	// an owner-drawn button makes it flicker).  m_uiChecks packs the read-only
	// mark in bit 0, the follow mark in bit 1 and the read-only box's enabled
	// state in bit 2: the mark has two sources (movie lock, this window's own
	// lock) while only one of them greys the box out, so the enabled state is
	// not derivable from the marks.
	int m_uiChecks;
	int m_uiEnable;
	int m_focusRow;		// row the frame box is showing, or -1

	// Resizing.  m_layout is the last geometry read back from the controls
	// (taken when the dialog opens and after a DPI change); every WM_SIZE is
	// applied as a delta against it.  m_minTrack is the smallest window the
	// frame allows: the template width (the top row and the columns have
	// nowhere to go sideways) by the height that leaves the grid its header
	// and nothing more.  m_fixedHeight is everything in the window that is not
	// the grid - caption, transport row, checkboxes, the two bottom lines - so
	// the floor is that plus one header, and it is measured rather than
	// computed from dialog units so a DPI change only needs a new snapshot.
	// m_fittedWidth and m_fittedFont are what the columns were last measured
	// for: a vertical drag and an unchanged font do not have to run
	// fitColumns() again.
	Layout m_layout;
	UINT   m_layoutDpi;
	CSize  m_minTrack;
	int	   m_fixedHeight;
	int	   m_fittedWidth;
	HFONT  m_fittedFont;

	// What the clipboard held the last time it was looked at.  Building the
	// context menu needs to know whether Paste is possible, and the command
	// must not parse ten megabytes a second time, so the result is kept here
	// and keyed by the clipboard sequence number - which changes whenever any
	// program writes to the clipboard.
	DWORD				 m_clipSequence;
	vbmcsv::Result		 m_clipResult;
	int					 m_clipBadLine;
	std::vector<uint16>	 m_clipButtons;

	CString m_titleBase;	// the template caption, before the file name
	CString m_statusText;
	TCHAR	m_cellText[24];
};

#endif // !defined(AFX_VBMEDITOR_H__4E1B7C60_9A3D_4C21_B0F5_6D8A2E17C934__INCLUDED_)
