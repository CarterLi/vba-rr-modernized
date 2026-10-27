// GBACheats.cpp : implementation file
//

#include "../stdafx.h"
#include "../resource.h"
#include "GBACheatsDlg.h"
#include "CheatExplain.h"
#include "../StringTokenizer.h"
#include "../Reg.h"
#include "../WinResUtil.h"
#include "../Sound.h"
#include "../VBA.h"
#include "../UiFont.h"

#include "../../common/CheatSearch.h"
#include "../../gba/GBACheats.h"
#include "../../gba/GBAGlobals.h"
#include "../../NLS.h"

////////////////////////////////

bool winGbaCheatReaddress()
{
	if (cheatSearchData.count != 2)
		return false;

	CheatSearchBlock *block = &cheatSearchData.blocks[0];
	block->data   = workRAM;

	block         = &cheatSearchData.blocks[1];
	block->data   = internalRAM;

	cheatSearchData.count = 2;
	return true;
}

enum GBACheatType
{
	GBA_CHEAT_TYPE_CODE,
	GBA_CHEAT_TYPE_GAMESHARK,
	GBA_CHEAT_TYPE_CODEBREAKER
};

static GBACheatType gbaCheatType(int code)
{
	if (code == 512)
		return GBA_CHEAT_TYPE_CODEBREAKER;
	if (code == 256 || code == 257)
		return GBA_CHEAT_TYPE_GAMESHARK;
	return GBA_CHEAT_TYPE_CODE;
}

static const char *gbaCheatTypeName(int code)
{
	switch (gbaCheatType(code))
	{
	case GBA_CHEAT_TYPE_GAMESHARK:
		return "Gameshark";
	case GBA_CHEAT_TYPE_CODEBREAKER:
		return "CodeBreaker";
	default:
		return "Code";
	}
}

// Keep LVS_EX_CHECKBOXES for native checkbox images.  Group rows use two
// extra state images, rendered from the Unicode arrow glyphs, so they show
// an expand/collapse arrow where a checkbox would normally appear.
enum GBACheatState
{
	CHEAT_STATE_COLLAPSED = 3,
	CHEAT_STATE_EXPANDED  = 4
};

static bool addCheatArrowImages(CListCtrl &list)
{
	CImageList *images = list.GetImageList(LVSIL_STATE);
	if (!images)
		return false;

	IMAGEINFO info;
	if (!images->GetImageInfo(0, &info))
		return false;

	int cx = info.rcImage.right - info.rcImage.left;
	int cy = info.rcImage.bottom - info.rcImage.top;
	if (cx <= 0 || cy <= 0)
		return false;

	CClientDC screenDC(NULL);
	CDC dc;
	if (!dc.CreateCompatibleDC(&screenDC))
		return false;

	CBitmap bitmap;
	if (!bitmap.CreateCompatibleBitmap(&screenDC, cx, cy))
		return false;

	CBitmap *oldBitmap = dc.SelectObject(&bitmap);
	const COLORREF maskColor = RGB(255, 0, 255);

	LOGFONTW lf;
	memset(&lf, 0, sizeof(lf));
	int fontHeight = cy - 2;
	if (fontHeight < 8)
		fontHeight = cy;
	lf.lfHeight = -fontHeight;
	lf.lfWeight = FW_NORMAL;
	lf.lfCharSet = DEFAULT_CHARSET;
	lf.lfOutPrecision = OUT_DEFAULT_PRECIS;
	lf.lfClipPrecision = CLIP_DEFAULT_PRECIS;
	lf.lfQuality = NONANTIALIASED_QUALITY;
	lf.lfPitchAndFamily = DEFAULT_PITCH | FF_DONTCARE;
	lstrcpynW(lf.lfFaceName, L"Segoe UI Symbol", LF_FACESIZE);

	HFONT font = ::CreateFontIndirectW(&lf);
	HGDIOBJ oldFont = NULL;
	if (font)
		oldFont = ::SelectObject(dc.GetSafeHdc(), font);

	dc.SetBkMode(TRANSPARENT);
	dc.SetTextColor(RGB(0, 0, 0));

	const wchar_t *arrows[2] = { L"\u25B6", L"\u25BC" };
	bool ok = true;
	for (int i = 0; i < 2; i++)
	{
		dc.FillSolidRect(0, 0, cx, cy, maskColor);
		CRect rect(0, 0, cx, cy);
		::DrawTextW(dc.GetSafeHdc(), arrows[i], -1, &rect,
		            DT_CENTER | DT_VCENTER | DT_SINGLELINE | DT_NOPREFIX);

		// ImageList_Add() requires a bitmap that is not selected into a DC.
		dc.SelectObject(oldBitmap);
		int added = images->Add(&bitmap, maskColor);
		if (added == -1)
		{
			ok = false;
			break;
		}
		dc.SelectObject(&bitmap);
	}

	if (font)
	{
		::SelectObject(dc.GetSafeHdc(), oldFont);
		::DeleteObject(font);
	}
	dc.SelectObject(oldBitmap);
	return ok;
}

static bool gbaCheatsCanGroup(int first, int second)
{
	if (first < 0 || second < 0 || first >= cheatsNumber || second >= cheatsNumber)
		return false;
	if (!cheatsList[first].desc[0] || strcmp(cheatsList[first].desc, cheatsList[second].desc) != 0)
		return false;
	return gbaCheatType(cheatsList[first].code) == gbaCheatType(cheatsList[second].code);
}

static void deleteCheatRange(int first, int count)
{
	for (int i = first + count - 1; i >= first; i--)
		cheatsDelete(i, false);
}

// A GSA v3 code is stored as 16 hex digits.  The batch dialog treats a
// single 16-digit token as v1/v2 and an 8+8 token pair as v3, so insert a
// separator when editing v3 codes to keep their encryption mode intact.
static CString gsaV3CodeForEdit(const char *code)
{
	CString result = code;
	if (result.GetLength() == 16)
		result.Insert(8, ' ');
	return result;
}

static void addInvalidCode(CString &errors, const CString &code)
{
	if (!errors.IsEmpty())
		errors += "\r\n";
	errors += code;
}

static bool reportInvalidCodes(const CString &errors)
{
	if (errors.IsEmpty())
		return false;
	systemMessage(IDS_INVALID_CHEAT_CODES, N_(_T("Invalid cheat code(s):\n%s")), (LPCTSTR)errors);
	return true;
}

static bool processRawCheatCodes(const CString &buffer, const CString &desc, bool apply, int *codeCount = NULL)
{
	StringTokenizer st(buffer, " \t\n\r");
	LPCTSTR t = st.next();
	if (!t)
	{
		systemMessage(0, N_("Code cannot be empty"));
		return false;
	}

	CString errors;
	int count = 0;
	while (t)
	{
		CString token = t;
		token.MakeUpper();
		if (!cheatsValidateCheatCode(winAnsiFromWide(token), false))
		{
			if (!apply)
				addInvalidCode(errors, token);
		}
		else
		{
			if (apply)
				cheatsAddCheatCode(winAnsiFromWide(token), winAnsiFromWide(desc));
			count++;
		}
		t = st.next();
	}

	if (!apply && reportInvalidCodes(errors))
		return false;

	if (codeCount)
		*codeCount = count;
	return true;
}

