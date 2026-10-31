// TextOptions.cpp : implementation file
//

#include "../stdafx.h"
#include "../../common/Text.h"
#include "../BitmapFont.h"
#include "../GameText.h"
#include "../resource.h"
#include "../VBA.h"
#include "../WinResUtil.h"
#include "TextOptions.h"

#include <cstdlib>

// TextOptions dialog

IMPLEMENT_DYNAMIC(TextOptions, CDialog)
TextOptions::TextOptions(CWnd*pParent /*=NULL*/)
	: CDialog(TextOptions::IDD, pParent)
{
	m_screenSize      = 12;
	m_gameSize        = 8;
	m_gameDisplaySize = 0;
	m_marginLeft      = 0;
	m_marginBottom    = 0;
	m_currentMode     = 1;
	m_listReady       = false;
}

TextOptions::~TextOptions()
{}

// The On Screen overlay draws with GDI, so a scalable fixed pitch face is what
// it wants.  Bitmap faces are excluded here; they have their own list for the
// In Game and On Game modes.
static int CALLBACK enumScreenFont(const LOGFONT *lf, const TEXTMETRIC *tm,
                                   DWORD fontType, LPARAM lParam)
{
	if (lf->lfFaceName[0] == _T('@'))
		return 1;

	if ((fontType & RASTER_FONTTYPE) != 0)
		return 1;

	if ((tm->tmPitchAndFamily & TMPF_FIXED_PITCH) != 0)
		return 1;

	((std::set<CString> *)lParam)->insert(CString(lf->lfFaceName));
	return 1;
}

struct GameFontFamilies
{
	std::set<CString> *names;
};

struct GameFontStrikes
{
	std::set<int> *heights;
};

// The In Game and On Game modes rasterise a bitmap face into the emulated
// frame.  Only raster fonts are offered: they are pixel fonts already, so they
// stay crisp and need no antialiasing.
static int CALLBACK enumGameFamily(const LOGFONT *lf, const TEXTMETRIC *tm,
                                   DWORD fontType, LPARAM lParam)
{
	if (lf->lfFaceName[0] == _T('@'))
		return 1;

	if ((fontType & RASTER_FONTTYPE) == 0)
		return 1;

	((GameFontFamilies *)lParam)->names->insert(CString(lf->lfFaceName));
	return 1;
}

// One call per bitmap strike of the face.  lfHeight is the character height
// GDI expects, which is tmHeight without the internal leading; that is what
// CreateFont() and the game renderer ask for.
static int CALLBACK enumGameStrike(const LOGFONT *lf, const TEXTMETRIC *tm,
                                   DWORD fontType, LPARAM lParam)
{
	if ((fontType & RASTER_FONTTYPE) == 0)
		return 1;

	const int height = tm->tmHeight - tm->tmInternalLeading;
	if (height > 0)
		((GameFontStrikes *)lParam)->heights->insert(height);

	return 1;
}

// Asks GDI which strike a character height really selects.  A family can
// advertise several character sets or near-duplicate heights, and two of them
// can map to the same bitmap; the probe keeps the size list to the strikes the
// renderer can actually reach.  Fills in what that strike renders at.
static bool probeRasterStrike(HDC dc, const CString &face, int height,
                              GameTextStrike *strike)
{
	HFONT font = CreateFontW(-height, 0, 0, 0, FW_BOLD, FALSE, FALSE, FALSE,
	                         DEFAULT_CHARSET, OUT_RASTER_PRECIS, CLIP_DEFAULT_PRECIS,
	                         NONANTIALIASED_QUALITY, DEFAULT_PITCH | FF_DONTCARE,
	                         face.GetString());
	if (font == NULL)
		return false;

	HGDIOBJ oldFont = SelectObject(dc, font);
	TEXTMETRICW tm;
	const bool ok = GetTextMetricsW(dc, &tm) != FALSE && tm.tmHeight > 0;

	if (ok)
	{
		// A fixed pitch face reports the cell width here, which is the other
		// number its own name carries ("Terminal 8x12").
		strike->width      = tm.tmAveCharWidth > 0 ? tm.tmAveCharWidth : 0;
		strike->height     = tm.tmHeight;
		strike->charHeight = height;
	}

	SelectObject(dc, oldFont);
	DeleteObject(font);
	return ok;
}

