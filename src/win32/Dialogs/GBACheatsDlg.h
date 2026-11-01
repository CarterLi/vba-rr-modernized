#if !defined(AFX_GBACHEATS_H__FC31D47D_52C8_42B2_95C7_7C3FD09316A4__INCLUDED_)
#define AFX_GBACHEATS_H__FC31D47D_52C8_42B2_95C7_7C3FD09316A4__INCLUDED_

#pragma once
// GBACheatsDlg.h : header file
//

#include <set>
#include <vector>

#include "../Controls/CheatCodeEdit.h"

/////////////////////////////////////////////////////////////////////////////
// GBACheatSearch dialog

struct WinCheatsData
{
	u32  addr;
	char address[9];
	char oldValue[12];
	char newValue[12];
};

class GBACheatSearch : public CDialog
{
	// Construction
public:
	afx_msg void OnSizeType(UINT id);
	afx_msg void OnNumberType(UINT id);
	afx_msg void OnSearchType(UINT id);
	afx_msg void OnValueType(UINT id);
	void addChange(int index, u32 address, u32 oldValue, u32 newValue);
	GBACheatSearch(CWnd*pParent = NULL);  // standard constructor
	~GBACheatSearch();

	// Dialog Data
	//{{AFX_DATA(GBACheatSearch)
	enum { IDD = IDD_CHEATS };
	CEdit     m_value;
	CListCtrl m_list;
	int       valueType;
	int       sizeType;
	int       searchType;
	int       numberType;
	BOOL      updateValues;
	//}}AFX_DATA

	// Overrides
	// ClassWizard generated virtual function overrides
	//{{AFX_VIRTUAL(GBACheatSearch)
protected:
	virtual void DoDataExchange(CDataExchange*pDX);   // DDX/DDV support
	//}}AFX_VIRTUAL

	// Implementation
protected:
	// Generated message map functions
	//{{AFX_MSG(GBACheatSearch)
	afx_msg void OnOk();
	afx_msg void OnStart();
	afx_msg void OnSearch();
	afx_msg void OnAddCheat();
	afx_msg void OnUpdate();
	afx_msg void OnGetdispinfoCheatList(NMHDR*pNMHDR, LRESULT*pResult);
	afx_msg void OnItemchangedCheatList(NMHDR*pNMHDR, LRESULT*pResult);
	virtual BOOL OnInitDialog();
	//}}AFX_MSG
	DECLARE_MESSAGE_MAP()
private:
	void addChanges(bool showMsgs);
	WinCheatsData *data;
public:
	afx_msg void OnClose();
	afx_msg void OnBnClickedCheatrefreshbutton();
};

/////////////////////////////////////////////////////////////////////////////
// AddCheat dialog

class AddCheat : public CDialog
{
	// Construction
public:
	bool addCheat();
	afx_msg void OnSizeType(UINT id);
	afx_msg void OnNumberType(UINT id);
	AddCheat(u32 address, CWnd*pParent = NULL, int editIndex = -1, bool initialEnabled = true);  // standard constructor

	// Dialog Data
	//{{AFX_DATA(AddCheat)
	enum { IDD = IDD_ADD_CHEAT };
	CEdit   m_value;
	CEdit   m_desc;
	CEdit   m_address;
	CButton m_enabled;
	int     sizeType;
	int     numberType;
	//}}AFX_DATA

	// Overrides
	// ClassWizard generated virtual function overrides
	//{{AFX_VIRTUAL(AddCheat)
protected:
	virtual void DoDataExchange(CDataExchange*pDX);   // DDX/DDV support
	//}}AFX_VIRTUAL

	// Implementation
protected:
	u32   address;
	int   editIndex;
	bool  initialEnabled;

	// Generated message map functions
	//{{AFX_MSG(AddCheat)
	afx_msg void OnOk();
	afx_msg void OnCancel();
	virtual BOOL OnInitDialog();
	//}}AFX_MSG
	DECLARE_MESSAGE_MAP()
};
/////////////////////////////////////////////////////////////////////////////
// GBACheatList dialog

struct GBACheatListRow
{
	enum { GROUP, CHEAT };

	int  kind;
	int  first;
	int  count;
	bool expanded;
};

class GBACheatList : public CDialog
{
	// Construction
public:
	void refresh();
	GBACheatList(CWnd*pParent = NULL);  // standard constructor

	// Dialog Data
	//{{AFX_DATA(GBACheatList)
	enum { IDD = IDD_CHEAT_LIST };
	CButton      m_restore;
	CButton      m_group;
	CListCtrl    m_list;
	CToolTipCtrl m_tooltip;
	//}}AFX_DATA

	// Overrides
	// ClassWizard generated virtual function overrides
	//{{AFX_VIRTUAL(GBACheatList)
protected:
	virtual void DoDataExchange(CDataExchange*pDX);   // DDX/DDV support
	//}}AFX_VIRTUAL

	// Implementation
protected:
	bool duringRefresh;
	bool restoreValues;
	bool groupCheats;
	std::vector<GBACheatListRow> rows;
	std::set<int> expandedGroups;

	void rebuildRows();
	void selectRow(int index);
	void updateButtonStates();
	void editGroup(int rowIndex);
	void editCheat(int cheatIndex);

