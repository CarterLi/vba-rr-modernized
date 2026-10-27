#ifndef RAMWATCH_H
#define RAMWATCH_H

bool ResetWatches();
void OpenRWRecentFile(int memwRFileNumber);
extern bool AutoRWLoad;
extern bool RWSaveWindowPos;
#define MAX_RECENT_WATCHES 5
extern TCHAR rw_recent_files[MAX_RECENT_WATCHES][1024];
extern bool AskSave();
extern int ramw_x;
extern int ramw_y;
extern bool RWfileChanged;

//Constants
#define AUTORWLOAD _T("RamWatchAutoLoad")
#define RWSAVEPOS _T("RamWatchSaveWindowPos")
#define RAMWX _T("RamwX")
#define RAMWY _T("RamwY")

// AddressWatcher is self-contained now
struct AddressWatcher
{
	unsigned int Address; // hardware address
	char Size;
	char Type;
	char* comment; // NULL means no comment, non-NULL means allocated comment
	int WrongEndian;
	unsigned int CurValue;
};
#define MAX_WATCH_COUNT 256
extern AddressWatcher rswatches[MAX_WATCH_COUNT];
extern int WatchCount; // number of valid items in rswatches

extern char Watch_Dir[1024];

extern HWND RamWatchHWnd;
extern HACCEL RamWatchAccels;

bool InsertWatch(const AddressWatcher& Watch, const char *Comment);
// Opens the watch dialog with Address/Size/Type already filled in, so the user
// only has to type a note.  Used when the caller already knows what to watch.
bool AddWatchFromTemplate(const AddressWatcher& Watch, HWND parent=NULL);
void Update_RAM_Watch();
bool Load_Watches(bool clear, const char* filename);
void RWAddRecentFile(const char *filename);

#endif