void TextOptions::collectFonts()
{
	m_screenFonts.clear();
	m_gameFontStrikes.clear();
	m_bundledFamilies.clear();

	LOGFONT lf;
	ZeroMemory(&lf, sizeof(lf));
	lf.lfCharSet        = DEFAULT_CHARSET;
	lf.lfPitchAndFamily = DEFAULT_PITCH | FF_DONTCARE;

	CClientDC dc(this);
	EnumFontFamiliesEx(dc.GetSafeHdc(), &lf, enumScreenFont, (LPARAM)&m_screenFonts, 0);

	// EnumFontFamiliesEx with an empty face name stops at the first strike of
	// each raster family.  Ask once for the names, then ask again per face to
	// get every bitmap size it provides.
	std::set<CString> gameNames;
	GameFontFamilies families;
	families.names = &gameNames;
	EnumFontFamiliesEx(dc.GetSafeHdc(), &lf, enumGameFamily, (LPARAM)&families, 0);

	HDC probeDC = CreateCompatibleDC(NULL);
	for (std::set<CString>::const_iterator it = gameNames.begin(); it != gameNames.end(); ++it)
	{
		LOGFONT faceLf = lf;
		lstrcpynW(faceLf.lfFaceName, *it, LF_FACESIZE);

		std::set<int> candidates;
		GameFontStrikes strikes;
		strikes.heights = &candidates;
		EnumFontFamiliesEx(dc.GetSafeHdc(), &faceLf, enumGameStrike, (LPARAM)&strikes, 0);

		// Two character heights can select the same bitmap.  Keep one entry
		// per reachable strike, preferring the height that equals the
		// strike's rendered height when the face offers it.  The map is
		// ordered by rendered height, which is the order the list shows.
		std::map<int, GameTextStrike> rendered;

		for (std::set<int>::const_iterator size = candidates.begin();
		     size != candidates.end(); ++size)
		{
			GameTextStrike strike;
			if (probeDC == NULL)
			{
				strike.width      = 0;
				strike.height     = *size;
				strike.charHeight = *size;
			}
			else if (!probeRasterStrike(probeDC, *it, *size, &strike))
			{
				continue;
			}

			std::map<int, GameTextStrike>::iterator found = rendered.find(strike.height);
			if (found == rendered.end() || strike.charHeight == strike.height)
				rendered[strike.height] = strike;
		}

		std::vector<GameTextStrike> &sizes = m_gameFontStrikes[*it];
		for (std::map<int, GameTextStrike>::const_iterator it2 = rendered.begin();
		     it2 != rendered.end(); ++it2)
			sizes.push_back(it2->second);

		if (sizes.empty())
		{
			GameTextStrike fallback;
			fallback.width      = 0;
			fallback.height     = 8;
			fallback.charHeight = 8;
			sizes.push_back(fallback);
		}
	}
	if (probeDC != NULL)
		DeleteDC(probeDC);

	// The fonts that travel with the executable are not system faces, so GDI
	// knows nothing about them and they are added by hand.  Each family
	// ("gse") is one entry in the font combo and each size it was cut in
	// ("4x6", "8x16_bold") is one entry in the size combo, which is how the
	// names read anyway.
	for (int i = 0; i < winBitmapFontCount(); i++)
	{
		CString name, family, label;
		const TextFont *font = winBitmapFontAt(i, &name, &family, &label);
		if (font == NULL)
			continue;

		GameTextStrike strike;
		strike.width      = font->width;
		strike.height     = font->height;
		strike.charHeight = font->height;
		strike.label      = label;
		strike.font       = name;

		if (m_gameFontStrikes.find(family) == m_gameFontStrikes.end())
			m_bundledFamilies.push_back(family);
		m_gameFontStrikes[family].push_back(strike);
	}
}

int TextOptions::currentMode()
{
	int mode = GetCheckedRadioButton(IDC_RADIO_PREFILTER, IDC_RADIO_POSTRENDER) - IDC_RADIO_PREFILTER;
	if (mode < 0 || mode > 2)
		mode = 1;
	return mode;
}

