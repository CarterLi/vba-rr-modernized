// RecordSettings.cpp : the encoder picker shown before the save dialog.
//
// The list is built from what this machine actually has installed, so a
// system with no hardware encoder simply shows the software row, and an
// audio codec whose encoder is not installed never appears at all.
//
// The container and the extension follow from the chosen pair; they are shown
// in the summary line and never edited here.

#include "../stdafx.h"

#include "RecordSettings.h"

#include "../resource.h"
#include "../UiFont.h"
#include "../VBA.h"
#include "../WinResUtil.h"
#include "../VideoEncoders.h"

#include "../../common/System.h"

// Sound quality divisor, 1 = 44100 Hz.  Same variable the sound backend and
// recordingSoundFormat() in MainWndTools.cpp read.
extern int32 soundQuality;

namespace
{
	const UINT kRelayout = WM_APP + 1;

	// Rows of the preset boxes, in the order fillPresets() adds them.
	enum { PRESET_LOW, PRESET_MEDIUM, PRESET_HIGH, PRESET_CUSTOM };

	const int kVideoPresets[] = { VIDEO_BITRATE_LOW, VIDEO_BITRATE_MEDIUM, VIDEO_BITRATE_HIGH };
	const int kAudioPresets[] = { AUDIO_BITRATE_LOW, AUDIO_BITRATE_MEDIUM, AUDIO_BITRATE_HIGH };

	const int kFrameRates[] = { FRAMERATE_NATIVE, 60, 30, 20, 15 };

	// One profile list per codec: the numbers are that codec's own profile
	// numbers, so 77 means Main in H.264 and nothing in particular in the
	// other two.  Only the 8-bit 4:2:0 profiles are offered, because that is
	// what the frame is before the encoder sees it.
	struct ProfileEntry
	{
		int			  value;
		const wchar_t *name;
	};

	const ProfileEntry kH264Profiles[] = {
		{ VIDEO_PROFILE_AUTO, L"" },	// the empty name means "Auto", from the resources
		{ H264_PROFILE_BASELINE, L"Baseline" },
		{ H264_PROFILE_MAIN, L"Main" },
		{ H264_PROFILE_HIGH, L"High" },
	};

	// HEVC and AV1 have one 8-bit 4:2:0 profile each, so there is nothing to
	// choose; the box stays on Auto and is disabled.  See VideoWriter.h.
	const ProfileEntry kHevcProfiles[] = { { VIDEO_PROFILE_AUTO, L"" } };
	const ProfileEntry kAv1Profiles[]  = { { VIDEO_PROFILE_AUTO, L"" } };

	// The rate the sound backend runs at, the same 44100 / soundQuality the
	// recording format uses.  It is shown, never edited: the emulator decides
	// it from Options -> Sound -> Sound quality.
	int emulatorSampleRate()
	{
		const int divisor = soundQuality > 0 ? soundQuality : 1;
		return 44100 / divisor;
	}

	int clampInt(int value, int low, int high)
	{
		if (value < low)
			return low;
		if (value > high)
			return high;
		return value;
	}
}

CRecordSettings::CRecordSettings(CWnd *parent, const RecordParams &current)
	: CDialog(CRecordSettings::IDD, parent)
{
	m_titleFont			  = NULL;
	m_params			  = current;
	m_params.videoBitrate = clampInt(m_params.videoBitrate, VIDEO_BITRATE_MIN, VIDEO_BITRATE_MAX);
	m_params.audioBitrate = clampInt(m_params.audioBitrate, AUDIO_BITRATE_MIN, AUDIO_BITRATE_MAX);
}

void CRecordSettings::DoDataExchange(CDataExchange *pDX)
{
	CDialog::DoDataExchange(pDX);

	DDX_Control(pDX, IDC_ENC_VIDEO, m_list);
	DDX_Control(pDX, IDC_ENC_AUDIO, m_audioBox);
	DDX_Control(pDX, IDC_ENC_VIDEO_PRESET, m_videoPreset);
	DDX_Control(pDX, IDC_ENC_AUDIO_PRESET, m_audioPreset);
	DDX_Control(pDX, IDC_ENC_PROFILE, m_profileBox);
	DDX_Control(pDX, IDC_ENC_FRAMERATE, m_frameRateBox);
	DDX_Control(pDX, IDC_ENC_VIDEO_SPIN, m_videoSpin);
	DDX_Control(pDX, IDC_ENC_AUDIO_SPIN, m_audioSpin);
}

