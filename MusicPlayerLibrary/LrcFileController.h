#pragma once
#include "pch.h"

namespace MusicPlayerLibrary
{
    
enum class LrcMetadataType
{
	Artist, Album, Author, By, Offset, Title, Ignored, Error
};

enum class ThreeWayCompareResult
{
	Less = -1, Equal = 0, Greater = 1
};

enum class LrcAuxiliaryInfoNative
{
	Lyric,
	Translation,
	Romanization,
	Ignored
};

class LrcLanguageHelper
{
public:
	enum class LanguageType
	{
		zh, en, jp, kr, others
	};
	static void detect_eng_vs_jpn_romaji_prob(const CString& input, float* eng_prob, float* jpn_romaji_prob);
	static LanguageType detect_language_type(const CString& input_trimmed, float* probability = nullptr);
};

class LrcAbstractNode {
protected:
	int time_ms;            // time in milliseconds
public:
	explicit LrcAbstractNode(int time) : time_ms(time) {}
	virtual ~LrcAbstractNode() = default;
	[[nodiscard]] int get_time_ms() const { return time_ms; }
	[[nodiscard]] virtual int get_lrc_str_count() const = 0;
	virtual int get_lrc_str_at(int index, CString& out_str) const = 0;
	[[nodiscard]] virtual LrcAuxiliaryInfoNative get_auxiliary_info(int index) const = 0;
	[[nodiscard]] virtual int get_auxiliary_info_at(LrcAuxiliaryInfoNative info) const = 0;
	[[nodiscard]] virtual bool is_translation_enabled() const { return false; }
	[[nodiscard]] virtual bool is_romanization_enabled() const { return false; }
	[[nodiscard]] virtual float get_lrc_percentage(float current_timestamp) const = 0;
	[[nodiscard]] virtual bool is_lrc_percentage_enabled() const { return false; }
	virtual void set_lrc_end_timestamp(int end_time_ms) { }

	bool operator<(const LrcAbstractNode& other) const {
		return time_ms < other.time_ms;
	}
};

class LrcNode final: public LrcAbstractNode {
	CString lrc_text;       // lyric text
public:
	LrcNode(int t, const CString& text)
		: LrcAbstractNode(t), lrc_text(text) {
	}

	[[nodiscard]] int get_lrc_str_count() const override {
		return 1;
	}

	int get_lrc_str_at(int index, CString& out_str) const override {
		if (index != 0) return -1;
		return out_str = lrc_text, 0;
	}

	[[nodiscard]] LrcAuxiliaryInfoNative get_auxiliary_info(int index) const override {
		if (index == 0)
			return LrcAuxiliaryInfoNative::Lyric;
		return LrcAuxiliaryInfoNative::Ignored;
	}

	[[nodiscard]] int get_auxiliary_info_at(LrcAuxiliaryInfoNative info) const override {
		if (info == LrcAuxiliaryInfoNative::Lyric) return 0;
		return -1;
	}
	[[nodiscard]] float get_lrc_percentage(float current_timestamp) const override
	{
		return 1.0f;
	}
};

/*
* for display lrc with translation or romanization
*/
class LrcMultiNode : virtual public LrcAbstractNode {
	int str_count;
	CSimpleArray<CString> lrc_texts;
	CSimpleArray<LrcAuxiliaryInfoNative> aux_infos;
	CSimpleArray<LrcLanguageHelper::LanguageType> lang_types;

public:

	LrcMultiNode(int t, const CSimpleArray<CString>& texts);

	[[nodiscard]] int get_lrc_str_count() const override {
		return str_count;
	}

	int get_lrc_str_at(int index, CString& out_str) const override {
		if (index < 0 || index >= str_count) return -1;
		out_str = lrc_texts[index];
		return 0;
	}

	[[nodiscard]] LrcAuxiliaryInfoNative get_auxiliary_info(int index) const override
	{
		return aux_infos[index];
	}

	[[nodiscard]] int get_auxiliary_info_at(LrcAuxiliaryInfoNative info) const override {
		return aux_infos.Find(info);
	}

	[[nodiscard]] bool is_translation_enabled() const override {
		return aux_infos.Find(LrcAuxiliaryInfoNative::Translation) != -1;
	}

	[[nodiscard]] bool is_romanization_enabled() const override {
		return aux_infos.Find(LrcAuxiliaryInfoNative::Romanization) != -1;
	}

