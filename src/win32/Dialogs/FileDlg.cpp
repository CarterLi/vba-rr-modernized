// FileDlg.cpp: implementation of the FileDlg class.
//
// The dialog itself is the shell dialog of Windows Vista and later, see
// ModernFileDialog.cpp; this class only carries the parameters the rest of
// the emulator passes around and remembers the picked path.
//
// Dialog parameters and paths stay UTF-16 through both the shell dialog and
// the legacy common dialog, so localized text is not limited by the system
// ANSI code page.
//
//////////////////////////////////////////////////////////////////////
#include "../stdafx.h"
#include "FileDlg.h"
#include "ModernFileDialog.h"
#include "../Sound.h"
#include "../../common/System.h"

/////////////////////////////////////////////////////////////////////////////
// FileDlg

//////////////////////////////////////////////////////////////////////
// Construction/Destruction
//////////////////////////////////////////////////////////////////////

FileDlg::FileDlg(CWnd *parent, LPCTSTR file, LPCTSTR filter,
                 int filterIndex, LPCTSTR ext, LPCTSTR *exts, LPCTSTR initialDir,
                 LPCTSTR title, bool save, bool noReadOnly)
	: m_parent(parent), isSave(save), noReadOnly(noReadOnly), extensions(exts),
	  m_filterIndex(filterIndex)
{
	if (file)
		m_file = file;
	if (filter)
		m_filter = filter;
	if (ext)
		m_ext = ext;
	if (initialDir)
		m_initialDir = initialDir;
	if (title)
		m_title = title;
}

FileDlg::~FileDlg()
{
}

int FileDlg::DoModal()
{
	systemSoundClearBuffer();

	WinFileDialogParams params;
	params.owner        = m_parent ? m_parent->GetSafeHwnd() : NULL;
	params.save         = isSave;
	params.mustExist    = !isSave;
	params.hideReadOnly = noReadOnly;
	params.title        = m_title.IsEmpty() ? NULL : m_title.GetString();
	params.initialDir   = m_initialDir.IsEmpty() ? NULL : m_initialDir.GetString();
	params.fileName     = m_file.GetString();
	params.filter       = m_filter.IsEmpty() ? NULL : m_filter.GetString();
	params.filterIndex  = m_filterIndex;
	params.defaultExt   = m_ext.IsEmpty() ? NULL : m_ext.GetString();
	params.extensions   = extensions;

	int         index  = m_filterIndex;
	std::wstring picked;
	int         result = winFileDialog(params, picked, &index);

	if (result == IDOK)
		m_path = picked.c_str();

	m_filterIndex = index;
	return result;
}

int FileDlg::getFilterIndex()
{
	return m_filterIndex;
}

LPCTSTR FileDlg::GetPathName()
{
	return m_path;
}