BEGIN_MESSAGE_MAP(CRecordSettings, CDialog)
	ON_NOTIFY(LVN_ITEMCHANGED, IDC_ENC_VIDEO, OnVideoChanged)
	ON_CBN_SELCHANGE(IDC_ENC_AUDIO, OnAudioChanged)
	ON_CBN_SELCHANGE(IDC_ENC_VIDEO_PRESET, OnVideoPreset)
	ON_CBN_SELCHANGE(IDC_ENC_AUDIO_PRESET, OnAudioPreset)
	ON_CBN_SELCHANGE(IDC_ENC_PROFILE, OnVideoOptionChanged)
	ON_CBN_SELCHANGE(IDC_ENC_FRAMERATE, OnVideoOptionChanged)
	ON_EN_CHANGE(IDC_ENC_VIDEO_BITRATE, OnBitrateChanged)
	ON_EN_CHANGE(IDC_ENC_AUDIO_BITRATE, OnBitrateChanged)
	ON_BN_CLICKED(ID_OK, OnOk)
	ON_BN_CLICKED(ID_CANCEL, OnCancel)
	ON_MESSAGE(kRelayout, OnRelayout)
	ON_WM_DESTROY()
END_MESSAGE_MAP()

BOOL CRecordSettings::OnInitDialog()
{
	CDialog::OnInitDialog();

	// The whole row selects, not just the first column, and the selection
	// stays visible when the list loses focus, so the list reads as a choice
	// rather than as a table.
	m_list.SetExtendedStyle(LVS_EX_FULLROWSELECT | LVS_EX_DOUBLEBUFFER);

	videoEncoders::enumerate(m_video, m_audio);

	fillVideoList();
	fillAudioList();
	fillPresets();
	fillFrameRates();

	m_videoSpin.SetRange32(VIDEO_BITRATE_MIN, VIDEO_BITRATE_MAX);
	m_videoSpin.SetPos(m_params.videoBitrate);

	m_audioSpin.SetRange32(AUDIO_BITRATE_MIN, AUDIO_BITRATE_MAX);
	m_audioSpin.SetPos(m_params.audioBitrate);

	for (int i = 0; i < 5; ++i)
		if (kFrameRates[i] == m_params.frameRate)
			m_frameRateBox.SetCurSel(i);

	CheckDlgButton(IDC_ENC_ASYNC, m_params.async ? BST_CHECKED : BST_UNCHECKED);

	// Selecting the row runs OnVideoChanged, which is what fills the profile
	// box with the list belonging to that codec; the saved profile can only
	// be applied after that.
	selectVideo(m_params.videoSubtype, m_params.videoHardware);
	selectAudio(m_params.audioSubtype);
	selectProfile(m_params.profile);

	{
		CString text;
		text.Format(_T("%d Hz"), emulatorSampleRate());
		SetDlgItemText(IDC_ENC_SAMPLE_RATE, text);
	}

	updateSummary();
	CenterWindow();

	// The column widths and the heading font need the final control size,
	// which the font hook only settles after this handler returns.
	PostMessage(kRelayout);

	return TRUE;
}

void CRecordSettings::OnDestroy()
{
	if (m_titleFont != NULL)
	{
		DeleteObject(m_titleFont);
		m_titleFont = NULL;
	}

	CDialog::OnDestroy();
}

LRESULT CRecordSettings::OnRelayout(WPARAM, LPARAM)
{
	layoutColumns();
	applyTitleFont();
	return 0;
}

void CRecordSettings::layoutColumns()
{
	if (m_list.GetSafeHwnd() == NULL)
		return;

	CRect client;
	m_list.GetClientRect(&client);

	const UINT dpi	 = uiFont::layoutDpi(m_hWnd);
	const int  codec = MulDiv(46, dpi, 96);
	const int  type	 = MulDiv(88, dpi, 96);

	// The codec name is at most "HEVC"; the type column has to hold
	// "Hardware (NVIDIA)"; the encoder name takes whatever is left.
	int encoder = client.Width() - codec - type;
	if (encoder < MulDiv(90, dpi, 96))
		encoder = MulDiv(90, dpi, 96);

	m_list.SetColumnWidth(0, codec);
	m_list.SetColumnWidth(1, encoder);
	m_list.SetColumnWidth(2, type);
}