	[[nodiscard]] float get_lrc_percentage(float current_timestamp) const override
	{
		return 1.0f;
	}
};

class LrcProgressNode: virtual public LrcAbstractNode
{
protected:
	int node_count;
	struct node_info
	{
		int time_ms;
		CString node_text;
	};
	int end_time_ms;
	CSimpleArray<node_info> nodes;
public:
	LrcProgressNode(int t, const CString& text_with_node);
	[[nodiscard]] int get_lrc_str_count() const override { return 1; }
	int get_lrc_str_at(int index, CString& out_str) const override
	{
		if (index != 0) return -1;
		CString text;
		for (int i = 0; i < node_count; ++i)
		{
			text.Append(nodes[i].node_text);
		}
		return out_str = text, 0;
	}
	[[nodiscard]] LrcAuxiliaryInfoNative get_auxiliary_info(int index) const override
	{
		if (index == 0)
			return LrcAuxiliaryInfoNative::Lyric;
		return LrcAuxiliaryInfoNative::Ignored;
	}
	[[nodiscard]] int get_auxiliary_info_at(LrcAuxiliaryInfoNative info) const override
	{
		if (info == LrcAuxiliaryInfoNative::Lyric) return 0;
		return -1;
	}
	[[nodiscard]] float get_lrc_percentage(float current_timestamp) const override;
	[[nodiscard]] bool is_lrc_percentage_enabled() const override { return true; }
	void set_lrc_end_timestamp(int time_ms) override { this->end_time_ms = time_ms; }
};

class LrcProgressMultiNode final:
	public LrcProgressNode, public LrcMultiNode
{
public:
	LrcProgressMultiNode(int t, const CString& str_1, const CSimpleArray<CString>& str_arr_2);
	[[nodiscard]] int get_lrc_str_count() const override
	{
		return LrcMultiNode::get_lrc_str_count();
	}
	int get_lrc_str_at(int index, CString& out_str) const override
	{
		return LrcMultiNode::get_lrc_str_at(index, out_str);
	}
	[[nodiscard]] LrcAuxiliaryInfoNative get_auxiliary_info(int index) const override
	{
		return LrcMultiNode::get_auxiliary_info(index);
	}
	[[nodiscard]] int get_auxiliary_info_at(LrcAuxiliaryInfoNative info) const override
	{
		return LrcMultiNode::get_auxiliary_info_at(info);
	}
	[[nodiscard]] float get_lrc_percentage(float current_timestamp) const override
	{
		return LrcProgressNode::get_lrc_percentage(current_timestamp);
	}
	[[nodiscard]] bool is_translation_enabled() const override { return LrcMultiNode::is_translation_enabled(); }
	[[nodiscard]] bool is_romanization_enabled() const override { return LrcMultiNode::is_romanization_enabled(); }
	[[nodiscard]] bool is_lrc_percentage_enabled() const override { return true; }
	void set_lrc_end_timestamp(int time_ms) override { LrcProgressNode::set_lrc_end_timestamp(time_ms); }
};

class LrcNodeFactory {
public:
	static LrcAbstractNode* CreateLrcNode(int time_ms, const CSimpleArray<CString>& lrc_texts) {
		auto ifLrcContainsControllerNode = [](const CString& lrc_text)
		{
			const auto last_index = lrc_text.GetLength() - 1;
			return lrc_text.GetLength() > 0 &&
				(lrc_text[last_index] == ']' || lrc_text[last_index] == '>');
		};
		if (lrc_texts.GetSize() == 1) {
			if (ifLrcContainsControllerNode(lrc_texts[0]))
				return new LrcProgressNode(time_ms, lrc_texts[0]);
			return new LrcNode(time_ms, lrc_texts[0]);
		}
		if (lrc_texts.GetSize() > 1) {
			if (ifLrcContainsControllerNode(lrc_texts[0]))
				return new LrcProgressMultiNode(time_ms, lrc_texts[0], lrc_texts);
			return new LrcMultiNode(time_ms, lrc_texts);
		}
		return nullptr;
	}
};

/*
* internal helper of CLrcManagerWnd, perform lyric management
*/
class LrcFileControllerNative {
	friend class CLrcManagerWnd;

