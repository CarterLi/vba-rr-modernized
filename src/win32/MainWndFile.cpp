#include "stdafx.h"
#include "resource.h"
#include "MainWnd.h"
#include "Reg.h"
#include "WinResUtil.h"
#include "WinMiscUtil.h"
#include "Sound.h"
#include "VBA.h"
#include "Dialogs/ExportGSASnapshot.h"
#include "Dialogs/FileDlg.h"
#include "Dialogs/GSACodeSelect.h"
#include "Dialogs/RomInfo.h"
#include "Dialogs/LuaOpenDialog.h"
#include "Dialogs/ram_search.h"
#include "Dialogs/ramwatch.h"

#include "../NLS.h"
#include "../version.h"
#include "../gba/GBA.h"
#include "../gba/GBAGlobals.h"
#include "../gba/EEprom.h"
#include "../gba/GBASound.h"
#include "../gb/GB.h"
#include "../gb/gbGlobals.h"
#include "../common/SystemGlobals.h"
#include "../common/ImageWrite.h"
#include "../common/movie.h"
#include "../common/vbalua.h"

void MainWnd::OnFileOpen()
{
	if (winFileOpenSelect(0))
	{
		winFileRun();
	}
}

void MainWnd::OnFileOpenGBx()
{
	if (winFileOpenSelect(1))
	{
		winFileRun();
	}
}

void MainWnd::OnFilePause()
{
	systemSetPause(!theApp.paused);
}

void MainWnd::OnUpdateFilePause(CCmdUI *pCmdUI)
{
	pCmdUI->SetCheck(theApp.paused);
}

void MainWnd::OnFileReset()
{
	if (emulating)
	{
		if (VBAMovieIsPlaying() && VBAMovieIsXorInput() || VBAMovieIsRecording())
		{
			VBAMovieSignalReset();
		}
		else if (VBAMovieIsPlaying())
		{
			// HACK: backward-compatibility shortcut
			VBAMovieRestart();
		}
		else
		{
			theApp.emulator.emuReset();
			systemScreenMessageRes(IDS_RESET);
		}
	}
}

void MainWnd::OnUpdateFileReset(CCmdUI *pCmdUI)
{
	pCmdUI->Enable(emulating);
}

void MainWnd::OnFileRecentFreeze()
{
	theApp.recentFreeze = !theApp.recentFreeze;
}

void MainWnd::OnUpdateFileRecentFreeze(CCmdUI *pCmdUI)
{
	pCmdUI->SetCheck(theApp.recentFreeze);
}

void MainWnd::OnFileRecentReset()
{
	systemSoundClearBuffer();
	if (MessageBox(_T("Really clear your recent ROMs list?"), //winResLoadString(IDS_REALLY_CLEAR),
	               winResLoadString(IDS_CONFIRM_ACTION),
	               MB_YESNO | MB_DEFBUTTON2) == IDNO)
		return;

	for (int i = 0; i < 10; ++i)
		theApp.recentFiles[i] = "";
}

BOOL MainWnd::OnFileRecentFile(UINT nID)
{
	if (theApp.recentFiles[(nID & 0xFFFF) - ID_FILE_MRU_FILE1].GetLength())
	{
		theApp.romFilename = theApp.recentFiles[(nID & 0xFFFF) - ID_FILE_MRU_FILE1];
		winFileRun();
	}
	return TRUE;
}

void MainWnd::OnUpdateFileRecentFile(CCmdUI *pCmdUI)
{
	int fileID = pCmdUI->m_nID - ID_FILE_MRU_FILE1;

	bool bExist = !theApp.recentFiles[fileID].IsEmpty();

	if (pCmdUI->m_pMenu != NULL)
	{
		CString p = theApp.recentFiles[fileID];

		int index = max(p.ReverseFind('/'), max(p.ReverseFind('\\'), p.ReverseFind('|')));

		if (index != -1)
		{
			p.Delete(0, index + 1);
		}

		p.Replace(_T("&"), _T("&&"));

		CString number("1&0 - ");
		if (fileID < 9)
			number.Format(_T("&%d - "), fileID + 1);

		if (p.IsEmpty())
		{
			p	   = "No Recent ROM";
			bExist = false;
		}

		pCmdUI->SetText(number + p);
		theApp.winAccelMgr.UpdateMenu(pCmdUI->m_pMenu->GetSafeHmenu());
	}

	pCmdUI->Enable(bExist);
}