static bool processGsaCbaCodes(const CString &buffer, const CString &desc, bool apply, int *codeCount = NULL)
{
	StringTokenizer st(buffer, " \t\n\r");
	CString part1;
	CString errors;
	int count = 0;

	LPCTSTR t = st.next();
	if (!t)
	{
		systemMessage(0, N_("Code cannot be empty"));
		return false;
	}

	while (t)
	{
		CString token = t;
		token.MakeUpper();
		if (token.GetLength() == 16)
		{
			if (!cheatsValidateGSACode(winAnsiFromWide(token), false))
			{
				if (!apply)
					addInvalidCode(errors, token);
			}
			else
			{
				if (apply)
					cheatsAddGSACode(winAnsiFromWide(token), winAnsiFromWide(desc), false);
				count++;
			}
		}
		else if (token.GetLength() == 12)
		{
			CString code = token.Left(8) + " " + token.Right(4);
			if (!cheatsValidateCBACode(winAnsiFromWide(code), false))
			{
				if (!apply)
					addInvalidCode(errors, code);
			}
			else
			{
				if (apply)
					cheatsAddCBACode(winAnsiFromWide(code), winAnsiFromWide(desc));
				count++;
			}
		}
		else if (part1.IsEmpty())
			part1 = token;
		else
		{
			CString code;
			if (token.GetLength() == 4)
			{
				code = part1 + " " + token;
				if (!cheatsValidateCBACode(winAnsiFromWide(code), false))
				{
					if (!apply)
						addInvalidCode(errors, code);
				}
				else
				{
					if (apply)
						cheatsAddCBACode(winAnsiFromWide(code), winAnsiFromWide(desc));
					count++;
				}
			}
			else
			{
				code = part1 + token;
				if (!cheatsValidateGSACode(winAnsiFromWide(code), false))
				{
					if (!apply)
						addInvalidCode(errors, code);
				}
				else
				{
					if (apply)
						cheatsAddGSACode(winAnsiFromWide(code), winAnsiFromWide(desc), true);
					count++;
				}
			}
			part1.Empty();
		}

		t = st.next();
	}

	if (!part1.IsEmpty())
	{
		// an odd number of tokens was entered
		if (!apply)
			addInvalidCode(errors, part1);
	}

	if (!apply && reportInvalidCodes(errors))
		return false;

	if (codeCount)
		*codeCount = count;
	return true;
}

/////////////////////////////////////////////////////////////////////////////
// GBACheatSearch dialog

GBACheatSearch::GBACheatSearch(CWnd*pParent /*=NULL*/)
	: CDialog(GBACheatSearch::IDD, pParent)
{
	//{{AFX_DATA_INIT(GBACheatSearch)
	valueType    = -1;
	sizeType     = -1;
	searchType   = -1;
	numberType   = -1;
	updateValues = FALSE;
	//}}AFX_DATA_INIT
	data = NULL;
}

GBACheatSearch::~GBACheatSearch()
{
	if (data)
		free(data);
}

void GBACheatSearch::DoDataExchange(CDataExchange*pDX)
{
	CDialog::DoDataExchange(pDX);
	//{{AFX_DATA_MAP(GBACheatSearch)
	DDX_Control(pDX, IDC_VALUE, m_value);
	DDX_Control(pDX, IDC_CHEAT_LIST, m_list);
	DDX_Radio(pDX, IDC_OLD_VALUE, valueType);
	DDX_Radio(pDX, IDC_SIZE_8, sizeType);
	DDX_Radio(pDX, IDC_EQ, searchType);
	DDX_Radio(pDX, IDC_SIGNED, numberType);
	DDX_Check(pDX, IDC_UPDATE, updateValues);
	//}}AFX_DATA_MAP
}

BEGIN_MESSAGE_MAP(GBACheatSearch, CDialog)
//{{AFX_MSG_MAP(GBACheatSearch)
ON_BN_CLICKED(ID_OK, OnOk)
ON_BN_CLICKED(IDC_START, OnStart)
ON_BN_CLICKED(IDC_SEARCH, OnSearch)
ON_BN_CLICKED(IDC_ADD_CHEAT, OnAddCheat)
ON_BN_CLICKED(IDC_UPDATE, OnUpdate)
ON_NOTIFY(LVN_GETDISPINFO, IDC_CHEAT_LIST, OnGetdispinfoCheatList)
ON_NOTIFY(LVN_ITEMCHANGED, IDC_CHEAT_LIST, OnItemchangedCheatList)
ON_CONTROL_RANGE(BN_CLICKED, IDC_OLD_VALUE, IDC_SPECIFIC_VALUE, OnValueType)
ON_CONTROL_RANGE(BN_CLICKED, IDC_EQ, IDC_GE, OnSearchType)
ON_CONTROL_RANGE(BN_CLICKED, IDC_SIGNED, IDC_HEXADECIMAL, OnNumberType)
ON_CONTROL_RANGE(BN_CLICKED, IDC_SIZE_8, IDC_SIZE_32, OnSizeType)
//}}AFX_MSG_MAP
ON_WM_CLOSE()
ON_BN_CLICKED(IDC_CHEATREFRESHBUTTON, OnBnClickedCheatrefreshbutton)
//ON_WM_DESTROY()
//ON_WM_NCDESTROY()
//ON_WM_SYSCOMMAND()
//ON_WM_ACTIVATE()
END_MESSAGE_MAP()

/////////////////////////////////////////////////////////////////////////////
// GBACheatSearch message handlers

void GBACheatSearch::OnOk()
{
	if (theApp.modelessCheatDialogIsOpen)
	{
		theApp.modelessCheatDialogIsOpen = false;
		DestroyWindow();
	}
	else
	{
		EndDialog(TRUE);
	}
}

void GBACheatSearch::OnClose()
{
	CDialog::OnClose();
	if (theApp.modelessCheatDialogIsOpen)
	{
		theApp.modelessCheatDialogIsOpen = false;
		DestroyWindow();
	}
	else
	{
		EndDialog(FALSE);
	}
}

void GBACheatSearch::OnStart()
{
	if (cheatSearchData.count == 0)
	{
		CheatSearchBlock *block = &cheatSearchData.blocks[0];
		block->data   = workRAM;
		block->offset = 0x2000000;
		block->size   = 0x40000;
		block->saved  = (u8 *)malloc(0x40000);
		block->bits   = (u8 *)malloc(0x40000>>3);

		block         = &cheatSearchData.blocks[1];
		block->data   = internalRAM;
		block->offset = 0x3000000;
		block->size   = 0x8000;
		block->saved  = (u8 *)malloc(0x8000);
		block->bits   = (u8 *)malloc(0x8000>>3);

		cheatSearchData.count = 2;
	}

	cheatSearchStart(&cheatSearchData);
	GetDlgItem(IDC_SEARCH)->EnableWindow(TRUE);

	if (theApp.modelessCheatDialogIsOpen)
	{
		GetDlgItem(IDC_CHEATREFRESHBUTTON)->ShowWindow(TRUE);
		GetDlgItem(IDC_CHEATREFRESHBUTTON)->EnableWindow(FALSE);
	}
}

void GBACheatSearch::OnSearch()
{
	CString buffer;

	if (valueType == 0)
		cheatSearch(&cheatSearchData,
		            searchType,
		            sizeType,
		            numberType == 0);
	else
	{
		m_value.GetWindowText(buffer);
		if (buffer.IsEmpty())
		{
			systemMessage(IDS_NUMBER_CANNOT_BE_EMPTY, N_("Number cannot be empty"));
			return;
		}
		int value = 0;
		switch (numberType)
		{
		case 0:
			value = (int)_tcstol(buffer, NULL, 10);
			break;
		case 1:
			value = (int)_tcstoul(buffer, NULL, 10);
			break;
		default:
			value = (int)_tcstoul(buffer, NULL, 16);
		}
		cheatSearchValue(&cheatSearchData,
		                 searchType,
		                 sizeType,
		                 numberType == 0,
		                 value);
	}

	addChanges(true);

	if (updateValues)
		cheatSearchUpdateValues(&cheatSearchData);

	if (theApp.modelessCheatDialogIsOpen)
		GetDlgItem(IDC_CHEATREFRESHBUTTON)->EnableWindow(cheatSearchGetCount(&cheatSearchData, sizeType) == 0 ? FALSE : TRUE);

	if (0 == cheatSearchGetCount(&cheatSearchData, sizeType))
		OnStart();
}

void GBACheatSearch::OnAddCheat()
{
	int mark = m_list.GetSelectionMark();

	if (mark != -1)
	{
		LVITEM item;
		memset(&item, 0, sizeof(item));
		item.mask  = LVIF_PARAM;
		item.iItem = mark;
		if (m_list.GetItem(&item))
		{
			AddCheat dlg((u32)item.lParam);
			dlg.DoModal();
		}
	}
}

