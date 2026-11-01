// CheatDbDlg.cpp : import cheats from a cheat database.
//
// Two sources are supported, and both end up in the same list:
//   an EZ-Flash Omega package (common/ezflashdb.cpp), which stores raw addresses
//   that the reader translates into ready to use writes;
//   a libretro-database .cht file (common/libretrocht.cpp), which stores
//   CodeBreaker / GameShark codes, so the text is handed to the engine's parser.
//
// Neither reader knows about MFC.  This file is the front end: it asks for the
// file, turns a parser result into a dialog or a message, and writes the ticked
// cheats into the list through the engine entry points the cheat editor already
// uses, so the rest of the emulator sees nothing new.

#include "../stdafx.h"
#include "../resource.h"
#include "CheatDbDlg.h"
#include "FileDlg.h"
#include "../VBA.h"
#include "../WinResUtil.h"
#include "../WinText.h"

#include "../../common/SystemGlobals.h"
#include "../../common/cheatcodetext.h"
#include "../../common/ezflashdb.h"
#include "../../common/libretrocht.h"
#include "../../gba/GBA.h"
#include "../../gba/GBAGlobals.h"
#include "../../gba/GBACheats.h"

#include <cstdio>
#include <string>
#include <vector>

namespace
{

// One skipped line, as the details window shows it.  The parser only reports a
// kind and the offending text, so the sentence is chosen here and translated
// like everything else.
CString cheatDbProblemText(const CheatDbProblem &problem)
{
	UINT id = IDS_CHEAT_DB_PROBLEM_LINE;
	switch (problem.kind)
	{
	case CHEAT_DB_PROBLEM_SECTION: id = IDS_CHEAT_DB_PROBLEM_SECTION; break;
	case CHEAT_DB_PROBLEM_ADDRESS: id = IDS_CHEAT_DB_PROBLEM_ADDRESS; break;
	case CHEAT_DB_PROBLEM_VALUE:   id = IDS_CHEAT_DB_PROBLEM_VALUE;   break;
	case CHEAT_DB_PROBLEM_EMPTY:   id = IDS_CHEAT_DB_PROBLEM_EMPTY;   break;
	case CHEAT_DB_PROBLEM_LIMIT:   id = IDS_CHEAT_DB_PROBLEM_LIMIT;   break;
	case CHEAT_DB_PROBLEM_CODE:    id = IDS_CHEAT_DB_PROBLEM_CODE;    break;
	default:                 id = IDS_CHEAT_DB_PROBLEM_LINE;    break;
	}

	CString text;
	text.Format(winResLoadString(id), problem.line,
	            (LPCTSTR)winWideFromAnsi(problem.text));
	return text;
}

CString cheatDbErrorText(CheatDbError error, const std::string &detail)
{
	UINT id = IDS_CHEAT_DB_ERR_FILE;
	switch (error)
	{
	case CHEAT_DB_ERR_ARCHIVE:    id = IDS_CHEAT_DB_ERR_ARCHIVE;    break;
	case CHEAT_DB_ERR_INDEX:      id = IDS_CHEAT_DB_ERR_INDEX;      break;
	case CHEAT_DB_ERR_INDEX_DATA: id = IDS_CHEAT_DB_ERR_INDEX_DATA; break;
	case CHEAT_DB_ERR_ENTRY:
	case CHEAT_DB_ERR_GAME_FILE:  id = IDS_CHEAT_DB_ERR_ENTRY;      break;
	default:                id = IDS_CHEAT_DB_ERR_FILE;       break;
	}

	CString text;
	text.Format(winResLoadString(id), (LPCTSTR)winWideFromAnsi(detail));
	return text;
}

// "Nothing usable in there", with the number of entries that had to be dropped
// on the way.
void cheatDbShowEmpty(CWnd *parent, const CheatDbFile &file, const std::string &detail)
{
	CString text;
	text.Format(winResLoadString(IDS_CHEAT_DB_EMPTY), (LPCTSTR)winWideFromAnsi(detail));
	if (!file.problems.empty())
	{
		CString skipped;
		skipped.Format(winResLoadString(IDS_CHEAT_DB_SKIPPED), (int)file.problems.size());
		text += _T("\r\n\r\n");
		text += skipped;
	}
	parent->MessageBox(text, NULL, MB_ICONERROR);
}

// The package separates words with '_' because it doubles as a file name.
CString cheatDbSpaced(const std::string &text)
{
	CString result = winWideFromAnsi(text);
	result.Replace(_T('_'), _T(' '));
	return result;
}

// "Max Hearts", or "Money: 9,999,999" for a section that offers a choice.
CString cheatDbEntryName(const CheatDbEntry &entry)
{
	CString name = cheatDbSpaced(entry.section);
	if (entry.useKeyName)
	{
		name += _T(": ");
		name += cheatDbSpaced(entry.key);
	}
	return name;
}

// CheatsData::desc holds 32 bytes including the terminator.
std::string cheatDbEntryDesc(const CheatDbEntry &entry)
{
	std::string desc = entry.section;
	for (size_t i = 0; i < desc.size(); i++)
	{
		if (desc[i] == '_')
			desc[i] = ' ';
	}
	if (entry.useKeyName)
	{
		std::string key = entry.key;
		for (size_t i = 0; i < key.size(); i++)
		{
			if (key[i] == '_')
				key[i] = ' ';
		}
		desc += ": ";
		desc += key;
	}
	if (desc.size() > 31)
		desc.resize(31);
	return desc;
}

CString cheatDbCodeString(const CheatDbWrite &write)
{
	char code[24];
	if (write.size == 1)
		sprintf(code, "%08X:%02X", write.address, write.value & 0xFF);
	else if (write.size == 2)
		sprintf(code, "%08X:%04X", write.address, write.value & 0xFFFF);
	else
		sprintf(code, "%08X:%08X", write.address, write.value);
	return winWideFromAnsi(code);
}

// Where the row will end up writing.  A cheat that patches two places far apart
// shows the first address plus a count; the whole code string is what actually
// gets added.  Sources that store code text start each code with its address, so
// the first eight characters are what the user wants to see.
CString cheatDbAddressText(const CheatDbEntry &entry, const std::vector<CheatCodeText> &codes)
{
	char text[32];

	if (!codes.empty())
	{
		std::string address = codes[0].text;
		if (address.size() > 8)
			address.resize(8);
		if (codes.size() == 1)
			sprintf(text, "%s", address.c_str());
		else
			sprintf(text, "%s +%d", address.c_str(), (int)codes.size() - 1);
		return winWideFromAnsi(text);
	}

	if (entry.writes.empty())
		return CString();
	if (entry.writes.size() == 1)
		sprintf(text, "%08X", entry.writes[0].address);
	else
		sprintf(text, "%08X +%d", entry.writes[0].address, (int)entry.writes.size() - 1);
	return winWideFromAnsi(text);
}

// Memory areas the package can mean with its truncated addresses.  0 is "leave
// them alone", the rest replace the bank and page the file dropped.
unsigned int cheatDbAreaBase(int selection)
{
	static const unsigned int bases[] = { 0, 0x03000000, 0x02000000, 0x02010000,
	                                      0x02020000, 0x02030000 };
	if (selection < 0 || selection >= (int)(sizeof(bases) / sizeof(bases[0])))
		return 0;
	return bases[selection];
}

/////////////////////////////////////////////////////////////////////////////
// CheatDbDlg

class CheatDbDlg : public CDialog
{
public:
	// `db` is the package the file came from, or NULL for a libretro file: only
	// the package loses the memory area of its addresses, so only it can be
	// re-read with another one.
	CheatDbDlg(EzCheatDb *db, const CheatDbFile &file, int indexSkipped, CWnd *parent)
	    : CDialog(CheatDbDlg::IDD, parent), m_db(db), m_file(file), m_indexSkipped(indexSkipped),
	      m_added(0), m_problemBase(file.problems.size())
	{
	}