CString TextOptions::selectedFamilyName()
{
	CComboBox *combo = (CComboBox *)GetDlgItem(IDC_TEXT_FONT);
	if (combo == NULL)
		return CString();

	const int index = combo->GetCurSel();
	if (index == CB_ERR)
		return CString();

	if (m_currentMode != 2 && index == 0)
		return CString(GAME_TEXT_FONT_BUILTIN);

	CString name;
	combo->GetLBText(index, name);
	return name;
}

// The font combo shows a family for the bundled faces, but the renderer and
// the settings file want the one size of it that is selected.
CString TextOptions::selectedFontName()
{
	const CString family = selectedFamilyName();
	if (m_currentMode == 2 || family.IsEmpty())
		return family;

	CComboBox *combo = (CComboBox *)GetDlgItem(IDC_TEXT_SIZE);
	if (combo != NULL)
	{
		const int index = combo->GetCurSel();
		if (index >= 0 && index < (int)m_sizeList.size() &&
		    !m_sizeList[index].font.IsEmpty())
			return m_sizeList[index].font;
	}

	return family;
}

CString TextOptions::familyForFont(const CString &font)
{
	for (std::map<CString, std::vector<GameTextStrike> >::const_iterator it =
		     m_gameFontStrikes.begin(); it != m_gameFontStrikes.end(); ++it)
	{
		for (size_t i = 0; i < it->second.size(); i++)
		{
			if (!it->second[i].font.IsEmpty() &&
			    it->second[i].font.Compare(font) == 0)
				return it->first;
		}
	}

	return font;
}

int TextOptions::selectedSize()
{
	// The size combo of the game modes was filled from m_sizeList, so the
	// selection indexes straight into it.  The label is "7x11" and cannot be
	// read back as a number.
	CComboBox *combo = (CComboBox *)GetDlgItem(IDC_TEXT_SIZE);
	if (combo != NULL)
	{
		const int index = combo->GetCurSel();
		if (index >= 0 && index < (int)m_sizeList.size())
			return m_sizeList[index].charHeight;
	}

	CString text;
	GetDlgItemText(IDC_TEXT_SIZE, text);
	return _ttoi(text);
}

int TextOptions::selectedDisplaySize()
{
	CComboBox *combo = (CComboBox *)GetDlgItem(IDC_TEXT_SIZE);
	if (combo != NULL)
	{
		const int index = combo->GetCurSel();
		if (index >= 0 && index < (int)m_sizeList.size())
			return m_sizeList[index].height;
	}

	CString text;
	GetDlgItemText(IDC_TEXT_SIZE, text);
	return _ttoi(text);
}

void TextOptions::rebuildFontList()
{
	CComboBox *combo = (CComboBox *)GetDlgItem(IDC_TEXT_FONT);
	if (combo == NULL)
		return;

	combo->ResetContent();

	if (m_currentMode == 2)
	{
		if (m_screenFonts.empty())
		{
			combo->AddString(_T("Courier New"));
		}
		else
		{
			for (std::set<CString>::const_iterator it = m_screenFonts.begin();
			     it != m_screenFonts.end(); ++it)
				combo->AddString(*it);
		}

		int index = combo->FindStringExact(-1, m_screenFont);
		if (index == CB_ERR && combo->GetCount() > 0)
			index = 0;
		if (index != CB_ERR)
			combo->SetCurSel(index);
		return;
	}

	// The built-in face is listed first, under the same name the settings
	// file uses.  It stands next to the bundled families ("gse", "mcs",
	// "verdana"), and those are not translated either.
	combo->AddString(GAME_TEXT_FONT_BUILTIN);

	// Then the bundled faces.  They are always there, unlike the system
	// faces, and a user looking for one of them should not have to hunt for
	// it in the middle of what Windows happens to have installed.  One entry
	// per family; the sizes it offers are the size combo's list.
	std::set<CString> bundled;
	for (std::vector<CString>::const_iterator it = m_bundledFamilies.begin();
	     it != m_bundledFamilies.end(); ++it)
	{
		combo->AddString(*it);
		bundled.insert(*it);
	}

	for (std::map<CString, std::vector<GameTextStrike> >::const_iterator it = m_gameFontStrikes.begin();
	     it != m_gameFontStrikes.end(); ++it)
	{
		if (bundled.find(it->first) == bundled.end())
			combo->AddString(it->first);
	}

	// The stored value names one size of a bundled family ("gse7x11"), so it
	// has to be looked up as its family before the combo can match it.
	int index = 0;
	if (m_gameFont.Compare(GAME_TEXT_FONT_BUILTIN) != 0)
	{
		const int found = combo->FindStringExact(-1, familyForFont(m_gameFont));
		if (found != CB_ERR)
			index = found;
	}
	combo->SetCurSel(index);
}