void GBACheatSearch::OnUpdate()
{
	if (GetDlgItem(IDC_UPDATE)->SendMessage(BM_GETCHECK,
	                                        0,
	                                        0) & BST_CHECKED)
		updateValues = true;
	else
		updateValues = false;
	regSetDwordValue(_T("cheatsUpdate"), updateValues);
}

void GBACheatSearch::OnGetdispinfoCheatList(NMHDR*pNMHDR, LRESULT*pResult)
{
	LV_DISPINFO*info = (LV_DISPINFO *)pNMHDR;
	if (info->item.mask & LVIF_TEXT)
	{
		int index = info->item.iItem;
		int col   = info->item.iSubItem;

		switch (col)
		{
		case 0:
			lstrcpyn(info->item.pszText, winWideFromAnsi(data[index].address), info->item.cchTextMax);
			break;
		case 1:
			lstrcpyn(info->item.pszText, winWideFromAnsi(data[index].oldValue), info->item.cchTextMax);
			break;
		case 2:
			lstrcpyn(info->item.pszText, winWideFromAnsi(data[index].newValue), info->item.cchTextMax);
			break;
		}
	}
	*pResult = TRUE;
}

void GBACheatSearch::OnItemchangedCheatList(NMHDR*pNMHDR, LRESULT*pResult)
{
	GetDlgItem(IDC_ADD_CHEAT)->EnableWindow(m_list.GetSelectionMark() != -1);
	*pResult = TRUE;
}

BOOL GBACheatSearch::OnInitDialog()
{
	CDialog::OnInitDialog();

	CString temp = winResLoadString(IDS_ADDRESS);

	m_list.InsertColumn(0, temp, LVCFMT_CENTER, 125, 0);

	temp = winResLoadString(IDS_OLD_VALUE);
	m_list.InsertColumn(1, temp, LVCFMT_CENTER, 125, 1);

	temp = winResLoadString(IDS_NEW_VALUE);
	m_list.InsertColumn(2, temp, LVCFMT_CENTER, 125, 2);

	m_list.SetFont(CFont::FromHandle(uiFont::mono()),
	               TRUE);

	m_list.SetExtendedStyle(LVS_EX_FULLROWSELECT);

	if (!cheatSearchData.count)
	{
		GetDlgItem(IDC_SEARCH)->EnableWindow(FALSE);
		GetDlgItem(IDC_ADD_CHEAT)->EnableWindow(FALSE);
	}

	valueType = regQueryDwordValue(_T("cheatsValueType"), 0);
	if (valueType < 0 || valueType > 1)
		valueType = 0;

	searchType = regQueryDwordValue(_T("cheatsSearchType"), SEARCH_EQ);
	if (searchType > 5 || searchType < 0)
		searchType = 0;

	numberType = regQueryDwordValue(_T("cheatsNumberType"), 2);
	if (numberType < 0 || numberType > 2)
		numberType = 2;

	sizeType = regQueryDwordValue(_T("cheatsSizeType"), 0);
	if (sizeType < 0 || sizeType > 2)
		sizeType = 0;

	updateValues = regQueryDwordValue(_T("cheatsUpdate"), 0) ?
	               true : false;

	UpdateData(FALSE);

	if (valueType == 0)
		m_value.EnableWindow(FALSE);
	CenterWindow();

	if (theApp.modelessCheatDialogIsOpen)
		GetDlgItem(IDC_CHEATREFRESHBUTTON)->ShowWindow(TRUE);

	if (cheatSearchData.count)
	{
		addChanges(false);
		if (theApp.modelessCheatDialogIsOpen)
			GetDlgItem(IDC_CHEATREFRESHBUTTON)->EnableWindow(cheatSearchGetCount(&cheatSearchData,
			                                                                     sizeType) == 0 ? FALSE : TRUE);
	}
	else
	{
		if (theApp.modelessCheatDialogIsOpen)
			GetDlgItem(IDC_CHEATREFRESHBUTTON)->EnableWindow(FALSE);
	}

	return TRUE; // return TRUE unless you set the focus to a control
	             // EXCEPTION: OCX Property Pages should return FALSE
}

void GBACheatSearch::addChanges(bool showMsgs)
{
	int count = cheatSearchGetCount(&cheatSearchData, sizeType);

	m_list.DeleteAllItems();

	if (count > 4000)
	{
		if (showMsgs)
			systemMessage(
			    IDS_SEARCH_PRODUCED_TOO_MANY,
			    N_("Search produced %d results.\nThey have been remembered, but are too many to display.\nPlease refine it better by performing additional searches."),
			    count);
		return;
	}

	if (count == 0)
	{
		if (showMsgs)
			systemMessage(IDS_SEARCH_PRODUCED_NO_RESULTS, N_("Search produced no results."));
		return;
	}

	m_list.SetItemCount(count);
	if (data)
		free(data);

	data = (WinCheatsData *)calloc(count, sizeof(WinCheatsData));

	int inc = 1;
	switch (sizeType)
	{
	case 1:
		inc = 2;
		break;
	case 2:
		inc = 4;
		break;
	}

	int index = 0;
	if (numberType == 0)
	{
		for (int i = 0; i < cheatSearchData.count; i++)
		{
			CheatSearchBlock *block = &cheatSearchData.blocks[i];

			for (int j = 0; j < block->size; j += inc)
			{
				if (IS_BIT_SET(block->bits, j))
				{
					addChange(index++,
					          block->offset | j,
					          cheatSearchSignedRead(block->saved,
					                                j,
					                                sizeType),
					          cheatSearchSignedRead(block->data,
					                                j,
					                                sizeType));
				}
			}
		}
	}
	else
	{
		for (int i = 0; i < cheatSearchData.count; i++)
		{
			CheatSearchBlock *block = &cheatSearchData.blocks[i];

			for (int j = 0; j < block->size; j += inc)
			{
				if (IS_BIT_SET(block->bits, j))
				{
					addChange(index++,
					          block->offset | j,
					          cheatSearchRead(block->saved,
					                          j,
					                          sizeType),
					          cheatSearchRead(block->data,
					                          j,
					                          sizeType));
				}
			}
		}
	}

	for (int i = 0; i < count; i++)
	{
		LVITEM item;

		item.mask      = LVIF_TEXT | LVIF_PARAM | LVIF_STATE;
		item.iItem     = i;
		item.iSubItem  = 0;
		item.lParam    = data[i].addr;
		item.state     = 0;
		item.stateMask = 0;
		item.pszText   = LPSTR_TEXTCALLBACK;
		m_list.InsertItem(&item);

		m_list.SetItemText(i, 1, LPSTR_TEXTCALLBACK);
		m_list.SetItemText(i, 2, LPSTR_TEXTCALLBACK);
	}
}

void GBACheatSearch::addChange(int index, u32 address, u32 oldValue, u32 newValue)
{
	data[index].addr = address;
	sprintf(data[index].address, "%08x", address);
	switch (numberType)
	{
	case 0:
		sprintf(data[index].oldValue, "%d", oldValue);
		sprintf(data[index].newValue, "%d", newValue);
		break;
	case 1:
		sprintf(data[index].oldValue, "%u", oldValue);
		sprintf(data[index].newValue, "%u", newValue);
		break;
	case 2:
		switch (sizeType)
		{
		case 0:
			sprintf(data[index].oldValue, "%02x", oldValue);
			sprintf(data[index].newValue, "%02x", newValue);
			break;
		case 1:
			sprintf(data[index].oldValue, "%04x", oldValue);
			sprintf(data[index].newValue, "%04x", newValue);
			break;
		case 2:
			sprintf(data[index].oldValue, "%08x", oldValue);
			sprintf(data[index].newValue, "%08x", newValue);
			break;
		}
	}
}

void GBACheatSearch::OnValueType(UINT id)
{
	switch (id)
	{
	case IDC_OLD_VALUE:
		valueType = 0;
		m_value.EnableWindow(FALSE);
		regSetDwordValue(_T("cheatsValueType"), 0);
		break;
	case IDC_SPECIFIC_VALUE:
		valueType = 1;
		m_value.EnableWindow(TRUE);
		regSetDwordValue(_T("cheatsValueType"), 1);
		break;
	}
}