void MainWnd::OnFileExit()
{
	if (AskSave())
		SendMessage(WM_CLOSE);
}

void MainWnd::OnFileClose()
{
	// save battery file before we change the filename...
	CloseRamWindows();
	winFileClose();
}

void MainWnd::OnUpdateFileClose(CCmdUI *pCmdUI)
{
	pCmdUI->Enable(emulating);
}

void MainWnd::OnFileLoad()
{

	LPCTSTR exts[] = { _T(".sgm"), NULL };
	CString filter = winResLoadFilter(IDS_FILTER_SGM);
	CString title  = winResLoadString(IDS_SELECT_SAVE_GAME_NAME);

	CString saveName = winGetDestFilename(theApp.gameFilename, IDS_SAVE_DIR, exts[0]);
	CString saveDir	 = winGetDestDir(IDS_SAVE_DIR);

	FileDlg dlg(this, saveName, filter, 0, _T("SGM"), exts, saveDir, title, false, true);

	if (dlg.DoModal() == IDOK)
	{
		CallRegisteredLuaFunctions(LUACALL_BEFORESTATELOAD);

		bool res = winReadSaveGame(dlg.GetPathName());

		// deleting rewinds because you loaded a save state is stupid
		///  theApp.rewindCount = 0;
		///  theApp.rewindCounter = 0;
		///  theApp.rewindSaveNeeded = false;

		if (res)
		{
			systemScreenMessageRes(IDS_LOADED_STATE);

			theApp.frameSearching = false;
			theApp.frameSearchSkipping = false;

			CallRegisteredLuaFunctions(LUACALL_AFTERSTATELOAD);
		}
	}
}

void MainWnd::OnUpdateFileLoad(CCmdUI *pCmdUI)
{
	pCmdUI->Enable(emulating);
}

BOOL MainWnd::OnFileLoadSlot(UINT nID)
{
	nID = nID + 1 - ID_FILE_LOADGAME_SLOT1;

	CString filename = winGetSavestateFilename(theApp.gameFilename, nID);

	CallRegisteredLuaFunctions(LUACALL_BEFORESTATELOAD);

	bool res = winReadSaveGame(filename);

	// deleting rewinds because you loaded a save state is stupid
///  theApp.rewindCount = 0;
///  theApp.rewindCounter = 0;
///  theApp.rewindSaveNeeded = false;

	if (res)
	{
		// Replaying and re-recording are reported by the movie code's own
		// strings; a plain load has one of its own.
		UINT formatId = IDS_LOADED_STATE_N;
		if (VBAMovieIsActive())
			formatId = VBAMovieIsRecording() ? IDS_RERECORDED_STATE_N
			                                 : IDS_REPLAYED_STATE_N;

		systemScreenMessageRes(formatId, (int)nID);

		int lastSlot = theApp.currentSlot;

		if (theApp.loadMakesRecent)
		{
			// to update the file's modification date
			SYSTEMTIME st;
			FILETIME   ft;
			GetSystemTime(&st);
			SystemTimeToFileTime(&st, &ft);
			HANDLE fh = CreateFile(filename, 
									GENERIC_READ | GENERIC_WRITE, FILE_SHARE_READ | FILE_SHARE_WRITE, 
									NULL, 
									OPEN_EXISTING, 
									0, 
									NULL);
			if (fh != INVALID_HANDLE_VALUE)
				SetFileTime(fh, NULL, NULL, &ft);
			CloseHandle(fh);
		}

		if (theApp.loadMakesCurrent)
			theApp.currentSlot = nID - 1;
		else
			theApp.currentSlot = lastSlot;  // restore value in case the call to OnFileSaveSlot changed it

		theApp.frameSearching	   = false;
		theApp.frameSearchSkipping = false;

		CallRegisteredLuaFunctions(LUACALL_AFTERSTATELOAD);
	}

	return res;
}