	// Generated message map functions
	//{{AFX_MSG(GBACheatList)
	afx_msg void OnAddCheat();
	afx_msg void OnAddCode();
	afx_msg void OnAddGameshark();
	afx_msg void OnLoadDatabase();
	afx_msg void OnEnable();
	afx_msg void OnEdit();
	afx_msg void OnExplain();
	afx_msg void OnRemove();
	afx_msg void OnRestore();
	afx_msg void OnGroupCheats();
	afx_msg void OnOk();
	afx_msg void OnItemchangingCheatList(NMHDR*pNMHDR, LRESULT*pResult);
	afx_msg void OnItemchangedCheatList(NMHDR*pNMHDR, LRESULT*pResult);
	afx_msg void OnClickCheatList(NMHDR*pNMHDR, LRESULT*pResult);
	virtual BOOL OnInitDialog();
	virtual BOOL PreTranslateMessage(MSG* pMsg);
	//}}AFX_MSG
	DECLARE_MESSAGE_MAP()
};
/////////////////////////////////////////////////////////////////////////////
// AddGSACode dialog

class AddGSACode : public CDialog
{
	// Construction
public:
	AddGSACode(CWnd*pParent = NULL, LPCTSTR code = NULL, LPCTSTR desc = NULL,
	           int editIndex = -1, int editCount = 1, bool initialEnabled = true);

	// Dialog Data
	//{{AFX_DATA(AddGSACode)
	enum { IDD = IDD_ADD_CHEAT_DLG };
	CEdit          m_desc;
	CCheatCodeEdit m_code;
	CButton        m_enabled;
	//}}AFX_DATA

	// Overrides
	// ClassWizard generated virtual function overrides
	//{{AFX_VIRTUAL(AddGSACode)
protected:
	virtual void DoDataExchange(CDataExchange*pDX);   // DDX/DDV support
	//}}AFX_VIRTUAL

	// Implementation
protected:
	CString initialCode;
	CString initialDesc;
	int     editIndex;
	int     editCount;
	bool    initialEnabled;

	// Generated message map functions
	//{{AFX_MSG(AddGSACode)
	afx_msg void OnOk();
	afx_msg void OnCancel();
	virtual BOOL OnInitDialog();
	//}}AFX_MSG
	DECLARE_MESSAGE_MAP()
};

/////////////////////////////////////////////////////////////////////////////
// AddCBACode dialog

class AddCBACode : public CDialog
{
	// Construction
public:
	AddCBACode(CWnd*pParent = NULL, LPCTSTR code = NULL, LPCTSTR desc = NULL,
	           int editIndex = -1, int editCount = 1, bool initialEnabled = true);

	// Dialog Data
	//{{AFX_DATA(AddCBACode)
	enum { IDD = IDD_ADD_CHEAT_DLG };
	CEdit          m_desc;
	CCheatCodeEdit m_code;
	CButton        m_enabled;
	//}}AFX_DATA

	// Overrides
	// ClassWizard generated virtual function overrides
	//{{AFX_VIRTUAL(AddCBACode)
protected:
	virtual void DoDataExchange(CDataExchange*pDX);   // DDX/DDV support
	//}}AFX_VIRTUAL

	// Implementation
protected:
	CString initialCode;
	CString initialDesc;
	int     editIndex;
	int     editCount;
	bool    initialEnabled;

	// Generated message map functions
	//{{AFX_MSG(AddCBACode)
	afx_msg void OnOk();
	afx_msg void OnCancel();
	virtual BOOL OnInitDialog();
	//}}AFX_MSG
	DECLARE_MESSAGE_MAP()
};

/////////////////////////////////////////////////////////////////////////////
// AddCheatCode dialog

class AddCheatCode : public CDialog
{
	// Construction
public:
	AddCheatCode(CWnd*pParent = NULL, LPCTSTR code = NULL, LPCTSTR desc = NULL,
	             int editIndex = -1, int editCount = 1, bool initialEnabled = true);

	// Dialog Data
	//{{AFX_DATA(AddCheatCode)
	enum { IDD = IDD_ADD_CHEAT_DLG };
	CEdit          m_desc;
	CCheatCodeEdit m_code;
	CButton        m_enabled;
	//}}AFX_DATA

	// Overrides
	// ClassWizard generated virtual function overrides
	//{{AFX_VIRTUAL(AddCheatCode)
protected:
	virtual void DoDataExchange(CDataExchange*pDX);   // DDX/DDV support
	//}}AFX_VIRTUAL

	// Implementation
protected:
	CString initialCode;
	CString initialDesc;
	int     editIndex;
	int     editCount;
	bool    initialEnabled;

	// Generated message map functions
	//{{AFX_MSG(AddCheatCode)
	afx_msg void OnOk();
	afx_msg void OnCancel();
	virtual BOOL OnInitDialog();
	//}}AFX_MSG
	DECLARE_MESSAGE_MAP()
};
//{{AFX_INSERT_LOCATION}}
// Microsoft Visual C++ will insert additional declarations immediately before the previous line.

#endif // !defined(AFX_GBACHEATS_H__FC31D47D_52C8_42B2_95C7_7C3FD09316A4__INCLUDED_)