void GBACheatSearch::OnSearchType(UINT id)
{
	switch (id)
	{
	case IDC_EQ:
		searchType = SEARCH_EQ;
		regSetDwordValue(_T("cheatsSearchType"), 0);
		break;
	case IDC_NE:
		searchType = SEARCH_NE;
		regSetDwordValue(_T("cheatsSearchType"), 1);
		break;
	case IDC_LT:
		searchType = SEARCH_LT;
		regSetDwordValue(_T("cheatsSearchType"), 2);
		break;
	case IDC_LE:
		searchType = SEARCH_LE;
		regSetDwordValue(_T("cheatsSearchType"), 3);
		break;
	case IDC_GT:
		searchType = SEARCH_GT;
		regSetDwordValue(_T("cheatsSearchType"), 4);
		break;
	case IDC_GE:
		searchType = SEARCH_GE;
		regSetDwordValue(_T("cheatsSearchType"), 5);
		break;
	}
}

void GBACheatSearch::OnNumberType(UINT id)
{
	switch (id)
	{
	case IDC_SIGNED:
		numberType = 0;
		regSetDwordValue(_T("cheatsNumberType"), 0);
		if (m_list.GetItemCount())
		{
			addChanges(false);
		}
		break;
	case IDC_UNSIGNED:
		numberType = 1;
		regSetDwordValue(_T("cheatsNumberType"), 1);
		if (m_list.GetItemCount())
		{
			addChanges(false);
		}
		break;
	case IDC_HEXADECIMAL:
		numberType = 2;
		regSetDwordValue(_T("cheatsNumberType"), 2);
		if (m_list.GetItemCount())
		{
			addChanges(false);
		}
		break;
	}
}

void GBACheatSearch::OnSizeType(UINT id)
{
	switch (id)
	{
	case IDC_SIZE_8:
		sizeType = BITS_8;
		regSetDwordValue(_T("cheatsSizeType"), 0);
		if (m_list.GetItemCount())
		{
			addChanges(false);
		}
		break;
	case IDC_SIZE_16:
		sizeType = BITS_16;
		regSetDwordValue(_T("cheatsSizeType"), 1);
		if (m_list.GetItemCount())
		{
			addChanges(false);
		}
		break;
	case IDC_SIZE_32:
		sizeType = BITS_32;
		regSetDwordValue(_T("cheatsSizeType"), 2);
		if (m_list.GetItemCount())
		{
			addChanges(false);
		}
		break;
	}
}

/////////////////////////////////////////////////////////////////////////////
// AddCheat dialog

AddCheat::AddCheat(u32 address, CWnd*pParent /*=NULL*/, int editIndex /*=-1*/, bool initialEnabled /*=true*/)
	: CDialog(AddCheat::IDD, pParent)
{
	//{{AFX_DATA_INIT(AddCheat)
	numberType = regQueryDwordValue(_T("cheatsNumberType"), 2);
	if (numberType < 0 || numberType > 2)
		numberType = 2;
	sizeType = regQueryDwordValue(_T("cheatsSizeType"), 0);
	if (sizeType < 0 || sizeType > 2)
		sizeType = 0;
	//}}AFX_DATA_INIT
	this->address = address;
	this->editIndex = editIndex;
	this->initialEnabled = initialEnabled;
	if (editIndex >= 0 && editIndex < cheatsNumber)
	{
		this->address = cheatsList[editIndex].address;
		sizeType = cheatsList[editIndex].size;
		// V8 stores raw 16/32-bit codes with these internal type values.
		if (sizeType == 114)
			sizeType = 1;
		else if (sizeType == 115)
			sizeType = 2;
		if (sizeType < 0 || sizeType > 2)
			sizeType = 0;
	}
}

void AddCheat::DoDataExchange(CDataExchange*pDX)
{
	CDialog::DoDataExchange(pDX);
	//{{AFX_DATA_MAP(AddCheat)
	DDX_Control(pDX, IDC_VALUE, m_value);
	DDX_Control(pDX, IDC_DESC, m_desc);
	DDX_Control(pDX, IDC_ADDRESS, m_address);
	DDX_Control(pDX, IDC_CHEAT_ENABLED, m_enabled);
	DDX_Radio(pDX, IDC_SIZE_8, sizeType);
	DDX_Radio(pDX, IDC_SIGNED, numberType);
	//}}AFX_DATA_MAP
}

BEGIN_MESSAGE_MAP(AddCheat, CDialog)
//{{AFX_MSG_MAP(AddCheat)
// IDOK/IDCANCEL and not the custom ID_OK/ID_CANCEL the template used to carry:
// the buttons were off the dialog, so nothing ever pressed them, and only the
// standard ids make Enter and Esc do the same thing as the buttons.
ON_BN_CLICKED(IDOK, OnOk)
ON_BN_CLICKED(IDCANCEL, OnCancel)
ON_CONTROL_RANGE(BN_CLICKED, IDC_SIGNED, IDC_HEXADECIMAL, OnNumberType)
ON_CONTROL_RANGE(BN_CLICKED, IDC_SIZE_8, IDC_SIZE_32, OnSizeType)
//}}AFX_MSG_MAP
END_MESSAGE_MAP()

/////////////////////////////////////////////////////////////////////////////
// AddCheat message handlers

void AddCheat::OnOk()
{
	// add cheat
	if (addCheat())
	{
		EndDialog(TRUE);
	}
}

void AddCheat::OnCancel()
{
	EndDialog(FALSE);
}

BOOL AddCheat::OnInitDialog()
{
	CDialog::OnInitDialog();

	CString buffer;

	if (editIndex >= 0 && editIndex < cheatsNumber)
	{
		buffer.Format(_T("%08x"), cheatsList[editIndex].address);
		m_address.SetWindowText(buffer);

		switch (numberType)
		{
		case 0:
			buffer.Format(_T("%d"), (int)cheatsList[editIndex].value);
			break;
		case 1:
			buffer.Format(_T("%u"), cheatsList[editIndex].value);
			break;
		default:
			buffer.Format(_T("%x"), cheatsList[editIndex].value);
			break;
		}
		m_value.SetWindowText(buffer);
		m_desc.SetWindowText(winWideFromAnsi(cheatsList[editIndex].desc));
	}
	else if (address != 0)
	{
		buffer.Format(_T("%08x"), address);
		m_address.SetWindowText(buffer);
		m_address.EnableWindow(FALSE);
	}

	UpdateData(FALSE);

	GetDlgItem(IDC_DESC)->SendMessage(EM_LIMITTEXT,
	                                  31,
	                                  0);

	if (editIndex < 0)
	{
		CString desc;
		m_desc.GetWindowText(desc);
		if (desc.IsEmpty())
			m_desc.SetWindowText(CTime::GetCurrentTime().Format("%Y-%m-%d %H:%M:%S"));
	}

	m_enabled.SetCheck(initialEnabled ? BST_CHECKED : BST_UNCHECKED);

	if (editIndex >= 0)
		SetWindowText(winResLoadString(IDS_EDIT_CHEAT));

	CenterWindow();

	return TRUE; // return TRUE unless you set the focus to a control
	             // EXCEPTION: OCX Property Pages should return FALSE
}

void AddCheat::OnNumberType(UINT id)
{
	switch (id)
	{
	case IDC_SIGNED:
		numberType = 0;
		regSetDwordValue(_T("cheatsNumberType"), 0);
		break;
	case IDC_UNSIGNED:
		numberType = 1;
		regSetDwordValue(_T("cheatsNumberType"), 1);
		break;
	case IDC_HEXADECIMAL:
		numberType = 2;
		regSetDwordValue(_T("cheatsNumberType"), 2);
		break;
	}
}

void AddCheat::OnSizeType(UINT id)
{
	switch (id)
	{
	case IDC_SIZE_8:
		sizeType = BITS_8;
		regSetDwordValue(_T("cheatsSizeType"), 0);
		break;
	case IDC_SIZE_16:
		sizeType = BITS_16;
		regSetDwordValue(_T("cheatsSizeType"), 1);
		break;
	case IDC_SIZE_32:
		sizeType = BITS_32;
		regSetDwordValue(_T("cheatsSizeType"), 2);
		break;
	}
}