void MainWnd::OnFileSave()
{

	LPCTSTR exts[] = { _T(".sgm"), NULL };
	CString filter = winResLoadFilter(IDS_FILTER_SGM);
	CString title  = winResLoadString(IDS_SELECT_SAVE_GAME_NAME);

	CString saveName = winGetDestFilename(theApp.gameFilename, IDS_SAVE_DIR, exts[0]);
	CString saveDir	 = winGetDestDir(IDS_SAVE_DIR);

	FileDlg dlg(this, saveName, filter, 0, _T("SGM"), exts, saveDir, title, true);

	if (dlg.DoModal() == IDOK)
	{
		CallRegisteredLuaFunctions(LUACALL_BEFORESTATESAVE);

		bool res = winWriteSaveGame(dlg.GetPathName());
		if (res)
		{
			systemScreenMessageRes(IDS_WROTE_STATE);

			CallRegisteredLuaFunctions(LUACALL_AFTERSTATESAVE);
		}
	}
}

void MainWnd::OnUpdateFileSave(CCmdUI *pCmdUI)
{
	pCmdUI->Enable(emulating);
}

BOOL MainWnd::OnFileSaveSlot(UINT nID)
{
	nID = nID + 1 - ID_FILE_SAVEGAME_SLOT1;

	if (theApp.saveMakesCurrent)
		theApp.currentSlot = nID - 1;

	CString filename = winGetSavestateFilename(theApp.gameFilename, nID);

	CallRegisteredLuaFunctions(LUACALL_BEFORESTATESAVE);

	bool res = winWriteSaveGame(filename);

	if (res)
	{
		systemScreenMessageRes(IDS_WROTE_STATE_N, (int)nID);

		CallRegisteredLuaFunctions(LUACALL_AFTERSTATESAVE);
	}

	return res;
}

BOOL MainWnd::OnSelectSlot(UINT nID)
{
	nID -= ID_SELECT_SLOT1;
	theApp.currentSlot = nID;

	CString buffer;
	buffer.Format(_T("Slot %d selected"), nID + 1);
	systemScreenMessage(winAnsiFromWide(buffer), 0);

	return true;
}

void MainWnd::OnFileImportBatteryfile()
{

	LPCTSTR exts[] = { _T(".sav"), NULL };
	CString filter = winResLoadFilter(IDS_FILTER_SAV);
	CString title  = winResLoadString(IDS_SELECT_BATTERY_FILE);

	CString batteryName = winGetDestFilename(theApp.gameFilename, IDS_BATTERY_DIR, exts[0]);
	CString batteryDir	= winGetDestDir(IDS_BATTERY_DIR);

	FileDlg dlg(this, batteryName, filter, 0, _T("SAV"), exts, batteryDir, title, false, true);

	if (dlg.DoModal() == IDCANCEL)
		return;

	CString str1 = winResLoadString(IDS_SAVE_WILL_BE_LOST);
	CString str2 = winResLoadString(IDS_CONFIRM_ACTION);

	systemSoundClearBuffer();
	if (MessageBox(str1,
	               str2,
	               MB_OKCANCEL) == IDCANCEL)
		return;

	bool res = false;

	res = theApp.emulator.emuReadBattery(winAnsiFromWide(dlg.GetPathName()));

	if (!res)
	{
		systemMessage(MSG_CANNOT_OPEN_FILE, "Cannot open file %s", dlg.GetPathName());
	}
	else if (VBAMovieIsRecording())
	{
		// FIXME: we just treat this as if using a cheat code for now
		VBAMovieSignalReset();
	}
	else
	{
		theApp.emulator.emuReset();
	}
}