void TextOptions::rebuildSizeList()
{
	CComboBox *combo = (CComboBox *)GetDlgItem(IDC_TEXT_SIZE);
	if (combo == NULL)
		return;

	combo->ResetContent();
	m_sizeList.clear();

	// On Screen uses an edit box for the TrueType height, so there is no list
	// to fill.  Only the In Game / On Game modes offer a list, because their
	// bitmap faces have a few fixed strikes.
	if (m_currentMode == 2)
		return;

	const CString face = selectedFamilyName();
	if (face.Compare(GAME_TEXT_FONT_BUILTIN) == 0)
	{
		// The built-in font only has its native 8x8 size.
		GameTextStrike builtin;
		builtin.width      = 8;
		builtin.height     = 8;
		builtin.charHeight = 8;
		builtin.label      = GAME_TEXT_FONT_BUILTIN_SIZE;
		m_sizeList.push_back(builtin);
	}
	else
	{
		std::map<CString, std::vector<GameTextStrike> >::const_iterator it =
			m_gameFontStrikes.find(face);
		if (it != m_gameFontStrikes.end())
			m_sizeList = it->second;
	}

	if (m_sizeList.empty())
	{
		GameTextStrike fallback;
		fallback.width      = 0;
		fallback.height     = 8;
		fallback.charHeight = 8;
		m_sizeList.push_back(fallback);
	}

	// A bundled face keeps the exact size that is stored, bold included: its
	// sizes are separate fonts, not a face with strikes, so nothing but the
	// name says which one was meant.
	int exact = -1;
	for (size_t i = 0; i < m_sizeList.size(); i++)
	{
		if (!m_sizeList[i].font.IsEmpty() &&
		    m_sizeList[i].font.Compare(m_gameFont) == 0)
		{
			exact = (int)i;
			break;
		}
	}

	// Otherwise keep the visible size across a font switch, not the character
	// height, so a 20 pixel face stays 20 pixels when the new face needs
	// another height to select its 20 pixel strike.
	int reference = m_gameDisplaySize;
	if (reference <= 0)
	{
		for (size_t i = 0; i < m_sizeList.size(); i++)
		{
			if (m_sizeList[i].charHeight == m_gameSize)
			{
				reference = m_sizeList[i].height;
				break;
			}
		}
		if (reference <= 0)
			reference = m_gameSize;
	}

	int best = 0;
	int bestDiff = -1;
	for (size_t i = 0; i < m_sizeList.size(); i++)
	{
		const GameTextStrike &strike = m_sizeList[i];

		// A bundled font carries its own label ("4x6", "8x16_bold").  A
		// system face is labelled with the two numbers it reports; one that
		// does not report a width shows the height alone.
		CString text = strike.label;
		if (text.IsEmpty())
		{
			if (strike.width > 0)
				text.Format(_T("%dx%d"), strike.width, strike.height);
			else
				text.Format(_T("%d"), strike.height);
		}
		combo->AddString(text);

		if (exact >= 0)
			continue;

		const int diff = abs(strike.height - reference);
		if (bestDiff < 0 || diff < bestDiff)
		{
			best = (int)i;
			bestDiff = diff;
		}
	}

	if (exact >= 0)
		best = exact;

	combo->SetCurSel(best);

	if (m_sizeList[best].charHeight > 0)
	{
		m_gameSize = m_sizeList[best].charHeight;
		m_gameDisplaySize = m_sizeList[best].height;
	}
}