bool AddCheat::addCheat()
{
	CString buffer;
	CString code;

	if (editIndex < 0 && cheatsNumber >= MAX_CHEATS)
	{
		systemMessage(0, N_("The cheat list is full."));
		return false;
	}

	m_address.GetWindowText(buffer);
	buffer.Trim();
	if (buffer.IsEmpty())
	{
		systemMessage(0, N_("Address cannot be empty"));
		return false;
	}

	TCHAR *end = NULL;
	const TCHAR *start = buffer;
	u32 address = (u32)_tcstoul(start, &end, 16);
	if (end == start || *end != 0)
	{
		systemMessage(0, N_(_T("Invalid address: %s")), (LPCTSTR)buffer);
		return false;
	}

	if ((address >= 0x02000000 && address < 0x02040000) ||
	    (address >= 0x03000000 && address < 0x03008000))
	{}
	else
	{
		systemMessage(IDS_INVALID_ADDRESS, N_("Invalid address: %08x"), address);
		return false;
	}
	if (sizeType != 0)
	{
		if (sizeType == 1 && address & 1)
		{
			systemMessage(IDS_MISALIGNED_HALFWORD, N_("Misaligned half-word address: %08x"), address);
			return false;
		}
		if (sizeType == 2 && address & 3)
		{
			systemMessage(IDS_MISALIGNED_WORD, N_("Misaligned word address: %08x"), address);
			return false;
		}
	}
	u32 value = 0;
	m_value.GetWindowText(buffer);
	buffer.Trim();

	if (buffer.IsEmpty())
	{
		systemMessage(IDS_VALUE_CANNOT_BE_EMPTY, N_("Value cannot be empty"));
		return false;
	}

	end = NULL;
	start = buffer;
	switch (numberType)
	{
	case 0:
		value = (u32)_tcstol(start, &end, 10);
		break;
	case 1:
		value = (u32)_tcstoul(start, &end, 10);
		break;
	default:
		value = (u32)_tcstoul(start, &end, 16);
	}
	if (end == start || *end != 0)
	{
		systemMessage(0, N_(_T("Invalid value: %s")), (LPCTSTR)buffer);
		return false;
	}

	m_desc.GetWindowText(buffer);

	switch (sizeType)
	{
	case 0:
		code.Format(_T("%08x:%02x"), address, value);
		break;
	case 1:
		code.Format(_T("%08x:%04x"), address, value);
		break;
	case 2:
		code.Format(_T("%08x:%08x"), address, value);
		break;
	}

	if (editIndex >= 0 && editIndex < cheatsNumber)
		cheatsDelete(editIndex, false);

	int newIndex = cheatsNumber;

#ifdef USE_GBA_CORE_V7
    cheatsAdd(winAnsiFromWide(code), winAnsiFromWide(buffer), address, value, -1, sizeType);
#else
    cheatsAdd(winAnsiFromWide(code), winAnsiFromWide(buffer), address, address, value, -1, sizeType); // note one more address
#endif

	if (cheatsNumber > newIndex)
	{
		if (m_enabled.GetCheck() == BST_CHECKED)
			cheatsEnable(newIndex);
		else
			cheatsDisable(newIndex);
	}

	return true;
}

/////////////////////////////////////////////////////////////////////////////
// GBACheatList dialog

GBACheatList::GBACheatList(CWnd*pParent /*=NULL*/)
	: CDialog(GBACheatList::IDD, pParent)
{
	//{{AFX_DATA_INIT(GBACheatList)
	// NOTE: the ClassWizard will add member initialization here
	//}}AFX_DATA_INIT
	duringRefresh = false;
	restoreValues = false;
	groupCheats   = true;
}

void GBACheatList::DoDataExchange(CDataExchange*pDX)
{
	CDialog::DoDataExchange(pDX);
	//{{AFX_DATA_MAP(GBACheatList)
	DDX_Control(pDX, IDC_RESTORE, m_restore);
	DDX_Control(pDX, IDC_GROUP_CHEATS, m_group);
	DDX_Control(pDX, IDC_CHEAT_LIST, m_list);
	//}}AFX_DATA_MAP
}

BEGIN_MESSAGE_MAP(GBACheatList, CDialog)
//{{AFX_MSG_MAP(GBACheatList)
ON_BN_CLICKED(IDC_ADD_CHEAT, OnAddCheat)
ON_BN_CLICKED(IDC_ADD_CODE, OnAddCode)
ON_BN_CLICKED(IDC_ADD_CODEBREAKER, OnAddCodebreaker)
ON_BN_CLICKED(IDC_ADD_GAMESHARK, OnAddGameshark)
ON_BN_CLICKED(IDC_ENABLE, OnEnable)
ON_BN_CLICKED(IDC_EDIT, OnEdit)
ON_BN_CLICKED(IDC_EXPLAIN_CHEAT, OnExplain)
ON_BN_CLICKED(IDC_REMOVE, OnRemove)
ON_BN_CLICKED(IDC_RESTORE, OnRestore)
ON_BN_CLICKED(IDC_GROUP_CHEATS, OnGroupCheats)
ON_BN_CLICKED(ID_OK, OnOk)
ON_NOTIFY(LVN_ITEMCHANGING, IDC_CHEAT_LIST, OnItemchangingCheatList)
ON_NOTIFY(LVN_ITEMCHANGED, IDC_CHEAT_LIST, OnItemchangedCheatList)
ON_NOTIFY(NM_CLICK, IDC_CHEAT_LIST, OnClickCheatList)
//}}AFX_MSG_MAP
END_MESSAGE_MAP()

/////////////////////////////////////////////////////////////////////////////
// GBACheatList message handlers

void GBACheatList::OnAddCheat()
{
	AddCheat dlg(0);
	dlg.DoModal();
	refresh();
}

void GBACheatList::OnAddCode()
{
	AddCheatCode dlg;
	dlg.DoModal();
	refresh();
}

void GBACheatList::OnAddCodebreaker()
{
	AddCBACode dlg;
	dlg.DoModal();
	refresh();
}

void GBACheatList::OnAddGameshark()
{
	AddGSACode dlg;
	dlg.DoModal();
	refresh();
}

void GBACheatList::OnEnable()
{
	int mark  = m_list.GetSelectionMark();
	int count = m_list.GetItemCount();

	if (mark != -1)
	{
		std::vector<bool> handled(cheatsNumber, false);
		LVITEM item;
		for (int i = 0; i < count; i++)
		{
			memset(&item, 0, sizeof(item));
			item.mask      = LVIF_PARAM|LVIF_STATE;
			item.stateMask = LVIS_SELECTED;
			item.iItem     = i;
			if (m_list.GetItem(&item) && (item.state & LVIS_SELECTED))
			{
				if (rows[i].kind == GBACheatListRow::GROUP)
				{
					bool allEnabled = true;
					for (int j = rows[i].first; j < rows[i].first + rows[i].count; j++)
					{
						if (!cheatsList[j].enabled)
							allEnabled = false;
						handled[j] = true;
					}
					for (int j = rows[i].first; j < rows[i].first + rows[i].count; j++)
					{
						if (allEnabled)
							cheatsDisable(j);
						else
							cheatsEnable(j);
					}
				}
				else if (!handled[rows[i].first])
				{
					int index = rows[i].first;
					if (cheatsList[index].enabled)
						cheatsDisable(index);
					else
						cheatsEnable(index);
					handled[index] = true;
				}
			}
		}
		refresh();
	}
}

void GBACheatList::OnEdit()
{
	if (m_list.GetSelectedCount() != 1)
		return;

	int mark = m_list.GetSelectionMark();
	if (mark < 0 || mark >= (int)rows.size())
		return;

	if (rows[mark].kind == GBACheatListRow::GROUP)
		editGroup(mark);
	else
		editCheat(rows[mark].first);
}

void GBACheatList::OnExplain()
{
	if (m_list.GetSelectedCount() != 1)
		return;

	int mark = m_list.GetSelectionMark();
	if (mark < 0 || mark >= (int)rows.size())
		return;

	winShowCheatExplanation(this, rows[mark].first, rows[mark].count);
}