void MainWnd::OnUpdateFileImportBatteryfile(CCmdUI *pCmdUI)
{
	// we allow this as we allow using cheats during recording
	pCmdUI->Enable(emulating /*&& !VBAMovieIsActive()*/);
}

void MainWnd::OnFileImportGamesharkcodefile()
{

	LPCTSTR exts[] = { NULL };
	CString filter = systemCartridgeType == IMAGE_GBA ? winResLoadFilter(IDS_FILTER_SPC) : winResLoadFilter(IDS_FILTER_GCF);
	CString title  = winResLoadString(IDS_SELECT_CODE_FILE);

	FileDlg dlg(this, _T(""), filter, 0, _T(""), exts, _T(""), title, false, true);

	if (dlg.DoModal() == IDCANCEL)
		return;

	CString str1 = winResLoadString(IDS_CODES_WILL_BE_LOST);
	CString str2 = winResLoadString(IDS_CONFIRM_ACTION);

	systemSoundClearBuffer();
	if (MessageBox(str1,
	               str2,
	               MB_OKCANCEL) == IDCANCEL)
		return;

	CString file = dlg.GetPathName();
	bool	res	 = winImportGSACodeFile(file);
}

void MainWnd::OnUpdateFileImportGamesharkcodefile(CCmdUI *pCmdUI)
{
	pCmdUI->Enable(emulating /*&& !VBAMovieIsActive()*/);
}

void MainWnd::OnFileImportGamesharksnapshot()
{

	LPCTSTR exts[] = { NULL };
	CString filter = systemCartridgeType == 1 ? winResLoadFilter(IDS_FILTER_GBS) : winResLoadFilter(IDS_FILTER_SPS);
	CString title  = winResLoadString(IDS_SELECT_SNAPSHOT_FILE);

	FileDlg dlg(this, _T(""), filter, 0, _T(""), exts, _T(""), title, false, true);

	if (dlg.DoModal() == IDCANCEL)
		return;

	CString str1 = winResLoadString(IDS_SAVE_WILL_BE_LOST);
	CString str2 = winResLoadString(IDS_CONFIRM_ACTION);

	systemSoundClearBuffer();
	if (MessageBox(str1,
	               str2,
	               MB_OKCANCEL) == IDCANCEL)
		return;

	if (systemCartridgeType == 1)
		gbReadGSASnapshot(winAnsiFromWide(dlg.GetPathName()));
	else
		CPUReadGSASnapshot(winAnsiFromWide(dlg.GetPathName()));
}

void MainWnd::OnUpdateFileImportGamesharksnapshot(CCmdUI *pCmdUI)
{
	pCmdUI->Enable(emulating /*&& !VBAMovieIsActive()*/);
}

void MainWnd::OnFileExportBatteryfile()
{

	LPCTSTR exts[] = { _T(".sav"), _T(".dat"), NULL };
	CString filter = winResLoadFilter(IDS_FILTER_SAV);
	CString title  = winResLoadString(IDS_SELECT_BATTERY_FILE);

	CString batteryName = winGetDestFilename(theApp.gameFilename, IDS_BATTERY_DIR, exts[0]);
	CString batteryDir	= winGetDestDir(IDS_BATTERY_DIR);

	FileDlg dlg(this, batteryName, filter, 1, _T("SAV"), exts, batteryDir, title, true);

	if (dlg.DoModal() == IDCANCEL)
	{
		return;
	}

	bool result = false;

	if (systemCartridgeType == 1)
	{
		result = gbWriteBatteryFile(winAnsiFromWide(dlg.GetPathName()), false);
	}
	else
		result = theApp.emulator.emuWriteBattery(winAnsiFromWide(dlg.GetPathName()));

	if (!result)
		systemMessage(MSG_ERROR_CREATING_FILE, "Error creating file %s",
		              dlg.GetPathName());
}

void MainWnd::OnUpdateFileExportBatteryfile(CCmdUI *pCmdUI)
{
	pCmdUI->Enable(emulating);
}