void TextOptions::storeCurrentSelection()
{
	if (!m_listReady)
		return;

	const CString font = selectedFontName();

	if (m_currentMode == 2)
	{
		if (!font.IsEmpty())
			m_screenFont = font;

		// The On Screen size is typed into an edit box; keep it inside the
		// range the overlay accepts.
		BOOL translated = FALSE;
		const int size = (int)GetDlgItemInt(IDC_TEXT_SCREEN_SIZE, &translated, FALSE);
		if (translated && size > 0)
		{
			if (size < SCREEN_TEXT_SIZE_MIN)
				m_screenSize = SCREEN_TEXT_SIZE_MIN;
			else if (size > SCREEN_TEXT_SIZE_MAX)
				m_screenSize = SCREEN_TEXT_SIZE_MAX;
			else
				m_screenSize = size;
		}
	}
	else
	{
		const int size = selectedSize();
		const int displaySize = selectedDisplaySize();

		if (!font.IsEmpty())
			m_gameFont = font;
		if (size > 0)
			m_gameSize = size;
		if (displaySize > 0)
			m_gameDisplaySize = displaySize;
	}
}

void TextOptions::updateSizeControl()
{
	const bool screen = m_currentMode == 2;

	GetDlgItem(IDC_TEXT_SIZE)->ShowWindow(screen ? SW_HIDE : SW_SHOW);
	GetDlgItem(IDC_TEXT_SCREEN_SIZE)->ShowWindow(screen ? SW_SHOW : SW_HIDE);
	GetDlgItem(IDC_TEXT_SCREEN_SIZE_SPIN)->ShowWindow(screen ? SW_SHOW : SW_HIDE);

	if (!screen)
		return;

	SetDlgItemInt(IDC_TEXT_SCREEN_SIZE, (UINT)m_screenSize, FALSE);

	CSpinButtonCtrl *spin = (CSpinButtonCtrl *)GetDlgItem(IDC_TEXT_SCREEN_SIZE_SPIN);
	if (spin != NULL)
	{
		spin->SetRange32(SCREEN_TEXT_SIZE_MIN, SCREEN_TEXT_SIZE_MAX);
		spin->SetPos32(m_screenSize);
	}
}

static int clampTextMargin(int value)
{
	if (value < TEXT_MARGIN_MIN)
		return TEXT_MARGIN_MIN;
	if (value > TEXT_MARGIN_MAX)
		return TEXT_MARGIN_MAX;
	return value;
}

// Shows the margins the dialog holds.  The boxes are filled from m_marginLeft
// and m_marginBottom rather than read back: an up-down control sets its buddy
// to "0" when it is created, so the box always starts out holding a zero that
// is not the stored value.
void TextOptions::updateMarginControls()
{
	const int edits[2] = { IDC_TEXT_MARGIN_LEFT, IDC_TEXT_MARGIN_BOTTOM };
	const int spins[2] = { IDC_TEXT_MARGIN_LEFT_SPIN, IDC_TEXT_MARGIN_BOTTOM_SPIN };
	const int values[2] = { m_marginLeft, m_marginBottom };

	for (int i = 0; i < 2; i++)
	{
		const int value = clampTextMargin(values[i]);

		SetDlgItemInt(edits[i], (UINT)value, FALSE);

		CSpinButtonCtrl *spin = (CSpinButtonCtrl *)GetDlgItem(spins[i]);
		if (spin != NULL)
		{
			spin->SetRange32(TEXT_MARGIN_MIN, TEXT_MARGIN_MAX);
			spin->SetPos32(value);
		}
	}
}

// Apply has nothing to show in the In Game mode: it paints the messages into
// the emulated frame, and this dialog holds the emulator still, so the change
// cannot appear before the dialog closes.  The other two modes draw their text
// while the picture is presented and do show it straight away.
void TextOptions::updateApplyButton()
{
	CWnd *apply = GetDlgItem(IDC_TEXT_APPLY);
	if (apply != NULL)
		apply->EnableWindow(m_currentMode != 0);
}

void TextOptions::changeMode()
{
	storeCurrentSelection();

	m_currentMode = currentMode();
	rebuildFontList();
	rebuildSizeList();
	updateSizeControl();
	updateApplyButton();
	updateFontTip();
}

void TextOptions::updateFontTip()
{
	if (m_toolTip.GetSafeHwnd() == NULL)
		return;

	const UINT id = m_currentMode == 2
		? IDS_TEXT_SCREEN_FONT_TIP
		: IDS_TEXT_GAME_FONT_TIP;
	m_toolTip.UpdateTipText(winResLoadString(id), GetDlgItem(IDC_TEXT_FONT));
}