	// A package .cht read straight from disk: there is no package to ask again,
	// but the text is small enough to keep, so the memory area can still be
	// changed.  Used when the file turns out to be in the package's format.
	CheatDbDlg(const std::string &ezText, const CheatDbFile &file, CWnd *parent)
	    : CDialog(CheatDbDlg::IDD, parent), m_db(NULL), m_ezText(ezText), m_file(file),
	      m_indexSkipped(0), m_added(0), m_problemBase(file.problems.size())
	{
	}

	enum { IDD = IDD_CHEAT_DB };

	int AddedCount() const { return m_added; }

protected:
	virtual BOOL OnInitDialog();
	virtual void DoDataExchange(CDataExchange *pDX);
	virtual void OnOK();
	virtual void OnCancel();

	afx_msg void OnAdd();
	afx_msg void OnDetails();
	afx_msg void OnAreaChanged();

	DECLARE_MESSAGE_MAP()

private:
	// One line of the list.  `codes` is filled for sources that store code text
	// and empty for the package, whose entries carry ready made writes.
	struct Row
	{
		int                        entry;
		std::vector<CheatCodeText> codes;
	};

	void buildRows();
	void fillList();
	void updateStatus();

	EzCheatDb   *m_db;
	std::string  m_ezText;        // set only for a lone package file, to re-parse it
	CheatDbFile  m_file;
	int          m_indexSkipped;
	int          m_added;
	size_t       m_problemBase;   // problems the reader reported, before the UI adds any

