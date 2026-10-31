#ifndef VBA_WIN32_TEXT_OPTIONS_H
#define VBA_WIN32_TEXT_OPTIONS_H

#pragma once

#include <map>
#include <set>
#include <vector>

// One size a game font offers.  For the fonts bundled with the executable the
// combo shows `label`, the size part of the font's own name ("4x6" under the
// "gse" family); a system face reports both numbers and is labelled with them.
// charHeight is what selects that strike in CreateFont and is the only value a
// system face needs.  `font` is the bundled font behind this strike and is
// empty when the face name alone picks it.
struct GameTextStrike
{
	int width;
	int height;
	int charHeight;
	CString label;
	CString font;
};

// TextOptions dialog

class TextOptions : public CDialog
{
	DECLARE_DYNAMIC(TextOptions)

public:
	TextOptions(CWnd* pParent = NULL);   // standard constructor
	virtual ~TextOptions();

	virtual BOOL OnInitDialog();
	virtual BOOL PreTranslateMessage(MSG* pMsg);

// Dialog Data
	enum { IDD = IDD_TEXTCONFIG };

protected:
	virtual void DoDataExchange(CDataExchange* pDX);    // DDX/DDV support

	DECLARE_MESSAGE_MAP()
public:
	afx_msg void OnBnClickedOk();
	afx_msg void OnBnClickedApply();
	afx_msg void OnBnClickedRadioPrefilter();
	afx_msg void OnBnClickedRadioPostfilter();
	afx_msg void OnBnClickedRadioPostrender();
	afx_msg void OnSelchangeFont();

private:
	void collectFonts();
	void rebuildFontList();
	void rebuildSizeList();
	void storeCurrentSelection();
	void applySettings();
	void changeMode();
	void updateFontTip();
	void updateSizeControl();
	void updateMarginControls();
	void updateApplyButton();
	void populateTooltips();

	int currentMode();
	// What the font combo shows for the current selection: a family name
	// ("gse") for the bundled fonts, a face name for a system one.
	CString selectedFamilyName();
	// The font actually used to draw: the bundled size behind the selection
	// ("gse7x11"), or the face name for a system face.
	CString selectedFontName();
	// The family a bundled font belongs to; the name itself when it is not
	// one of them.
	CString familyForFont(const CString &font);
	int selectedSize();
	int selectedDisplaySize();

	CToolTipCtrl m_toolTip;

	// The font combo shows one kind of face at a time: TrueType fixed pitch
	// faces for On Screen and raster faces (plus the built-in font) for the
	// game modes.  Both sets are collected once, when the dialog opens.
	std::set<CString> m_screenFonts;

	// The sizes each game font offers, by family name, smallest first.  Raster
	// system faces have a few fixed strikes; a bundled family has one entry per
	// size it was cut in.  The size list is rebuilt from this whenever the
	// family changes.
	std::map<CString, std::vector<GameTextStrike> > m_gameFontStrikes;

	// The list the size combo was filled from, in combo order, so that the
	// selection does not have to be read back out of the label text.
	std::vector<GameTextStrike> m_sizeList;

	// The bundled families, in the order they are listed: they are always
	// available and are shown before what the system happens to provide.
	std::vector<CString> m_bundledFamilies;

	CString m_screenFont;
	CString m_gameFont;
	int     m_screenSize;
	int     m_gameSize;
	// The game size the user sees (the rendered pixel height).  m_gameSize is
	// the GDI character height that selects that strike; the two differ by the
	// bitmap's internal leading.
	int     m_gameDisplaySize;
	// What the two margin boxes stand for.  They are the value the boxes show
	// at open and the value applySettings() stores: reading the boxes back
	// instead would find the zero the spin controls seed them with.
	int     m_marginLeft;
	int     m_marginBottom;
	int     m_currentMode;
	bool    m_listReady;
};

#endif