	CAtlArray<LrcAbstractNode*> lrc_nodes;
	int time_stamp_ms = 0, lrc_offset_ms = 0;
	size_t cur_lrc_node_index = 0;
	struct
	{
		CString artist, album, author, by, title;
	} metadata;
	int aux_enable_info = 0;
	float song_duration_sec = 0;
public:
	~LrcFileControllerNative();
	void parse_lrc_file(const CString& file_path);
	void parse_lrc_file_stream(CFile* file_stream);
	void clear_lrc_nodes();
	void set_time_stamp(int time_stamp_ms_in);
	void time_stamp_increase(int ms);
	void set_song_duration(float duration_sec) { song_duration_sec = duration_sec; }
	[[nodiscard]] bool valid() const;
	[[nodiscard]] int get_current_time_stamp() const { return time_stamp_ms; }
	[[nodiscard]] int get_current_lrc_lines_count() const;
	[[nodiscard]] int get_current_lrc_node_index() const { return static_cast<int>(cur_lrc_node_index); }
	[[nodiscard]] int get_lrc_node_count() const { return static_cast<int>(lrc_nodes.GetCount()); }
	[[nodiscard]] int get_lrc_node_time_ms(int index) const { assert(index < lrc_nodes.GetCount());  return lrc_nodes[index]->get_time_ms(); }
	int get_current_lrc_line_at(int index, CString& out_str) const;
	int get_lrc_line_at(int lrc_node_index, int index, CString& out_str) const;
	[[nodiscard]] int get_current_lrc_line_aux_index(LrcAuxiliaryInfoNative info) const;
	[[nodiscard]] int get_lrc_line_aux_index(int lrc_node_index, LrcAuxiliaryInfoNative info) const;

	[[nodiscard]] int is_auxiliary_info_enabled(LrcAuxiliaryInfoNative enable_info) const
	{
		return (aux_enable_info & (1 << static_cast<int>(enable_info))) != 0;
	}
	void set_auxiliary_info_enabled(LrcAuxiliaryInfoNative enable_info)
	{
		aux_enable_info |= (1 << static_cast<int>(enable_info));
	}
	void clear_auxiliary_info_enabled(LrcAuxiliaryInfoNative enable_info)
	{
		aux_enable_info &= ~(1 << static_cast<int>(enable_info));
	}
	void reset_auxiliary_info_enabled() { aux_enable_info = 0; }
	bool is_percentage_enabled(int index) { return lrc_nodes[index]->is_lrc_percentage_enabled(); }
	float get_lrc_percentage(int index) { return lrc_nodes[index]->get_lrc_percentage(time_stamp_ms / 1000.0f); }

	// static helpers
	static LrcMetadataType get_metadata_type(const CString& str);
	static int cstring_hash_fnv_64bit_int(const CString& str);
	static CString get_metadata_value(const CString& str);
};

public enum class LrcAuxiliaryInfo
{
	Lyric = 0,
	Translation = 1,
	Romanization = 2,
	Ignored = 3
};

public ref class LrcFileController:
	System::IDisposable
{
	LrcFileControllerNative* native_handle;

	void check_if_null();

public:
	LrcFileController();

	void ParseLrcFile(System::String^ filePath);
	void ClearLrcNodes();
	void SetTimeStamp(int timeStampMs);
	void TimeStampIncrease(int ms);
	void SetSongDuration(float durationSec);

	bool Valid();
	int GetCurrentTimeStamp();
	int GetCurrentLrcLinesCount();
	int GetCurrentLrcNodeIndex();
	int GetLrcNodeCount();
	int GetLrcNodeTimeMs(int index);

	System::String^ GetCurrentLrcLineAt(int index);
	System::String^ GetLrcLineAt(int lrcNodeIndex, int index);

	int GetCurrentLrcLineAuxIndex(LrcAuxiliaryInfo info);
	int GetLrcLineAuxIndex(int lrcNodeIndex, LrcAuxiliaryInfo info);

	bool IsAuxiliaryInfoEnabled(LrcAuxiliaryInfo enableInfo);
	void SetAuxiliaryInfoEnabled(LrcAuxiliaryInfo enableInfo);
	void ClearAuxiliaryInfoEnabled(LrcAuxiliaryInfo enableInfo);
	void ResetAuxiliaryInfoEnabled();

	bool IsPercentageEnabled(int index);
	float GetLrcPercentage(int index);

	~LrcFileController();
};
	
}