void GBACheatList::OnRemove()
{
	if (m_list.GetSelectedCount() == 0)
		return;

	int count = m_list.GetItemCount();
	std::vector<bool> remove(cheatsNumber, false);
	LVITEM item;
	for (int i = 0; i < count; i++)
	{
		memset(&item, 0, sizeof(item));
		item.mask      = LVIF_PARAM|LVIF_STATE;
		item.iItem     = i;
		item.stateMask = LVIS_SELECTED;
		if (m_list.GetItem(&item) && (item.state & LVIS_SELECTED))
		{
			if (rows[i].kind == GBACheatListRow::GROUP)
			{
				for (int j = rows[i].first; j < rows[i].first + rows[i].count; j++)
					remove[j] = true;
			}
			else
				remove[rows[i].first] = true;
		}
	}

	int deleteCount = 0;
	for (int i = 0; i < cheatsNumber; i++)
	{
		if (remove[i])
			deleteCount++;
	}
	if (deleteCount == 0)
		return;

	CString format = winResLoadString(IDS_CONFIRM_DELETE_CHEATS);
	CString message;
	message.Format((LPCTSTR)format, deleteCount);
	if (MessageBox(message, winResLoadString(IDS_CONFIRM_DELETE),
	               MB_YESNO | MB_ICONQUESTION) != IDYES)
		return;

	for (int i = cheatsNumber - 1; i >= 0; i--)
	{
		if (remove[i])
			cheatsDelete(i, restoreValues);
	}
	refresh();
}

void GBACheatList::OnRestore()
{
	restoreValues = !restoreValues;
	regSetDwordValue(_T("cheatsRestore"), restoreValues);
}

void GBACheatList::OnGroupCheats()
{
	groupCheats = !groupCheats;
	regSetDwordValue(_T("cheatsGroup"), groupCheats);
	refresh();
}

void GBACheatList::OnOk()
{
	EndDialog(TRUE);
}

void GBACheatList::selectRow(int index)
{
	if (index < 0 || index >= m_list.GetItemCount())
		return;

	m_list.SetItemState(index, LVIS_SELECTED | LVIS_FOCUSED,
	                    LVIS_SELECTED | LVIS_FOCUSED);
	m_list.SetSelectionMark(index);
	m_list.EnsureVisible(index, FALSE);
	updateButtonStates();
}

void GBACheatList::updateButtonStates()
{
	int selected = m_list.GetSelectedCount();
	GetDlgItem(IDC_REMOVE)->EnableWindow(selected > 0);
	GetDlgItem(IDC_EDIT)->EnableWindow(selected == 1);
	GetDlgItem(IDC_EXPLAIN_CHEAT)->EnableWindow(selected == 1);
	GetDlgItem(IDC_ENABLE)->EnableWindow(selected > 0);
}

void GBACheatList::OnItemchangingCheatList(NMHDR*pNMHDR, LRESULT*pResult)
{
	*pResult = FALSE;

	if (duringRefresh)
		return;

	LPNMLISTVIEW info = (LPNMLISTVIEW)pNMHDR;
	if (info->iItem < 0 || info->iItem >= (int)rows.size())
		return;
	if (!(info->uChanged & LVIF_STATE))
		return;
	if (((info->uOldState ^ info->uNewState) & LVIS_STATEIMAGEMASK) == 0)
		return;

	// The checkbox/arrow is handled explicitly in OnClickCheatList().
	// Rejecting the control's automatic state change also stops the native
	// checkbox from cycling into the arrow images.
	*pResult = TRUE;
}

void GBACheatList::OnItemchangedCheatList(NMHDR*, LRESULT*pResult)
{
	updateButtonStates();
	*pResult = 0;
}

void GBACheatList::OnClickCheatList(NMHDR*pNMHDR, LRESULT*pResult)
{
	LPNMITEMACTIVATE info = (LPNMITEMACTIVATE)pNMHDR;

	*pResult = 0;

	LVHITTESTINFO hit;
	memset(&hit, 0, sizeof(hit));
	hit.pt = info->ptAction;
	int index = m_list.HitTest(&hit);

	if (index < 0 || index >= (int)rows.size())
		return;

	bool onStateIcon = (hit.flags & LVHT_ONITEMSTATEICON) != 0;

	if (rows[index].kind == GBACheatListRow::GROUP)
	{
		if (!onStateIcon &&
		    ((GetKeyState(VK_CONTROL) & 0x8000) ||
		     (GetKeyState(VK_SHIFT) & 0x8000)))
			return;

		int first = rows[index].first;
		if (expandedGroups.count(first))
			expandedGroups.erase(first);
		else
			expandedGroups.insert(first);

		refresh();
		selectRow(index);
	}
	else if (onStateIcon)
	{
		int first = rows[index].first;
		if (cheatsList[first].enabled)
			cheatsDisable(first);
		else
			cheatsEnable(first);

		refresh();
		selectRow(index);
	}
}

BOOL GBACheatList::OnInitDialog()
{
	CDialog::OnInitDialog();

	CRect client;
	m_list.GetClientRect(&client);
	int total = client.Width() - GetSystemMetrics(SM_CXVSCROLL);
	if (total < 220)
		total = 220;

	int statusWidth = total / 10;
	if (statusWidth < 40)
		statusWidth = 40;
	int typeWidth = total / 4;
	if (typeWidth < 90)
		typeWidth = 90;

	int remaining = total - statusWidth - typeWidth;
	int codeWidth = remaining / 2;
	int descWidth = remaining - codeWidth;

	CString temp = winResLoadString(IDS_CODE);
	m_list.InsertColumn(0, temp, LVCFMT_LEFT, codeWidth, 0);
	temp = winResLoadString(IDS_DESCRIPTION);
	m_list.InsertColumn(1, temp, LVCFMT_LEFT, descWidth, 1);
	temp = winResLoadString(IDS_STATUS);
	m_list.InsertColumn(2, temp, LVCFMT_LEFT, statusWidth, 2);
	temp = winResLoadString(IDS_TYPE);
	m_list.InsertColumn(3, temp, LVCFMT_LEFT, typeWidth, 3);

	m_list.SetFont(CFont::FromHandle(uiFont::mono()),
	               TRUE);

	duringRefresh = false;
	m_list.SetExtendedStyle(LVS_EX_CHECKBOXES | LVS_EX_FULLROWSELECT);
	addCheatArrowImages(m_list);

	restoreValues = regQueryDwordValue(_T("cheatsRestore"), 0) ?
	                true : false;

	m_restore.SetCheck(restoreValues);

	groupCheats = regQueryDwordValue(_T("cheatsGroup"), 1) ?
	              true : false;

	m_group.SetCheck(groupCheats);

	m_tooltip.Create(this);
	m_tooltip.AddTool(GetDlgItem(IDC_RESTORE), IDS_RESTORE_TIP);
	m_tooltip.AddTool(GetDlgItem(IDC_GROUP_CHEATS), IDS_GROUP_CHEATS_TIP);

	refresh();
	CenterWindow();

	return TRUE; // return TRUE unless you set the focus to a control
	             // EXCEPTION: OCX Property Pages should return FALSE
}

BOOL GBACheatList::PreTranslateMessage(MSG* pMsg)
{
	if (m_tooltip.GetSafeHwnd())
		m_tooltip.RelayEvent(pMsg);

	return CDialog::PreTranslateMessage(pMsg);
}