void MainWnd::OnFileExportGamesharksnapshot()
{
	if (eepromInUse)
	{
		systemMessage(IDS_EEPROM_NOT_SUPPORTED, "EEPROM saves cannot be exported");
		return;
	}

	LPCTSTR exts[] = { _T(".sps"), NULL };

	CString filter = winResLoadFilter(IDS_FILTER_SPS);
	CString title  = winResLoadString(IDS_SELECT_SNAPSHOT_FILE);

	CString name = winGetDestFilename(theApp.gameFilename, CString(), exts[0]);

	FileDlg dlg(this, name, filter, 1, _T("SPS"), exts, _T(""), title, true);

	if (dlg.DoModal() == IDCANCEL)
		return;

	char buffer[16];
	strncpy(buffer, (const char *)&rom[0xa0], 12);
	buffer[12] = 0;

	ExportGSASnapshot dlg2(dlg.GetPathName(), buffer, this);
	dlg2.DoModal();
}

void MainWnd::OnUpdateFileExportGamesharksnapshot(CCmdUI *pCmdUI)
{
	pCmdUI->Enable(emulating && systemCartridgeType == IMAGE_GBA);
}

void MainWnd::OnFileQuickScreencapture()
{
	extern int32 captureNumber;   // GBAGlobals.cpp
	captureNumber = winScreenCapture(captureNumber);
}

void MainWnd::OnFileScreencapture()
{

	CString captureName = winGetDestFilename(theApp.gameFilename, IDS_CAPTURE_DIR, imageCaptureExtension());
	CString captureDir  = winGetDestDir(IDS_CAPTURE_DIR);

	captureName = winImageSaveDlg(this,
	                              captureName,
	                              captureDir,
	                              winResLoadString(IDS_SELECT_CAPTURE_NAME));

	if (captureName.IsEmpty())
		return;

	if (!winWriteScreenshotFile(captureName))
		return;

	systemScreenMessageRes(IDS_SCREEN_CAPTURE);
}

void MainWnd::OnUpdateFileScreencapture(CCmdUI *pCmdUI)
{
	// XP has no WIC, and there is no other writer left to fall back on.
	pCmdUI->Enable(emulating && imageWriteAvailable());
}

void MainWnd::OnFileRominformation()
{
	if (systemCartridgeType == IMAGE_GBA)
	{
		RomInfoGBA dlg(rom);
		dlg.DoModal();
	}
	else
	{
		RomInfoGB dlg(gbRom);
		dlg.DoModal();
	}
}

void MainWnd::OnUpdateFileRominformation(CCmdUI *pCmdUI)
{
	pCmdUI->Enable(emulating);
}

void MainWnd::OnFileTogglemenu()
{
	theApp.menuToggle = !theApp.menuToggle;

	if (theApp.menuToggle)
		SetMenu(&theApp.m_menu);
	else
		SetMenu(NULL);

	// The menu bar is part of the window frame, so adding or removing it
	// moves the client area and with it the area the picture is fitted into.
	// Full screen is where this matters: there the window is as large as the
	// monitor either way, and the picture has to be re-centred.
	layoutNow();
}

void MainWnd::OnUpdateFileTogglemenu(CCmdUI *pCmdUI)
{
	pCmdUI->SetCheck(theApp.menuToggle);
}

void MainWnd::OnFileSavegameOldestslot()
{
	if (!emulating)
		return;

	CFileStatus status;
	CString		str;
	time_t		time  = -1;
	int			found = -1;

	for (int i = 0; i < 10; i++)
	{
		if (CFile::GetStatus(winGetSavestateFilename(theApp.gameFilename, i + 1), status))
		{
			if (time - status.m_mtime.GetTime() > 0 || time == -1)
			{
				time  = (time_t)status.m_mtime.GetTime();
				found = i;
			}
		}
		else
		{
			found = i;
			break;
		}
	}

	OnFileSaveSlot(ID_FILE_SAVEGAME_SLOT1 + found);
}