// Derive the headings from the font the dialog hook fitted to the template
// line height.  Using the raw system message font here bypasses that fit.
void CRecordSettings::applyTitleFont()
{
	LOGFONT logFont;
	HFONT titleFont = (HFONT)SendDlgItemMessage(IDC_ENC_VIDEO_TITLE, WM_GETFONT);
	if (titleFont == NULL || GetObject(titleFont, sizeof(logFont), &logFont) == 0)
		return;

	logFont.lfWeight = FW_BOLD;

	if (m_titleFont != NULL)
		DeleteObject(m_titleFont);
	m_titleFont = CreateFontIndirect(&logFont);
	if (m_titleFont == NULL)
		return;

	SendDlgItemMessage(IDC_ENC_VIDEO_TITLE, WM_SETFONT, (WPARAM)m_titleFont, TRUE);
	SendDlgItemMessage(IDC_ENC_AUDIO_TITLE, WM_SETFONT, (WPARAM)m_titleFont, TRUE);
}

void CRecordSettings::fillVideoList()
{
	m_list.DeleteAllItems();

	m_list.InsertColumn(0, winResLoadString(IDS_RECORD_CODEC), LVCFMT_LEFT, 46);
	m_list.InsertColumn(1, winResLoadString(IDS_RECORD_ENCODER), LVCFMT_LEFT, 150);
	m_list.InsertColumn(2, winResLoadString(IDS_RECORD_TYPE), LVCFMT_LEFT, 88);

	const CString hardware = winResLoadString(IDS_RECORD_HARDWARE);
	const CString software = winResLoadString(IDS_RECORD_SOFTWARE);
	const CString none	   = winResLoadString(IDS_RECORD_NONE_VIDEO);

	for (size_t i = 0; i < m_video.size(); ++i)
	{
		const VideoEncoderChoice &entry = m_video[i];

		m_list.InsertItem((int)i, entry.available ? videoEncoders::codecName(entry.subtype) : _T("-"));

		if (!entry.available)
		{
			m_list.SetItemText((int)i, 1, none);
			continue;
		}

		m_list.SetItemText((int)i, 1, CString(entry.name.c_str()));

		CString type = entry.hardware ? hardware : software;
		if (entry.hardware && !entry.vendor.empty())
			type += CString(_T(" (")) + CString(entry.vendor.c_str()) + CString(_T(")"));

		m_list.SetItemText((int)i, 2, type);
	}
}

void CRecordSettings::fillAudioList()
{
	const GUID video = currentVideo(NULL);

	m_audioBox.ResetContent();
	m_audioRows.clear();

	const CString none = winResLoadString(IDS_RECORD_NONE);

	for (size_t i = 0; i < m_audio.size(); ++i)
	{
		const AudioEncoderChoice &entry = m_audio[i];

		// Only offer what the container of this pair can hold.
		if (videoEncoders::containerFor(video, entry.subtype) == NULL)
			continue;

		const CString text = entry.available ? CString(entry.name.c_str()) : none;
		const int	  row  = m_audioBox.AddString(text);
		if (row < 0)
			continue;

		m_audioBox.SetItemData((UINT)row, (DWORD_PTR)i);
		m_audioRows.push_back((int)i);
	}
}

void CRecordSettings::fillPresets()
{
	m_videoPreset.AddString(winResLoadString(IDS_RECORD_PRESET_LOW));
	m_videoPreset.AddString(winResLoadString(IDS_RECORD_PRESET_MEDIUM));
	m_videoPreset.AddString(winResLoadString(IDS_RECORD_PRESET_HIGH));
	m_videoPreset.AddString(winResLoadString(IDS_RECORD_PRESET_CUSTOM));

	m_audioPreset.AddString(winResLoadString(IDS_RECORD_PRESET_LOW));
	m_audioPreset.AddString(winResLoadString(IDS_RECORD_PRESET_MEDIUM));
	m_audioPreset.AddString(winResLoadString(IDS_RECORD_PRESET_HIGH));
	m_audioPreset.AddString(winResLoadString(IDS_RECORD_PRESET_CUSTOM));
}

void CRecordSettings::fillFrameRates()
{
	const CString autoText = winResLoadString(IDS_RECORD_AUTO);

	m_frameRateBox.AddString(autoText);
	m_frameRateBox.AddString(_T("60"));
	m_frameRateBox.AddString(_T("30"));
	m_frameRateBox.AddString(_T("20"));
	m_frameRateBox.AddString(_T("15"));
}

