#if !defined(AFX_RECORDSETTINGS_H__9E2A4C71_5D38_4C0A_B7E4_6F1B2A9C3D50__INCLUDED_)
#define AFX_RECORDSETTINGS_H__9E2A4C71_5D38_4C0A_B7E4_6F1B2A9C3D50__INCLUDED_

#pragma once

#include "../resource.h"
#include "../VideoEncoders.h"
#include "../VideoWriter.h"

#include <vector>

// Picks the encoder pair before the save dialog asks for a file name.  The
// container and the extension are not chosen here: they follow from the pair,
// and the summary line shows what that will be.
//
// See documentation/recording-settings-design.md.
class CRecordSettings : public CDialog
{
public:
	CRecordSettings(CWnd *parent, const RecordParams &current);

	RecordParams params() const { return m_params; }

	enum { IDD = IDD_RECORD_SETTINGS };

protected:
	virtual void DoDataExchange(CDataExchange *pDX);
	virtual BOOL OnInitDialog();

	afx_msg void OnVideoChanged(NMHDR *header, LRESULT *result);
	afx_msg void OnAudioChanged();
	afx_msg void OnBitrateChanged();
	afx_msg void OnVideoPreset();
	afx_msg void OnAudioPreset();
	afx_msg void OnVideoOptionChanged();
	afx_msg void OnOk();
	afx_msg void OnCancel();
	afx_msg LRESULT OnRelayout(WPARAM, LPARAM);
	afx_msg void OnDestroy();

	DECLARE_MESSAGE_MAP()

private:
	void fillVideoList();
	void fillAudioList();
	void fillPresets();
	void fillFrameRates();
	void fillProfileBox(const GUID &video);
	void updateSummary();
	void updatePresetBoxes();
	void enableItem(int id, BOOL enable);
	void layoutColumns();
	void applyTitleFont();

	GUID currentVideo(bool *hardware) const;
	GUID currentAudio() const;
	int	 currentProfile() const;
	int	 currentFrameRate() const;
	int	 profileValueAt(int row) const;

	void selectVideo(const GUID &subtype, bool hardware);
	void selectAudio(const GUID &subtype);
	void selectProfile(int profile);

	std::vector<VideoEncoderChoice> m_video;
	std::vector<AudioEncoderChoice> m_audio;

	RecordParams m_params;

	CListCtrl		m_list;
	CComboBox		m_audioBox;
	CComboBox		m_videoPreset;
	CComboBox		m_audioPreset;
	CComboBox		m_profileBox;
	CComboBox		m_frameRateBox;

	HFONT m_titleFont;	// bold headings, created once the font hook has run
	CSpinButtonCtrl m_videoSpin;
	CSpinButtonCtrl m_audioSpin;

	// Indices into m_audio of the rows currently shown in the combo box.
	std::vector<int> m_audioRows;
};

#endif // AFX_RECORDSETTINGS_H__9E2A4C71_5D38_4C0A_B7E4_6F1B2A9C3D50__INCLUDED_