void TextOptions::populateTooltips()
{
	if (!m_toolTip.Create(this))
		return;

	m_toolTip.AddTool(GetDlgItem(IDC_RADIO_PREFILTER),
	                  winResLoadString(IDS_TEXT_PREFILTER_TIP));
	m_toolTip.AddTool(GetDlgItem(IDC_RADIO_POSTFILTER),
	                  winResLoadString(IDS_TEXT_POSTFILTER_TIP));
	m_toolTip.AddTool(GetDlgItem(IDC_RADIO_POSTRENDER),
	                  winResLoadString(IDS_TEXT_POSTRENDER_TIP));
	m_toolTip.AddTool(GetDlgItem(IDC_TEXT_FONT),
	                  winResLoadString(IDS_TEXT_GAME_FONT_TIP));
	m_toolTip.AddTool(GetDlgItem(IDC_TEXT_SIZE),
	                  winResLoadString(IDS_TEXT_SIZE_TIP));
	m_toolTip.AddTool(GetDlgItem(IDC_TEXT_SCREEN_SIZE),
	                  winResLoadString(IDS_TEXT_SIZE_TIP));
	m_toolTip.AddTool(GetDlgItem(IDC_TEXT_MARGIN_LEFT),
	                  winResLoadString(IDS_TEXT_MARGIN_LEFT_TIP));
	m_toolTip.AddTool(GetDlgItem(IDC_TEXT_MARGIN_BOTTOM),
	                  winResLoadString(IDS_TEXT_MARGIN_BOTTOM_TIP));
	m_toolTip.Activate(TRUE);

	updateFontTip();
}

BOOL TextOptions::OnInitDialog()
{
	CDialog::OnInitDialog();

	CheckRadioButton(IDC_RADIO_WHITE, IDC_RADIO_BLACK, IDC_RADIO_WHITE + textColor);
	CheckRadioButton(IDC_RADIO_PREFILTER, IDC_RADIO_POSTRENDER, IDC_RADIO_PREFILTER + textMethod);
	CheckDlgButton(IDC_CHECK_OUTLINED, outlinedText);
	CheckDlgButton(IDC_CHECK_TRANSPARENT, transparentText);
	GetDlgItem(IDC_CHECK_TRANSPARENT)->EnableWindow(GetCheckedRadioButton(IDC_RADIO_PREFILTER,
	                                                                      IDC_RADIO_POSTRENDER) != IDC_RADIO_POSTRENDER);

	m_currentMode = currentMode();

	m_screenFont = theApp.screenTextFont;
	if (m_screenFont.IsEmpty())
		m_screenFont = _T("Consolas");

	m_gameFont = theApp.gameTextFont;
	if (m_gameFont.IsEmpty())
		m_gameFont = GAME_TEXT_FONT_BUILTIN;

	m_screenSize = theApp.screenTextSize;
	if (m_screenSize < SCREEN_TEXT_SIZE_MIN || m_screenSize > SCREEN_TEXT_SIZE_MAX)
		m_screenSize = 12;

	m_gameSize = theApp.gameTextSize;
	if (m_gameSize < 1 || m_gameSize > 128)
		m_gameSize = 8;
	m_gameDisplaySize = 0;

	// The margins are typed into boxes, so the dialog is the only place that
	// can lose them; it starts from what the settings hold.
	m_marginLeft   = clampTextMargin(theApp.textMarginLeft);
	m_marginBottom = clampTextMargin(theApp.textMarginBottom);

	updateMarginControls();

	collectFonts();
	rebuildFontList();
	rebuildSizeList();
	updateSizeControl();
	updateApplyButton();
	m_listReady = true;
	populateTooltips();

	return TRUE;  // return TRUE unless you set the focus to a control
	// EXCEPTION: OCX Property Pages should return FALSE
}

void TextOptions::DoDataExchange(CDataExchange*pDX)
{
	CDialog::DoDataExchange(pDX);
}

BOOL TextOptions::PreTranslateMessage(MSG* pMsg)
{
	if (m_toolTip.GetSafeHwnd() != NULL)
		m_toolTip.RelayEvent(pMsg);

	return CDialog::PreTranslateMessage(pMsg);
}