// The profile list belongs to the codec, so it is rebuilt whenever the
// selected row changes.  Rebuilding also puts the choice back on Auto,
// because a profile number means something different in each codec.
void CRecordSettings::fillProfileBox(const GUID &video)
{
	const ProfileEntry *entries = kH264Profiles;
	int					count	= _countof(kH264Profiles);

	if (videoEncoders::isHevc(video))
	{
		entries = kHevcProfiles;
		count	= _countof(kHevcProfiles);
	}
	else if (videoEncoders::isAv1(video))
	{
		entries = kAv1Profiles;
		count	= _countof(kAv1Profiles);
	}

	const CString autoText = winResLoadString(IDS_RECORD_AUTO);

	m_profileBox.ResetContent();
	for (int i = 0; i < count; ++i)
		m_profileBox.AddString(entries[i].value == VIDEO_PROFILE_AUTO ? autoText
		                                                             : entries[i].name);

	m_profileBox.SetCurSel(0);
}

int CRecordSettings::profileValueAt(int row) const
{
	const GUID video = currentVideo(NULL);

	const ProfileEntry *entries = kH264Profiles;
	if (videoEncoders::isHevc(video))
		entries = kHevcProfiles;
	else if (videoEncoders::isAv1(video))
		entries = kAv1Profiles;

	return entries[row].value;
}

void CRecordSettings::selectProfile(int profile)
{
	const int count = m_profileBox.GetCount();
	for (int i = 0; i < count; ++i)
	{
		if (profileValueAt(i) == profile)
		{
			m_profileBox.SetCurSel(i);
			return;
		}
	}

	m_profileBox.SetCurSel(0);
}

int CRecordSettings::currentProfile() const
{
	const int row = m_profileBox.GetCurSel();
	if (row < 0 || row >= m_profileBox.GetCount())
		return VIDEO_PROFILE_AUTO;

	return profileValueAt(row);
}

int CRecordSettings::currentFrameRate() const
{
	const int row = m_frameRateBox.GetCurSel();
	if (row < 0 || row >= 5)
		return FRAMERATE_NATIVE;

	return kFrameRates[row];
}

GUID CRecordSettings::currentVideo(bool *hardware) const
{
	const int row = m_list.GetNextItem(-1, LVNI_SELECTED);
	if (row < 0 || row >= (int)m_video.size())
	{
		if (hardware)
			*hardware = false;
		return GUID();
	}

	const VideoEncoderChoice &entry = m_video[row];
	if (hardware)
		*hardware = entry.hardware;

	if (!entry.available)
		return GUID();

	return entry.subtype;
}

GUID CRecordSettings::currentAudio() const
{
	const int row = m_audioBox.GetCurSel();
	if (row < 0)
		return GUID();

	const int index = (int)m_audioBox.GetItemData((UINT)row);
	if (index < 0 || index >= (int)m_audio.size())
		return GUID();

	const AudioEncoderChoice &entry = m_audio[index];
	return entry.available ? entry.subtype : GUID();
}

void CRecordSettings::selectVideo(const GUID &subtype, bool hardware)
{
	int fallback = -1;

	for (size_t i = 0; i < m_video.size(); ++i)
	{
		const VideoEncoderChoice &entry = m_video[i];

		if (entry.available && fallback < 0)
			fallback = (int)i;

		if (entry.available && entry.hardware == hardware && IsEqualGUID(entry.subtype, subtype))
		{
			m_list.SetItemState((int)i, LVIS_SELECTED | LVIS_FOCUSED, LVIS_SELECTED | LVIS_FOCUSED);
			m_list.EnsureVisible((int)i, FALSE);
			return;
		}
	}

	// The saved encoder is gone (driver update, another machine): fall back to
	// the first row, which is a hardware encoder when there is one.
	if (fallback < 0 && !m_video.empty())
		fallback = 0;

	if (fallback >= 0)
		m_list.SetItemState(fallback, LVIS_SELECTED | LVIS_FOCUSED, LVIS_SELECTED | LVIS_FOCUSED);
}