void GBACheatList::rebuildRows()
{
	rows.clear();
	std::set<int> stillExpanded;

	if (!groupCheats)
	{
		for (int i = 0; i < cheatsNumber; i++)
		{
			GBACheatListRow row;
			row.kind     = GBACheatListRow::CHEAT;
			row.first    = i;
			row.count    = 1;
			row.expanded = false;
			rows.push_back(row);
		}
		return;
	}

	int i = 0;
	while (i < cheatsNumber)
	{
		int j = i + 1;
		while (j < cheatsNumber && gbaCheatsCanGroup(i, j))
			j++;

		int count = j - i;
		if (count >= 2)
		{
			bool expanded = expandedGroups.count(i) != 0;
			if (expanded)
				stillExpanded.insert(i);

			GBACheatListRow row;
			row.kind     = GBACheatListRow::GROUP;
			row.first    = i;
			row.count    = count;
			row.expanded = expanded;
			rows.push_back(row);

			if (expanded)
			{
				for (int k = i; k < j; k++)
				{
					GBACheatListRow code;
					code.kind     = GBACheatListRow::CHEAT;
					code.first    = k;
					code.count    = 1;
					code.expanded = false;
					rows.push_back(code);
				}
			}
			i = j;
		}
		else
		{
			GBACheatListRow row;
			row.kind     = GBACheatListRow::CHEAT;
			row.first    = i;
			row.count    = 1;
			row.expanded = false;
			rows.push_back(row);
			i++;
		}
	}

	expandedGroups.swap(stillExpanded);
}

void GBACheatList::refresh()
{
	duringRefresh = true;
	m_list.DeleteAllItems();

	rebuildRows();

	for (int i = 0; i < (int)rows.size(); i++)
	{
		const GBACheatListRow &row = rows[i];
		LVITEM item;

		memset(&item, 0, sizeof(item));
		item.mask     = LVIF_TEXT | LVIF_PARAM | LVIF_STATE;
		item.iItem    = i;
		item.iSubItem = 0;
		item.lParam   = row.first;

		if (row.kind == GBACheatListRow::GROUP)
		{
			// The state image shows the Unicode arrow; the Code text names the
			// action.  Together they read as "arrow [Expand]/[Collapse]".
			CString text = row.expanded ? "[Collapse]" : "[Expand]";
			item.pszText = const_cast<LPTSTR>((LPCTSTR)text);
			m_list.InsertItem(&item);

			m_list.SetItemText(i, 1, winWideFromAnsi(cheatsList[row.first].desc));

			bool anyEnabled = false;
			bool allEnabled = true;
			for (int j = row.first; j < row.first + row.count; j++)
			{
				if (cheatsList[j].enabled)
					anyEnabled = true;
				else
					allEnabled = false;
			}

			CString status = allEnabled ? _T("E") : (anyEnabled ? _T("-") : _T("D"));
			m_list.SetItemText(i, 2, status);
			m_list.SetItemText(i, 3, winWideFromAnsi(gbaCheatTypeName(cheatsList[row.first].code)));

			int state = row.expanded ? CHEAT_STATE_EXPANDED : CHEAT_STATE_COLLAPSED;
			m_list.SetItemState(i, INDEXTOSTATEIMAGEMASK(state), LVIS_STATEIMAGEMASK);
		}
		else
		{
			const CString codeText = winWideFromAnsi(cheatsList[row.first].codestring);
			item.pszText = const_cast<LPTSTR>((LPCTSTR)codeText);
			m_list.InsertItem(&item);

			m_list.SetItemText(i, 1, winWideFromAnsi(cheatsList[row.first].desc));
			CString status = cheatsList[row.first].enabled ? _T("E") : _T("D");
			m_list.SetItemText(i, 2, status);
			m_list.SetItemText(i, 3, winWideFromAnsi(gbaCheatTypeName(cheatsList[row.first].code)));

			m_list.SetCheck(i, cheatsList[row.first].enabled ? TRUE : FALSE);
		}
	}

	duringRefresh = false;
	updateButtonStates();
}

void GBACheatList::editCheat(int cheatIndex)
{
	if (cheatIndex < 0 || cheatIndex >= cheatsNumber)
		return;

	bool enabled = cheatsList[cheatIndex].enabled;
	GBACheatType type = gbaCheatType(cheatsList[cheatIndex].code);

	if (type == GBA_CHEAT_TYPE_CODE)
	{
		AddCheat dlg(0, this, cheatIndex, enabled);
		dlg.DoModal();
	}
	else if (type == GBA_CHEAT_TYPE_CODEBREAKER)
	{
		AddCBACode dlg(this, winWideFromAnsi(cheatsList[cheatIndex].codestring), winWideFromAnsi(cheatsList[cheatIndex].desc),
		               cheatIndex, 1, enabled);
		dlg.DoModal();
	}
	else
	{
		CString code = cheatsList[cheatIndex].codestring;
		if (cheatsList[cheatIndex].code == 257)
			code = gsaV3CodeForEdit(winAnsiFromWide(code));

		AddGSACode dlg(this, code, winWideFromAnsi(cheatsList[cheatIndex].desc),
		               cheatIndex, 1, enabled);
		dlg.DoModal();
	}
	refresh();
}

void GBACheatList::editGroup(int rowIndex)
{
	if (rowIndex < 0 || rowIndex >= (int)rows.size())
		return;

	const GBACheatListRow &row = rows[rowIndex];
	CString code;
	CString desc = cheatsList[row.first].desc;
	bool allEnabled = true;

	for (int i = row.first; i < row.first + row.count; i++)
	{
		if (i > row.first)
			code += "\r\n";

		CString line = cheatsList[i].codestring;
		if (cheatsList[i].code == 257)
			line = gsaV3CodeForEdit(winAnsiFromWide(line));
		code += line;

		if (!cheatsList[i].enabled)
			allEnabled = false;
	}

	GBACheatType type = gbaCheatType(cheatsList[row.first].code);
	if (type == GBA_CHEAT_TYPE_CODE)
	{
		AddCheatCode dlg(this, code, desc, row.first, row.count, allEnabled);
		dlg.DoModal();
	}
	else if (type == GBA_CHEAT_TYPE_CODEBREAKER)
	{
		AddCBACode dlg(this, code, desc, row.first, row.count, allEnabled);
		dlg.DoModal();
	}
	else
	{
		AddGSACode dlg(this, code, desc, row.first, row.count, allEnabled);
		dlg.DoModal();
	}
	refresh();
}

/////////////////////////////////////////////////////////////////////////////
// AddGSACode dialog

AddGSACode::AddGSACode(CWnd*pParent /*=NULL*/, LPCTSTR code /*=NULL*/, LPCTSTR desc /*=NULL*/,
                       int editIndex /*=-1*/, int editCount /*=1*/, bool initialEnabled /*=true*/)
	: CDialog(AddGSACode::IDD, pParent)
	, initialCode(code ? code : _T(""))
	, initialDesc(desc ? desc : _T(""))
	, editIndex(editIndex)
	, editCount(editCount)
	, initialEnabled(initialEnabled)
{
	//{{AFX_DATA_INIT(AddGSACode)
	// NOTE: the ClassWizard will add member initialization here
	//}}AFX_DATA_INIT
}

void AddGSACode::DoDataExchange(CDataExchange*pDX)
{
	CDialog::DoDataExchange(pDX);
	//{{AFX_DATA_MAP(AddGSACode)
	DDX_Control(pDX, IDC_DESC, m_desc);
	DDX_Control(pDX, IDC_CODE, m_code);
	DDX_Control(pDX, IDC_CHEAT_ENABLED, m_enabled);
	//}}AFX_DATA_MAP
}

BEGIN_MESSAGE_MAP(AddGSACode, CDialog)
//{{AFX_MSG_MAP(AddGSACode)
ON_BN_CLICKED(ID_OK, OnOk)
ON_BN_CLICKED(ID_CANCEL, OnCancel)
//}}AFX_MSG_MAP
END_MESSAGE_MAP()

/////////////////////////////////////////////////////////////////////////////
// AddGSACode message handlers

void AddGSACode::OnOk()
{
	CString desc;
	CString buffer;

	m_code.GetWindowText(buffer);
	m_desc.GetWindowText(desc);

	int newCount = 0;
	if (!processGsaCbaCodes(buffer, desc, false, &newCount))
		return;

	int available = MAX_CHEATS - cheatsNumber + (editIndex >= 0 ? editCount : 0);
	if (newCount > available)
	{
		systemMessage(0, N_("The cheat list is full."));
		return;
	}

	if (editIndex >= 0)
		deleteCheatRange(editIndex, editCount);

	int first = cheatsNumber;
	processGsaCbaCodes(buffer, desc, true);

	if (!m_enabled.GetCheck())
	{
		for (int i = first; i < cheatsNumber; i++)
			cheatsDisable(i);
	}

	EndDialog(TRUE);
}