	std::vector<Row> m_rows;

	CListCtrl m_list;
	CButton   m_details;
	CComboBox m_area;
};

BEGIN_MESSAGE_MAP(CheatDbDlg, CDialog)
ON_BN_CLICKED(ID_OK, OnAdd)
ON_BN_CLICKED(ID_CANCEL, OnCancel)
ON_BN_CLICKED(IDC_CHEAT_DB_PROBLEMS, OnDetails)
ON_CBN_SELCHANGE(IDC_CHEAT_DB_AREA, OnAreaChanged)
END_MESSAGE_MAP()

void CheatDbDlg::DoDataExchange(CDataExchange *pDX)
{
	CDialog::DoDataExchange(pDX);
	DDX_Control(pDX, IDC_CHEAT_DB_LIST, m_list);
	DDX_Control(pDX, IDC_CHEAT_DB_PROBLEMS, m_details);
	DDX_Control(pDX, IDC_CHEAT_DB_AREA, m_area);
}

void CheatDbDlg::OnAdd()
{
	OnOK();
}

void CheatDbDlg::OnCancel()
{
	EndDialog(IDCANCEL);
}

BOOL CheatDbDlg::OnInitDialog()
{
	CDialog::OnInitDialog();

	m_list.SetExtendedStyle(LVS_EX_CHECKBOXES | LVS_EX_FULLROWSELECT | LVS_EX_LABELTIP);

	CRect client;
	m_list.GetClientRect(&client);
	int width = client.Width() - GetSystemMetrics(SM_CXVSCROLL) - 24;
	if (width < 120)
		width = 120;
	int addressWidth = 88;
	if (addressWidth > width / 2)
		addressWidth = width / 2;
	m_list.InsertColumn(0, winResLoadString(IDS_DESCRIPTION), LVCFMT_LEFT, width - addressWidth, 0);
	m_list.InsertColumn(1, winResLoadString(IDS_CHEAT_DB_ADDRESS), LVCFMT_LEFT, addressWidth, 1);

	// The package dropped the memory area of the addresses it stores short; this
	// says which one to use instead.  Nothing to choose when no address was cut.
	m_area.AddString(winResLoadString(IDS_CHEAT_DB_AREA_FILE));
	m_area.AddString(winResLoadString(IDS_CHEAT_DB_AREA_IWRAM));
	m_area.AddString(winResLoadString(IDS_CHEAT_DB_AREA_EW0));
	m_area.AddString(winResLoadString(IDS_CHEAT_DB_AREA_EW1));
	m_area.AddString(winResLoadString(IDS_CHEAT_DB_AREA_EW2));
	m_area.AddString(winResLoadString(IDS_CHEAT_DB_AREA_EW3));
	m_area.SetCurSel(0);
	m_area.EnableWindow((m_db != NULL || !m_ezText.empty()) && m_file.truncatedAddresses > 0);

	CString game = cheatDbSpaced(m_file.gameName);
	if (!game.IsEmpty())
	{
		CString title;
		title.Format(winResLoadString(IDS_CHEAT_DB_TITLE), (LPCTSTR)game);
		SetWindowText(title);
	}

	buildRows();
	fillList();

	m_details.EnableWindow(!m_file.problems.empty() || m_indexSkipped > 0);

	CenterWindow();
	return TRUE;
}

// Turns the parsed entries into list rows.  Package entries are already usable;
// code text has to be split and offered to the engine first, because the code's
// type nibble decides what it does and only the engine knows which types it can
// execute.
void CheatDbDlg::buildRows()
{
	m_rows.clear();
	m_file.problems.resize(m_problemBase);

	for (size_t i = 0; i < m_file.entries.size(); i++)
	{
		const CheatDbEntry &entry = m_file.entries[i];

		Row row;
		row.entry = (int)i;

		if (!entry.code.empty())
		{
			std::vector<CheatCodeText> candidates;
			CheatSplitCodes(entry.code.c_str(), candidates);

			for (size_t c = 0; c < candidates.size(); c++)
			{
				const char *code = candidates[c].text.c_str();
				const bool usable = candidates[c].gsa ? cheatsValidateGSACode(code, false)
				                                      : cheatsValidateCBACode(code, false);
				if (usable)
					row.codes.push_back(candidates[c]);
			}

			if (row.codes.empty())
			{
				// Nothing in this entry can be executed, so it is not offered.
				CheatDbProblem problem;
				problem.line = (int)i + 1;
				problem.kind = CHEAT_DB_PROBLEM_CODE;
				problem.text = entry.code.size() > 64 ? entry.code.substr(0, 64) : entry.code;
				m_file.problems.push_back(problem);
				continue;
			}
		}

		m_rows.push_back(row);
	}
}

void CheatDbDlg::fillList()
{
	m_list.DeleteAllItems();
	for (size_t i = 0; i < m_rows.size(); i++)
	{
		const CheatDbEntry &entry = m_file.entries[m_rows[i].entry];
		int item = m_list.InsertItem((int)i, cheatDbEntryName(entry));
		m_list.SetItemText(item, 1, cheatDbAddressText(entry, m_rows[i].codes));
	}
	updateStatus();
}

void CheatDbDlg::updateStatus()
{
	CString status = cheatDbSpaced(m_file.title);
	CString count;
	count.Format(winResLoadString(IDS_CHEAT_DB_COUNT), (int)m_rows.size());
	if (!status.IsEmpty())
		status += _T("  |  ");
	status += count;

	if (m_file.truncatedAddresses > 0)
	{
		CString truncated;
		truncated.Format(winResLoadString(IDS_CHEAT_DB_TRUNCATED), m_file.truncatedAddresses);
		status += _T("  |  ");
		status += truncated;
	}
	if (!m_file.problems.empty())
	{
		CString skipped;
		skipped.Format(winResLoadString(IDS_CHEAT_DB_SKIPPED), (int)m_file.problems.size());
		status += _T("  |  ");
		status += skipped;
	}
	if (m_indexSkipped > 0)
	{
		CString skipped;
		skipped.Format(winResLoadString(IDS_CHEAT_DB_INDEX_SKIPPED), m_indexSkipped);
		status += _T("  |  ");
		status += skipped;
	}
	SetDlgItemText(IDC_CHEAT_DB_STATUS, status);
}

void CheatDbDlg::OnAreaChanged()
{
	// Libretro addresses are absolute, so only a package file has a choice; it
	// either comes from an opened package or from text kept in memory.
	if (!m_db && m_ezText.empty())
		return;

	const unsigned int base = cheatDbAreaBase(m_area.GetCurSel());

	std::vector<bool> checked((size_t)m_list.GetItemCount(), false);
	for (int i = 0; i < m_list.GetItemCount(); i++)
		checked[i] = (m_list.GetCheck(i) != FALSE);

	// Same text and same reader, so this only fails when the file made no sense
	// in the first place; keep the current reading rather than lose the list.
	const bool ok = m_db ? (m_db->ReloadWithBase(base, m_file) == CHEAT_DB_OK)
	                     : EzParseChtText(m_ezText.data(), m_ezText.size(), m_file, base);
	if (!ok)
	{
		m_area.SetCurSel(0);
		return;
	}

	buildRows();
	fillList();
	for (int i = 0; i < m_list.GetItemCount() && i < (int)checked.size(); i++)
		m_list.SetCheck(i, checked[i]);
}

void CheatDbDlg::OnOK()
{
	// The Add button is the default button, so this also runs on Enter.
	std::vector<int> picked;
	int              needed = 0;
	for (int i = 0; i < m_list.GetItemCount(); i++)
	{
		if (!m_list.GetCheck(i))
			continue;
		picked.push_back(i);
		const Row &row = m_rows[i];
		needed += row.codes.empty() ? (int)m_file.entries[row.entry].writes.size()
		                            : (int)row.codes.size();
	}

	if (picked.empty())
	{
		EndDialog(IDCANCEL);
		return;
	}

	if (needed > MAX_CHEATS - cheatsNumber)
	{
		CString text;
		text.Format(winResLoadString(IDS_CHEAT_DB_FULL), needed, MAX_CHEATS - cheatsNumber);
		MessageBox(text, NULL, MB_ICONEXCLAMATION);
		return;
	}

	const int before = cheatsNumber;
	for (size_t p = 0; p < picked.size(); p++)
	{
		const Row          &row = m_rows[picked[p]];
		const CheatDbEntry &entry = m_file.entries[row.entry];
		const std::string   desc = cheatDbEntryDesc(entry);

		if (!row.codes.empty())
		{
			for (size_t c = 0; c < row.codes.size(); c++)
			{
				if (row.codes[c].gsa)
					cheatsAddGSACode(row.codes[c].text.c_str(), desc.c_str(), row.codes[c].v3);
				else
					cheatsAddCBACode(row.codes[c].text.c_str(), desc.c_str());
			}
			continue;
		}

		for (size_t w = 0; w < entry.writes.size(); w++)
			cheatsAddCheatCode(winAnsiFromWide(cheatDbCodeString(entry.writes[w])),
			                   desc.c_str());
	}

	// Every generated code is one the engine already accepts, so anything short
	// here means the list filled up between the check above and the writes.
	// Every code handed over is one the engine already accepts, so anything short
	// here means the list filled up between the check above and the writes.  A
	// GameShark master code can add an entry of its own, hence "less than".
	const int added = cheatsNumber - before;
	if (added < needed)
	{
		CString text;
		text.Format(winResLoadString(IDS_CHEAT_DB_PARTIAL), needed - added, needed);
		MessageBox(text, NULL, MB_ICONWARNING);
	}

	m_added = (int)picked.size();
	EndDialog(IDOK);
}

void CheatDbDlg::OnDetails()
{
	CString text;
	const size_t limit = 40;
	for (size_t i = 0; i < m_file.problems.size() && i < limit; i++)
	{
		if (!text.IsEmpty())
			text += _T("\r\n");
		text += cheatDbProblemText(m_file.problems[i]);
	}
	if (m_file.problems.size() > limit)
	{
		text += _T("\r\n...");
	}
	if (m_indexSkipped > 0)
	{
		CString skipped;
		skipped.Format(winResLoadString(IDS_CHEAT_DB_INDEX_SKIPPED), m_indexSkipped);
		if (!text.IsEmpty())
			text += _T("\r\n\r\n");
		text += skipped;
	}

	if (text.IsEmpty())
		text = winResLoadString(IDS_NONE);

	MessageBox(text, winResLoadString(IDS_CHEAT_DB_DETAILS_TITLE), MB_ICONINFORMATION);
}

// Shows the cheats of one .cht file, whichever of the two formats it is in.
//
// Both the package and the libretro database name their files .cht, so the name
// says nothing and the bytes decide (CheatDbSniffFormat).  A lone package file
// is a normal way to use this: it needs no package around it, because every
// entry already carries its own address and data.
int cheatDbImportFileText(CWnd *parent, const CString &path, std::string &text)
{
	const std::string ansiPath = winAnsiFromWide(path);

	CheatDbFile   file;
	CheatDbFormat wanted = CheatDbSniffFormat(text.data(), text.size());
	CheatDbFormat used   = CHEAT_DB_FORMAT_UNKNOWN;

	// Try the format the marks point at first, then the other one: neither
	// reader invents entries out of the other's text, so the second attempt
	// costs nothing and rescues a file whose marks were edited away.
	const CheatDbFormat order[2] = {
		(wanted == CHEAT_DB_FORMAT_LIBRETRO) ? CHEAT_DB_FORMAT_LIBRETRO : CHEAT_DB_FORMAT_EZFLASH,
		(wanted == CHEAT_DB_FORMAT_LIBRETRO) ? CHEAT_DB_FORMAT_EZFLASH : CHEAT_DB_FORMAT_LIBRETRO
	};

	// The first attempt is the one that read the file the way its marks suggest,
	// so its problems are the meaningful diagnosis when both fail.
	std::vector<CheatDbProblem> firstProblems;
	bool                        haveFirst = false;

	for (int attempt = 0; attempt < 2; attempt++)
	{
		const bool parsed = (order[attempt] == CHEAT_DB_FORMAT_EZFLASH)
		                        ? EzParseChtText(text.data(), text.size(), file)
		                        : LrParseChtText(text.data(), text.size(), file);
		if (parsed)
		{
			used = order[attempt];
			break;
		}
		if (!haveFirst)
		{
			firstProblems = file.problems;
			haveFirst = true;
		}
	}

	if (used == CHEAT_DB_FORMAT_UNKNOWN)
	{
		if (haveFirst)
			file.problems = firstProblems;
		cheatDbShowEmpty(parent, file, ansiPath);
		return 0;
	}

	// A lone file is not named after its game the way the package's files are,
	// so the file name is the best label there is; it also replaces an empty
	// title, which would leave the status line starting with a separator.
	const int slash = path.ReverseFind(_T('\\'));
	CString   stem = (slash >= 0) ? path.Mid(slash + 1) : path;
	const int dot = stem.ReverseFind(_T('.'));
	if (dot > 0)
		stem = stem.Left(dot);

	if (used == CHEAT_DB_FORMAT_LIBRETRO || file.title.empty())
		file.title = winAnsiFromWide(stem);
	if (file.gameName.empty())
		file.gameName = winAnsiFromWide(stem);

	// Keep the text for the package format so the memory area can still be
	// changed; a libretro file has absolute addresses and needs none of that.
	if (used == CHEAT_DB_FORMAT_EZFLASH)
	{
		CheatDbDlg dlg(text, file, parent);
		if (dlg.DoModal() != IDOK)
			return 0;
		return dlg.AddedCount();
	}

	CheatDbDlg dlg(NULL, file, 0, parent);
	if (dlg.DoModal() != IDOK)
		return 0;
	return dlg.AddedCount();
}

} // namespace