BEGIN_MESSAGE_MAP(TextOptions, CDialog)
ON_BN_CLICKED(IDOK, OnBnClickedOk)
ON_BN_CLICKED(IDC_TEXT_APPLY, OnBnClickedApply)
ON_BN_CLICKED(IDC_RADIO_PREFILTER, OnBnClickedRadioPrefilter)
ON_BN_CLICKED(IDC_RADIO_POSTFILTER, OnBnClickedRadioPostfilter)
ON_BN_CLICKED(IDC_RADIO_POSTRENDER, OnBnClickedRadioPostrender)
ON_CBN_SELCHANGE(IDC_TEXT_FONT, OnSelchangeFont)
END_MESSAGE_MAP()

// TextOptions message handlers

void TextOptions::applySettings()
{
	transparentText = IsDlgButtonChecked(IDC_CHECK_TRANSPARENT) != 0;
	outlinedText    = IsDlgButtonChecked(IDC_CHECK_OUTLINED) != 0;
	textMethod      = GetCheckedRadioButton(IDC_RADIO_PREFILTER, IDC_RADIO_POSTRENDER) - IDC_RADIO_PREFILTER;
	textColor       = GetCheckedRadioButton(IDC_RADIO_WHITE, IDC_RADIO_BLACK) - IDC_RADIO_WHITE;
	if (textMethod < 0)
		textMethod = 0;
	if (textMethod > 2)
		textMethod = 2;
	if (textColor < 0)
		textColor = 0;
	if (textColor > 7)
		textColor = 7;

	storeCurrentSelection();

	theApp.screenTextFont = m_screenFont;
	theApp.gameTextFont   = m_gameFont;
	theApp.screenTextSize = m_screenSize;
	theApp.gameTextSize   = m_gameSize;

	// The margins are the one setting that is read straight out of its
	// control: it can be typed into, and the spinner changes it.  A box that
	// cannot be read keeps what the dialog already holds instead of storing
	// a zero.
	const int marginIds[2] = { IDC_TEXT_MARGIN_LEFT, IDC_TEXT_MARGIN_BOTTOM };
	int *margins[2] = { &m_marginLeft, &m_marginBottom };

	for (int i = 0; i < 2; i++)
	{
		BOOL translated = FALSE;
		const UINT value = GetDlgItemInt(marginIds[i], &translated, FALSE);
		if (!translated)
			continue;

		*margins[i] = value > (UINT)TEXT_MARGIN_MAX
			? TEXT_MARGIN_MAX
			: (int)value;
	}

	theApp.textMarginLeft   = m_marginLeft;
	theApp.textMarginBottom = m_marginBottom;

	// Show the clamped size if the user typed one outside the range.
	updateSizeControl();
	updateMarginControls();
}

void TextOptions::OnBnClickedOk()
{
	applySettings();
	theApp.saveSettings();
	OnOK();
}

void TextOptions::OnBnClickedApply()
{
	applySettings();
	theApp.saveSettings();
	systemRefreshScreen();
}

void TextOptions::OnBnClickedRadioPrefilter()
{
	GetDlgItem(IDC_CHECK_TRANSPARENT)->EnableWindow(TRUE);
	changeMode();
}

void TextOptions::OnBnClickedRadioPostfilter()
{
	GetDlgItem(IDC_CHECK_TRANSPARENT)->EnableWindow(TRUE);
	changeMode();
}

void TextOptions::OnBnClickedRadioPostrender()
{
	GetDlgItem(IDC_CHECK_TRANSPARENT)->EnableWindow(FALSE);
	CheckDlgButton(IDC_CHECK_TRANSPARENT, FALSE);
	changeMode();
}

void TextOptions::OnSelchangeFont()
{
	// Game faces have a few fixed strikes; keep the current size as the
	// reference and pick the closest strike the new face provides.  On Screen
	// sizes are typed in, so its size does not depend on the face.
	if (m_currentMode == 2)
	{
		storeCurrentSelection();
		return;
	}

	// The size list belongs to the family, so it is rebuilt before the
	// selection is read back: the stored name has to be resolved against the
	// sizes of the family that is now selected.
	rebuildSizeList();
	storeCurrentSelection();
}