void MainWnd::OnUpdateFileSavegameOldestslot(CCmdUI *pCmdUI)
{
	bool enabled = emulating;
	if (pCmdUI->m_pMenu != NULL)
	{
		CFileStatus status;
		time_t		time  = -1;
		int			found = -1;

		if (emulating)
		{
			for (int i = 0; i < 10; i++)
			{
				if (CFile::GetStatus(winGetSavestateFilename(theApp.gameFilename, i + 1), status))
				{
					if (time - status.m_mtime.GetTime() > 0 || time == -1)
					{
						time  = (time_t)status.m_mtime.GetTime();
						found = i;
					}
				}
				else
				{
					found = i;
					break;
				}
			}
		}

		CString str;
		enabled = (found != -1);
		if (enabled)
			str.Format(_T("&Oldest Slot (#%d)"), found + 1);
		else
			str.Format(_T("&Oldest Slot"), found + 1);

		pCmdUI->SetText(str);

		theApp.winAccelMgr.UpdateMenu(pCmdUI->m_pMenu->GetSafeHmenu());
	}

	pCmdUI->Enable(enabled);
}

void MainWnd::OnFileLoadgameMostrecent()
{
	if (!emulating)
		return;

	CFileStatus status;
	CString		str;
	time_t		time  = 0;
	int			found = -1;

	for (int i = 0; i < 10; i++)
	{
		if (CFile::GetStatus(winGetSavestateFilename(theApp.gameFilename, i + 1), status))
		{
			if (status.m_mtime.GetTime() > time)
			{
				time  = (time_t)status.m_mtime.GetTime();
				found = i;
			}
		}
	}

	if (found != -1)
	{
		OnFileLoadSlot(ID_FILE_LOADGAME_SLOT1 + found);
	}
}

void MainWnd::OnUpdateFileLoadgameMostrecent(CCmdUI *pCmdUI)
{
	bool enabled = emulating;
	if (pCmdUI->m_pMenu != NULL)
	{
		CFileStatus status;
		int			found = -1;

		time_t time = 0;
		if (emulating)
		{
			for (int i = 0; i < 10; i++)
			{
				if (CFile::GetStatus(winGetSavestateFilename(theApp.gameFilename, i + 1), status))
				{
					if (status.m_mtime.GetTime() > time)
					{
						time  = (time_t)status.m_mtime.GetTime();
						found = i;
					}
				}
			}
		}

		CString str;
		enabled = (found != -1);
		if (enabled)
			str.Format(_T("Most &Recent Slot (#%d)"), found + 1);
		else
			str.Format(_T("Most &Recent Slot"), found + 1);

		pCmdUI->SetText(str);

		theApp.winAccelMgr.UpdateMenu(pCmdUI->m_pMenu->GetSafeHmenu());
	}

	pCmdUI->Enable(enabled);
}

void MainWnd::OnUpdateFileLoadSlot(CCmdUI *pCmdUI)
{
	int slotID = pCmdUI->m_nID - ID_FILE_LOADGAME_SLOT1 + 1;

	if (pCmdUI->m_pMenu != NULL)
	{
		pCmdUI->SetText(winGetSavestateMenuString(theApp.gameFilename, slotID));

		theApp.winAccelMgr.UpdateMenu(pCmdUI->m_pMenu->GetSafeHmenu());
	}

	pCmdUI->Enable(emulating && winFileExists(winGetSavestateFilename(theApp.gameFilename, slotID)));
}

void MainWnd::OnUpdateFileSaveSlot(CCmdUI *pCmdUI)
{
	if (pCmdUI->m_pMenu != NULL)
	{
		int slotID = pCmdUI->m_nID - ID_FILE_SAVEGAME_SLOT1 + 1;

		pCmdUI->SetText(winGetSavestateMenuString(theApp.gameFilename, slotID));

		theApp.winAccelMgr.UpdateMenu(pCmdUI->m_pMenu->GetSafeHmenu());
	}

	pCmdUI->Enable(emulating);
}