int winLoadCheatsFromDatabase(CWnd *parent, bool askForFile)
{
	if (systemCartridgeType != IMAGE_GBA || rom == NULL)
	{
		if (parent)
			parent->MessageBox(winResLoadString(IDS_CHEAT_DB_NEEDS_GBA), NULL,
			                   MB_ICONINFORMATION);
		return 0;
	}

	CString path = theApp.cheatDbPath;
	if (askForFile || path.IsEmpty())
	{
		LPCTSTR exts[] = { _T(".zip"), _T(".cht"), NULL };

		CString name;
		CString dir;
		int     slash = path.ReverseFind(_T('\\'));
		if (slash > 0)
		{
			dir = path.Left(slash);
			name = path.Mid(slash + 1);
		}
		else
		{
			name = path;
		}

		FileDlg dlg(parent, name, winResLoadFilter(IDS_FILTER_CHEAT_DATABASE), 0, _T("ZIP"),
		            exts, dir, winResLoadString(IDS_CHEAT_DB_CHOOSE), false);
		if (dlg.DoModal() != IDOK)
			return 0;
		path = dlg.GetPathName();
	}

	// A single .cht is not routed by its extension: the EZ-Flash package ships
	// its files as .cht too, so the bytes have to say which format it is.
	CString extension = path.Right(4);
	extension.MakeLower();
	if (extension == _T(".cht"))
	{
		std::string  text;
		CheatDbError error = CHEAT_DB_ERR_NONE;
		std::string  detail;
		if (!CheatDbReadTextFile(winAnsiFromWide(path), text, error, detail))
		{
			parent->MessageBox(cheatDbErrorText(error, detail), NULL, MB_ICONERROR);
			return 0;
		}
		theApp.cheatDbPath = path;
		return cheatDbImportFileText(parent, path, text);
	}

	EzCheatDb    db;
	CheatDbError   error = CHEAT_DB_ERR_NONE;
	std::string detail;
	const std::string ansiPath = winAnsiFromWide(path);
	if (!db.Open(ansiPath.c_str(), error, detail))
	{
		// A zip holding plain .cht files is not an indexed package, so there is
		// no game code to look up.  One file means there is nothing to choose.
		if (error == CHEAT_DB_ERR_INDEX)
		{
			std::string  text;
			int          count = 0;
			CheatDbError loneError = CHEAT_DB_ERR_NONE;
			std::string  loneDetail;
			const EzLoneChtResult lone =
			    EzReadLoneCht(ansiPath.c_str(), text, count, loneError, loneDetail);

			if (lone == EZ_LONE_READ)
			{
				theApp.cheatDbPath = path;
				return cheatDbImportFileText(parent, path, text);
			}
			if (lone == EZ_LONE_SEVERAL)
			{
				CString message;
				message.Format(winResLoadString(IDS_CHEAT_DB_MANY_FILES), count,
				               (LPCTSTR)winWideFromAnsi(ansiPath));
				parent->MessageBox(message, NULL, MB_ICONERROR);
				return 0;
			}
			// EZ_LONE_NONE and EZ_LONE_FAILED both end at the index message
			// below, which is the real problem with this file.
		}
		parent->MessageBox(cheatDbErrorText(error, detail), NULL, MB_ICONERROR);
		return 0;
	}
	theApp.cheatDbPath = path;

	// rom[0xac] is the four character game code the package indexes by, the same
	// field vba-over.ini uses.
	char gameCode[5] = { 0, 0, 0, 0, 0 };
	memcpy(gameCode, &rom[0xac], 4);
	for (int i = 0; i < 4; i++)
	{
		if (gameCode[i] < 0x20 || gameCode[i] > 0x7E)
			gameCode[i] = '?';
	}

	CheatDbFile file;
	switch (db.LoadForGameCode(gameCode, file, error, detail))
	{
	case CHEAT_DB_OK:
		break;

	case CHEAT_DB_NO_CHEATS:
	{
		CString text;
		text.Format(winResLoadString(IDS_CHEAT_DB_NO_CHEATS),
		            (LPCTSTR)winWideFromAnsi(gameCode));
		parent->MessageBox(text, NULL, MB_ICONINFORMATION);
		return 0;
	}

	case CHEAT_DB_EMPTY:
		cheatDbShowEmpty(parent, file, detail);
		return 0;

	default:
		parent->MessageBox(cheatDbErrorText(error, detail), NULL, MB_ICONERROR);
		return 0;
	}

	CheatDbDlg dlg(&db, file, db.IndexSkippedRecords(), parent);
	if (dlg.DoModal() != IDOK)
		return 0;
	return dlg.AddedCount();
}
