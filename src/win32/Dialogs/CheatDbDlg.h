#ifndef VBA_WIN32_CHEAT_DB_DLG_H
#define VBA_WIN32_CHEAT_DB_DLG_H

#pragma once

class CWnd;

// Imports cheats from a database and lets the user tick the ones to copy into the
// cheat list.  The source can be an EZ-Flash Omega package (.zip, or the extracted
// directory), a single package .cht, or a libretro-database .cht; the extension
// does not tell the two text formats apart, so a single file is identified by its
// contents.  `askForFile` forces the file chooser even when a source was used
// before.  Returns the number of cheats that were added.
extern int winLoadCheatsFromDatabase(CWnd *parent, bool askForFile);

#endif // VBA_WIN32_CHEAT_DB_DLG_H