void MainWnd::OnUpdateSelectSlot(CCmdUI *pCmdUI)
{
	if (pCmdUI->m_pMenu != NULL)
	{
		int slot = pCmdUI->m_nID - ID_SELECT_SLOT1;

		pCmdUI->SetText(winGetSavestateMenuString(theApp.gameFilename, slot + 1));

		theApp.winAccelMgr.UpdateMenu(pCmdUI->m_pMenu->GetSafeHmenu());

		pCmdUI->SetCheck(slot == theApp.currentSlot);
	}
}

void MainWnd::OnFileLoadgameAutoloadmostrecent()
{
	theApp.autoLoadMostRecent = !theApp.autoLoadMostRecent;
}

void MainWnd::OnUpdateFileLoadgameAutoloadmostrecent(CCmdUI *pCmdUI)
{
	pCmdUI->SetCheck(theApp.autoLoadMostRecent);
}

void MainWnd::OnFileLoadgameMakeRecent()
{
	theApp.loadMakesRecent = !theApp.loadMakesRecent;
}

void MainWnd::OnUpdateFileLoadgameMakeRecent(CCmdUI *pCmdUI)
{
	pCmdUI->SetCheck(theApp.loadMakesRecent);
}

void MainWnd::OnFileSavegameCurrent()
{
	OnFileSaveSlot(ID_FILE_SAVEGAME_SLOT1 + theApp.currentSlot);
}

void MainWnd::OnUpdateFileSavegameCurrent(CCmdUI *pCmdUI)
{
	if (pCmdUI->m_pMenu != NULL)
	{
		int slotID = theApp.currentSlot + 1;

		CString str;
		str.Format(_T("&Current Slot (#%d)"), slotID);

		pCmdUI->SetText(str);

		theApp.winAccelMgr.UpdateMenu(pCmdUI->m_pMenu->GetSafeHmenu());
	}

	pCmdUI->Enable(emulating);
}

void MainWnd::OnFileLoadgameCurrent()
{
	OnFileLoadSlot(ID_FILE_LOADGAME_SLOT1 + theApp.currentSlot);
}

void MainWnd::OnUpdateFileLoadgameCurrent(CCmdUI *pCmdUI)
{
	int slotID = theApp.currentSlot + 1;

	if (pCmdUI->m_pMenu != NULL)
	{
		CString str;
		str.Format(_T("&Current Slot (#%d)"), slotID);

		pCmdUI->SetText(str);

		theApp.winAccelMgr.UpdateMenu(pCmdUI->m_pMenu->GetSafeHmenu());
	}

	CFileStatus status;
	pCmdUI->Enable(emulating && CFile::GetStatus(winGetSavestateFilename(theApp.gameFilename, slotID), status));
}

void MainWnd::OnFileLoadgameMakeCurrent()
{
	theApp.loadMakesCurrent = !theApp.loadMakesCurrent;
}

void MainWnd::OnUpdateFileLoadgameMakeCurrent(CCmdUI *pCmdUI)
{
	pCmdUI->SetCheck(theApp.loadMakesCurrent);
}

void MainWnd::OnFileSavegameMakeCurrent()
{
	theApp.saveMakesCurrent = !theApp.saveMakesCurrent;
}

void MainWnd::OnUpdateFileSavegameMakeCurrent(CCmdUI *pCmdUI)
{
	pCmdUI->SetCheck(theApp.saveMakesCurrent);
}

void MainWnd::OnFileSavegameIncrementSlot()
{
	theApp.currentSlot = (theApp.currentSlot + 1) % 10;

	char str [32];
	sprintf(str, "Current Slot: %d", theApp.currentSlot + 1);
	systemScreenMessage(str, 0);
}

void MainWnd::OnUpdateFileSavegameIncrementSlot(CCmdUI *pCmdUI)
{
	if (pCmdUI->m_pMenu != NULL)
	{
		int slotID = theApp.currentSlot + 1;

		CString str;
		str.Format(_T("&Increase Current Slot (#%d -> #%d)"), slotID, slotID % 10 + 1);

		pCmdUI->SetText(str);

		theApp.winAccelMgr.UpdateMenu(pCmdUI->m_pMenu->GetSafeHmenu());
	}
}

