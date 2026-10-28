#if !defined(AFX_VBMEDITOR_H__4E1B7C60_9A3D_4C21_B0F5_6D8A2E17C934__INCLUDED_)
#define AFX_VBMEDITOR_H__4E1B7C60_9A3D_4C21_B0F5_6D8A2E17C934__INCLUDED_

#pragma once

#include "../resource.h"

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
	afx_msg LRESULT OnDpiChanged(WPARAM wParam, LPARAM lParam);
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

	bool   editable() const;		// may the grid change the movie right now?
	uint32 playheadRow() const;		// row the arrow belongs on, or NO_ROW
	int	   selectedRow();

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
	void deleteRow(uint32 row);
	void toggleMark(uint32 row);
	void gotoMark(uint32 row);
	void gotoNeighbourMark(int direction);
	void showContextMenu(CPoint point, uint32 row);

	enum { NO_ROW = 0xFFFFFFFFu };
	enum { TIMER_REFRESH = 1 };
	enum { REFRESH_MS = 50 };
	enum { CMD_MARK = 1, CMD_INSERT, CMD_DELETE, CMD_PREV, CMD_NEXT, CMD_CLEAR };

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
	int m_selectionRow;	// row the frame box is showing, or -1

	CString m_titleBase;	// the template caption, before the file name
	CString m_statusText;
	TCHAR	m_cellText[24];
};

#endif // !defined(AFX_VBMEDITOR_H__4E1B7C60_9A3D_4C21_B0F5_6D8A2E17C934__INCLUDED_)