void CRecordSettings::selectAudio(const GUID &subtype)
{
	for (size_t row = 0; row < m_audioRows.size(); ++row)
	{
		const AudioEncoderChoice &entry = m_audio[m_audioRows[row]];
		if (IsEqualGUID(entry.subtype, subtype))
		{
			m_audioBox.SetCurSel((int)row);
			return;
		}
	}

	// Default to the last row, which is "None", only when the saved codec is
	// not there at all.
	if (!m_audioRows.empty())
		m_audioBox.SetCurSel((int)m_audioRows.size() - 1);
}

// GetDlgItem returns NULL for a control the template does not have, and that
// happens for real: a language module built from an older resource script is
// missing the newer controls while the code that drives them is current.
// Everything that enables a control goes through here.
void CRecordSettings::enableItem(int id, BOOL enable)
{
	CWnd *control = GetDlgItem(id);
	if (control != NULL)
		control->EnableWindow(enable);
}

void CRecordSettings::updatePresetBoxes()
{
	// The box mirrors the number: typing 4000 by hand lands on "Medium", and
	// any other number lands on "Custom".
	const int video = (int)GetDlgItemInt(IDC_ENC_VIDEO_BITRATE, NULL, FALSE);
	int		  row	= PRESET_CUSTOM;
	for (int i = 0; i < 3; ++i)
		if (kVideoPresets[i] == video)
			row = i;
	m_videoPreset.SetCurSel(row);

	const int audio = (int)GetDlgItemInt(IDC_ENC_AUDIO_BITRATE, NULL, FALSE);
	row				= PRESET_CUSTOM;
	for (int i = 0; i < 3; ++i)
		if (kAudioPresets[i] == audio)
			row = i;
	m_audioPreset.SetCurSel(row);
}

void CRecordSettings::updateSummary()
{
	// The spin controls write their buddy's text while the template is still
	// being created (UDS_AUTOBUDDY | UDS_SETBUDDYINT), which sends EN_CHANGE
	// long before OnInitDialog() has attached the controls and enumerated the
	// encoders.  There is no selection to summarize at that point, and asking
	// the list would assert on its null handle.
	if (m_list.GetSafeHwnd() == NULL)
		return;

	bool	   hardware = false;
	const GUID video	= currentVideo(&hardware);
	const GUID audio	= currentAudio();

	const RecordContainer *container = videoEncoders::containerFor(video, audio);

	const BOOL hasVideo = !videoEncoders::isNullGuid(video);
	const BOOL hasAudio = !videoEncoders::isNullGuid(audio);

	// A bit rate target means nothing to a lossless encoder, so the box goes
	// away rather than sitting there doing nothing.
	const BOOL lossless = hasAudio && videoEncoders::isLosslessAudio(audio);
	enableItem(IDC_ENC_AUDIO_BITRATE, hasAudio && !lossless);
	enableItem(IDC_ENC_AUDIO_SPIN, hasAudio && !lossless);
	enableItem(IDC_ENC_AUDIO_PRESET, hasAudio && !lossless);
	enableItem(IDC_ENC_SAMPLE_RATE, hasAudio);
	enableItem(IDC_ENC_VIDEO_BITRATE, hasVideo);
	enableItem(IDC_ENC_VIDEO_SPIN, hasVideo);
	enableItem(IDC_ENC_VIDEO_PRESET, hasVideo);
	// Only H.264 has a profile worth choosing; see VideoWriter.h.
	enableItem(IDC_ENC_PROFILE, hasVideo && videoEncoders::isH264(video));
	enableItem(IDC_ENC_FRAMERATE, hasVideo);

	updatePresetBoxes();

	CString summary;
	CString hint;

	if (container == NULL)
	{
		summary = winResLoadString(IDS_RECORD_NOTHING_SELECTED);
		hint.Empty();
	}
	else
	{
		CString name = container->extension + 1;
		name.MakeUpper();

		if (hasVideo)
		{
			int32 width	 = 0;
			int32 height = 0;
			systemGetLCDResolution(width, height);

			if (theApp.captureScaled && theApp.surfaceSizeX > 0 && theApp.surfaceSizeY > 0)
			{
				width  = theApp.surfaceSizeX;
				height = theApp.surfaceSizeY;
			}

			const int videoBitrate = (int)GetDlgItemInt(IDC_ENC_VIDEO_BITRATE, NULL, FALSE);

			// The rate actually written: a target below the emulator's own
			// rate drops frames, so the summary has to say so.
			double	  fps  = systemGetFrameRate();
			const int rate = currentFrameRate();
			const int step = videoEncoders::videoFrameStep(fps, rate);
			if (step > 1)
				fps /= step;

			summary.Format(_T("%s (%s) - %dx%d - %.2f fps - %d kbps"), (LPCTSTR)name,
			               container->extension, (int)width, (int)height, fps, videoBitrate);

			hint = winResLoadString(hardware ? IDS_RECORD_HINT_HARDWARE : IDS_RECORD_HINT_SOFTWARE);
		}
		else
		{
			const int audioBitrate = (int)GetDlgItemInt(IDC_ENC_AUDIO_BITRATE, NULL, FALSE);

			if (lossless)
				summary.Format(_T("%s (%s) - %s - 44.1 kHz"), (LPCTSTR)name, container->extension,
				               videoEncoders::codecName(audio));
			else
				summary.Format(_T("%s (%s) - %s - 44.1 kHz - %d kbps"), (LPCTSTR)name,
				               container->extension, videoEncoders::codecName(audio), audioBitrate);

			hint = winResLoadString(IDS_RECORD_HINT_AUDIO_ONLY);
		}
	}

	SetDlgItemText(IDC_ENC_SUMMARY, summary);
	SetDlgItemText(IDC_ENC_HINT, hint);

	// A pair of "None"s records nothing.
	enableItem(ID_OK, container != NULL);
}