void MainWnd::OnFileSavegameDecrementSlot()
{
	theApp.currentSlot = (theApp.currentSlot + 9) % 10;

	char str [32];
	sprintf(str, "Current Slot: %d", theApp.currentSlot + 1);
	systemScreenMessage(str, 0);
}

void MainWnd::OnUpdateFileSavegameDecrementSlot(CCmdUI *pCmdUI)
{
	if (pCmdUI->m_pMenu != NULL)
	{
		int slotID = theApp.currentSlot + 1;

		CString str;
		str.Format(_T("&Decrease Current Slot (#%d -> #%d)"), slotID, (slotID + 8) % 10 + 1);

		pCmdUI->SetText(str);

		theApp.winAccelMgr.UpdateMenu(pCmdUI->m_pMenu->GetSafeHmenu());
	}
}

void MainWnd::OnFileSlotDisplayModificationTime()
{
	theApp.showSlotTime = !theApp.showSlotTime;
}

void MainWnd::OnUpdateFileSlotDisplayModificationTime(CCmdUI *pCmdUI)
{
	pCmdUI->SetCheck(theApp.showSlotTime);
}

void MainWnd::OnFileLuaOpen()
{
	// Every call opens another window; each one owns its own script.
	LuaWindowCreate(this);
}

void MainWnd::OnUpdateFileLuaOpen(CCmdUI *pCmdUI)
{
	pCmdUI->SetCheck(FALSE);
	pCmdUI->Enable(true);
}

void MainWnd::OnFileLuaCloseAll()
{
	LuaWindowsCloseAll();
}

void MainWnd::OnFileLuaReload()
{
	VBAReloadLuaCode();
}

void MainWnd::OnFileLuaStop()
{
	VBALuaStop();
}

void MainWnd::OnFileRamSearch()
{

	if (!RamSearchHWnd)
	{
		reset_address_info();
		LRESULT CALLBACK RamSearchProc(HWND hDlg, UINT uMsg, WPARAM wParam, LPARAM lParam);
		// AfxGetResourceHandle and not AfxGetInstanceHandle: the template has
		// to come from the language module when one is loaded.
		::CreateDialog(AfxGetResourceHandle(), MAKEINTRESOURCE(IDD_RAMSEARCH), AfxGetMainWnd()->GetSafeHwnd(), (DLGPROC) RamSearchProc);
	}
	else
	{
		if (::IsIconic(RamSearchHWnd))
			::ShowWindow(RamSearchHWnd, SW_RESTORE);
		::SetForegroundWindow(RamSearchHWnd);
	}
}

void MainWnd::OnUpdateFileRamSearch(CCmdUI *pCmdUI)
{
	pCmdUI->Enable(TRUE);
}

void MainWnd::OnFileRamWatch()
{

	if (!RamWatchHWnd)
	{
		LRESULT CALLBACK RamWatchProc(HWND hDlg, UINT uMsg, WPARAM wParam, LPARAM lParam);
		RamWatchHWnd = ::CreateDialog(AfxGetResourceHandle(), MAKEINTRESOURCE(IDD_RAMWATCH), AfxGetMainWnd()->GetSafeHwnd(), (DLGPROC) RamWatchProc);
	}
	else
	{
		if (::IsIconic(RamWatchHWnd))
			::ShowWindow(RamWatchHWnd, SW_RESTORE);
		::SetForegroundWindow(RamWatchHWnd);
	}
}

void MainWnd::OnUpdateFileRamWatch(CCmdUI *pCmdUI)
{
	pCmdUI->Enable(TRUE);
}

void MainWnd::OnUpdateFileLuaCloseAll(CCmdUI *pCmdUI)
{
	pCmdUI->Enable(LuaWindowsAny());
}