void AddGSACode::OnCancel()
{
	EndDialog(FALSE);
}

BOOL AddGSACode::OnInitDialog()
{
	CDialog::OnInitDialog();

	m_code.LimitText(1024);
	m_desc.LimitText(31);

	CString title;
	if (editIndex >= 0)
	{
		m_code.SetWindowText(initialCode);
		m_desc.SetWindowText(initialDesc);
		title = winResLoadString(IDS_EDIT_GSA_CODE);
	}
	else
	{
		m_desc.SetWindowText(CTime::GetCurrentTime().Format("%Y-%m-%d %H:%M:%S"));
		title = winResLoadString(IDS_ADD_GSA_CODE);
	}

	m_enabled.SetCheck(initialEnabled ? BST_CHECKED : BST_UNCHECKED);
	SetWindowText(title);
	CenterWindow();

	return TRUE; // return TRUE unless you set the focus to a control
	             // EXCEPTION: OCX Property Pages should return FALSE
}

/////////////////////////////////////////////////////////////////////////////
// AddCBACode dialog

AddCBACode::AddCBACode(CWnd*pParent /*=NULL*/, LPCTSTR code /*=NULL*/, LPCTSTR desc /*=NULL*/,
                       int editIndex /*=-1*/, int editCount /*=1*/, bool initialEnabled /*=true*/)
	: CDialog(AddCBACode::IDD, pParent)
	, initialCode(code ? code : _T(""))
	, initialDesc(desc ? desc : _T(""))
	, editIndex(editIndex)
	, editCount(editCount)
	, initialEnabled(initialEnabled)
{
	//{{AFX_DATA_INIT(AddCBACode)
	// NOTE: the ClassWizard will add member initialization here
	//}}AFX_DATA_INIT
}

void AddCBACode::DoDataExchange(CDataExchange*pDX)
{
	CDialog::DoDataExchange(pDX);
	//{{AFX_DATA_MAP(AddCBACode)
	DDX_Control(pDX, IDC_DESC, m_desc);
	DDX_Control(pDX, IDC_CODE, m_code);
	DDX_Control(pDX, IDC_CHEAT_ENABLED, m_enabled);
	//}}AFX_DATA_MAP
}

BEGIN_MESSAGE_MAP(AddCBACode, CDialog)
//{{AFX_MSG_MAP(AddCBACode)
ON_BN_CLICKED(ID_OK, OnOk)
ON_BN_CLICKED(ID_CANCEL, OnCancel)
//}}AFX_MSG_MAP
END_MESSAGE_MAP()

/////////////////////////////////////////////////////////////////////////////
// AddCBACode message handlers

void AddCBACode::OnOk()
{
	CString desc;
	CString buffer;

	m_code.GetWindowText(buffer);
	m_desc.GetWindowText(desc);

	int newCount = 0;
	if (!processGsaCbaCodes(buffer, desc, false, &newCount))
		return;

	int available = MAX_CHEATS - cheatsNumber + (editIndex >= 0 ? editCount : 0);
	if (newCount > available)
	{
		systemMessage(0, N_("The cheat list is full."));
		return;
	}

	if (editIndex >= 0)
		deleteCheatRange(editIndex, editCount);

	int first = cheatsNumber;
	processGsaCbaCodes(buffer, desc, true);

	if (!m_enabled.GetCheck())
	{
		for (int i = first; i < cheatsNumber; i++)
			cheatsDisable(i);
	}

	EndDialog(TRUE);
}

void AddCBACode::OnCancel()
{
	EndDialog(FALSE);
}

BOOL AddCBACode::OnInitDialog()
{
	CDialog::OnInitDialog();

	m_code.LimitText(1024);
	m_desc.LimitText(31);

	CString title;
	if (editIndex >= 0)
	{
		m_code.SetWindowText(initialCode);
		m_desc.SetWindowText(initialDesc);
		title = winResLoadString(IDS_EDIT_CBA_CODE);
	}
	else
	{
		m_desc.SetWindowText(CTime::GetCurrentTime().Format("%Y-%m-%d %H:%M:%S"));
		title = winResLoadString(IDS_ADD_CBA_CODE);
	}

	m_enabled.SetCheck(initialEnabled ? BST_CHECKED : BST_UNCHECKED);
	SetWindowText(title);
	CenterWindow();

	return TRUE; // return TRUE unless you set the focus to a control
	             // EXCEPTION: OCX Property Pages should return FALSE
}

/////////////////////////////////////////////////////////////////////////////
// AddCheatCode dialog

AddCheatCode::AddCheatCode(CWnd*pParent /*=NULL*/, LPCTSTR code /*=NULL*/, LPCTSTR desc /*=NULL*/,
                           int editIndex /*=-1*/, int editCount /*=1*/, bool initialEnabled /*=true*/)
	: CDialog(AddCheatCode::IDD, pParent)
	, initialCode(code ? code : _T(""))
	, initialDesc(desc ? desc : _T(""))
	, editIndex(editIndex)
	, editCount(editCount)
	, initialEnabled(initialEnabled)
{
	//{{AFX_DATA_INIT(AddCheatCode)
	// NOTE: the ClassWizard will add member initialization here
	//}}AFX_DATA_INIT
}

void AddCheatCode::DoDataExchange(CDataExchange*pDX)
{
	CDialog::DoDataExchange(pDX);
	//{{AFX_DATA_MAP(AddCheatCode)
	DDX_Control(pDX, IDC_DESC, m_desc);
	DDX_Control(pDX, IDC_CODE, m_code);
	DDX_Control(pDX, IDC_CHEAT_ENABLED, m_enabled);
	//}}AFX_DATA_MAP
}

BEGIN_MESSAGE_MAP(AddCheatCode, CDialog)
//{{AFX_MSG_MAP(AddCheatCode)
ON_BN_CLICKED(ID_OK, OnOk)
ON_BN_CLICKED(ID_CANCEL, OnCancel)
//}}AFX_MSG_MAP
END_MESSAGE_MAP()

/////////////////////////////////////////////////////////////////////////////
// AddCheatCode message handlers

void AddCheatCode::OnOk()
{
	CString desc;
	CString buffer;

	m_code.GetWindowText(buffer);
	m_desc.GetWindowText(desc);

	int newCount = 0;
	if (!processRawCheatCodes(buffer, desc, false, &newCount))
		return;

	int available = MAX_CHEATS - cheatsNumber + (editIndex >= 0 ? editCount : 0);
	if (newCount > available)
	{
		systemMessage(0, N_("The cheat list is full."));
		return;
	}

	if (editIndex >= 0)
		deleteCheatRange(editIndex, editCount);

	int first = cheatsNumber;
	processRawCheatCodes(buffer, desc, true);

	if (!m_enabled.GetCheck())
	{
		for (int i = first; i < cheatsNumber; i++)
			cheatsDisable(i);
	}

	EndDialog(TRUE);
}

void AddCheatCode::OnCancel()
{
	EndDialog(FALSE);
}

BOOL AddCheatCode::OnInitDialog()
{
	CDialog::OnInitDialog();

	m_code.LimitText(1024);
	m_desc.LimitText(31);

	CString title;
	if (editIndex >= 0)
	{
		m_code.SetWindowText(initialCode);
		m_desc.SetWindowText(initialDesc);
		title = winResLoadString(IDS_EDIT_CHEAT_CODE);
	}
	else
	{
		m_desc.SetWindowText(CTime::GetCurrentTime().Format("%Y-%m-%d %H:%M:%S"));
		title = winResLoadString(IDS_ADD_CHEAT_CODE);
	}

	m_enabled.SetCheck(initialEnabled ? BST_CHECKED : BST_UNCHECKED);
	SetWindowText(title);
	CenterWindow();

	return TRUE; // return TRUE unless you set the focus to a control
	             // EXCEPTION: OCX Property Pages should return FALSE
}

void GBACheatSearch::OnBnClickedCheatrefreshbutton()
{
	addChanges(false);
}