void CRecordSettings::OnVideoChanged(NMHDR *header, LRESULT *result)
{
	NM_LISTVIEW *view = (NM_LISTVIEW *)header;
	*result			  = 0;

	if ((view->uChanged & LVIF_STATE) == 0)
		return;

	// The notification arrives twice per change (old state, new state); only
	// the one that leaves the row selected is interesting.
	if ((view->uNewState & LVIS_SELECTED) == 0)
		return;

	// Keep whatever audio codec was already picked when it is still legal
	// for the new container.
	const GUID audio = currentAudio();
	fillAudioList();
	selectAudio(audio);

	// A different codec means a different set of profiles.
	fillProfileBox(currentVideo(NULL));

	updateSummary();
}

void CRecordSettings::OnAudioChanged()
{
	updateSummary();
}

void CRecordSettings::OnVideoPreset()
{
	const int row = m_videoPreset.GetCurSel();
	if (row < 0 || row >= 3)
		return;

	// Writing the box fires EN_CHANGE, which brings the summary and the
	// preset row itself back in sync.
	SetDlgItemInt(IDC_ENC_VIDEO_BITRATE, (UINT)kVideoPresets[row], FALSE);
}

void CRecordSettings::OnAudioPreset()
{
	const int row = m_audioPreset.GetCurSel();
	if (row < 0 || row >= 3)
		return;

	SetDlgItemInt(IDC_ENC_AUDIO_BITRATE, (UINT)kAudioPresets[row], FALSE);
}

void CRecordSettings::OnVideoOptionChanged()
{
	updateSummary();
}

void CRecordSettings::OnBitrateChanged()
{
	updateSummary();
}

void CRecordSettings::OnOk()
{
	bool	   hardware = false;
	const GUID video	= currentVideo(&hardware);
	const GUID audio	= currentAudio();

	if (videoEncoders::containerFor(video, audio) == NULL)
		return;

	m_params.videoSubtype  = video;
	m_params.videoHardware = hardware;
	m_params.audioSubtype  = audio;
	m_params.videoBitrate  = clampInt((int)GetDlgItemInt(IDC_ENC_VIDEO_BITRATE, NULL, FALSE),
	                                 VIDEO_BITRATE_MIN, VIDEO_BITRATE_MAX);
	m_params.audioBitrate  = clampInt((int)GetDlgItemInt(IDC_ENC_AUDIO_BITRATE, NULL, FALSE),
	                                 AUDIO_BITRATE_MIN, AUDIO_BITRATE_MAX);
	m_params.frameRate	   = currentFrameRate();
	m_params.async		   = IsDlgButtonChecked(IDC_ENC_ASYNC) == BST_CHECKED;
	m_params.profile	   = videoEncoders::isNullGuid(video) ? VIDEO_PROFILE_AUTO : currentProfile();

	EndDialog(IDOK);
}

void CRecordSettings::OnCancel()
{
	EndDialog(IDCANCEL);
}
