#include "pch.h"
#include "AtlTraceRedirect.h"
#include "LrcFileController.h"

#include <msclr/marshal_cppstd.h>
#include <vcclr.h>

using namespace MusicPlayerLibrary;

bool IsRomajiSyllableToken(const CStringA& token)
{
    static const std::initializer_list<CStringA> romaji_tokens = {
        "a", "i", "u", "e", "o",
        "ka", "ki", "ku", "ke", "ko", "ga", "gi", "gu", "ge", "go",
        "sa", "shi", "su", "se", "so", "za", "ji", "zu", "ze", "zo",
        "ta", "chi", "tsu", "te", "to", "da", "de", "do",
        "na", "ni", "nu", "ne", "no",
        "ha", "hi", "fu", "he", "ho", "ba", "bi", "bu", "be", "bo", "pa", "pi", "pu", "pe", "po",
        "ma", "mi", "mu", "me", "mo",
        "ya", "yu", "yo",
        "ra", "ri", "ru", "re", "ro",
        "wa", "wo", "n",
        "kya", "kyu", "kyo", "gya", "gyu", "gyo",
        "sha", "shu", "sho", "ja", "ju", "jo",
        "cha", "chu", "cho",
        "nya", "nyu", "nyo", "hya", "hyu", "hyo",
        "bya", "byu", "byo", "pya", "pyu", "pyo",
        "mya", "myu", "myo", "rya", "ryu", "ryo"
    };

    for (const auto& romaji_token : romaji_tokens)
    {
        if (token == romaji_token)
        {
            return true;
        }
    }
    return false;
}

bool IsStrongSeparatedRomaji(const CString& input)
{
    CString lower = input;
    lower.MakeLower();
    CStringA text{ CT2A(lower) };
    CStringA token;
    int token_count = 0;
    bool has_separator = false;

    auto flush_token = [&token, &token_count]()
    {
        if (token.IsEmpty())
        {
            return true;
        }

        ++token_count;
        const bool is_valid = IsRomajiSyllableToken(token);
        token.Empty();
        return is_valid;
    };

    for (int i = 0; i < text.GetLength(); ++i)
    {
        const unsigned char c = static_cast<unsigned char>(text[i]);
        if (isspace(c) || c == '-' || c == '\'')
        {
            has_separator = true;
            if (!flush_token())
            {
                return false;
            }
            continue;
        }

        if (!isalpha(c))
        {
            return false;
        }

        token.AppendChar(static_cast<char>(c));
    }

    if (!flush_token())
    {
        return false;
    }

    return has_separator && token_count >= 2;
}

CSimpleArray<CString> SplitLrcForProgressMultiNode2(const CSimpleArray<CString>& texts)
{
    CSimpleArray<CString> strs;
    for (int i = 0; i < texts.GetSize(); ++i)
    {
        const auto& text = texts[i];
        auto new_text = CString();
        bool is_pressed = false;
        for (int j = 0; j < text.GetLength(); ++j)
        {
            if (text[j] == '[' || text[j] == '<')
            {
                is_pressed = true;
                continue;
            }
            if (text[j] == ']' || text[j] == '>')
            {
                is_pressed = false;
            }
            else
            {
                if (is_pressed) continue;
                new_text.AppendChar(text[j]);
            }
        }
        strs.Add(new_text);
    }
    return strs;
}


LrcMultiNode::LrcMultiNode(int t, const CSimpleArray<CString>& texts) :
    LrcAbstractNode(t), str_count(texts.GetSize()), lrc_texts(texts)
{
    for (int i = 0; i < str_count; ++i)
    {
        lang_types.Add(LrcLanguageHelper::detect_language_type(texts[i]));
        aux_infos.Add(LrcAuxiliaryInfoNative::Lyric);
    }
    int jp_index = lang_types.Find(LrcLanguageHelper::LanguageType::jp), 
        kr_index = lang_types.Find(LrcLanguageHelper::LanguageType::kr),
        eng_index = lang_types.Find(LrcLanguageHelper::LanguageType::en),
        zh_index = lang_types.Find(LrcLanguageHelper::LanguageType::zh);
    if (jp_index != -1)
    {
        aux_infos[jp_index] = LrcAuxiliaryInfoNative::Lyric;
        int jp_index_2;
        for (jp_index_2 = jp_index + 1; jp_index_2 < str_count; ++jp_index_2) {
            if (lang_types[jp_index_2] == LrcLanguageHelper::LanguageType::jp) {
                break;
            }
        }
        if (jp_index_2 != str_count) {
            // 一般是罗马音置前导致的误判，设置第二个为歌词
            aux_infos[jp_index_2] = LrcAuxiliaryInfoNative::Lyric;
            aux_infos[jp_index] = LrcAuxiliaryInfoNative::Ignored;
        }
        if (kr_index != -1)
        {
            ATLTRACE(_T("warn: jp & kr mix, ignoring kr line\n"));
            aux_infos[kr_index] = LrcAuxiliaryInfoNative::Ignored;
        }
    }
    if (jp_index == -1 && kr_index != -1)
    {
        aux_infos[kr_index] = LrcAuxiliaryInfoNative::Lyric;
    }


    if (zh_index != -1)
    {
        for (int i = zh_index + 1; i < str_count; ++i)
        {
            if (lang_types[i] == LrcLanguageHelper::LanguageType::zh)
            {
                aux_infos[i] = LrcAuxiliaryInfoNative::Translation; // 无法判断中文和日文，假定后出现的为翻译
                lang_types[zh_index] = LrcLanguageHelper::LanguageType::jp;
				aux_infos[zh_index] = LrcAuxiliaryInfoNative::Lyric;
				jp_index = zh_index;
				zh_index = i;
                break;
            }
        }
        if (jp_index != -1 || kr_index != -1 || eng_index != -1 && lang_types[zh_index] == LrcLanguageHelper::LanguageType::zh)
        {
//            ATLTRACE(_T("info: translation hit, line %s\n"), texts[zh_index].GetString());
            aux_infos[zh_index] = LrcAuxiliaryInfoNative::Translation;
        }
        else
        {
            aux_infos[zh_index] = LrcAuxiliaryInfoNative::Lyric;
        }
    }

    if (eng_index != -1)
    {
        float eng_prob, romaji_prob;
        const bool has_jp_or_kr_lyric = jp_index != -1 || kr_index != -1;
        const bool has_zh_only_pair = jp_index == -1 && kr_index == -1 && zh_index != -1;
        const bool is_strong_separated_romaji = has_zh_only_pair && IsStrongSeparatedRomaji(texts[eng_index]);
        LrcLanguageHelper::detect_eng_vs_jpn_romaji_prob(texts[eng_index], &eng_prob, &romaji_prob);
        if (eng_prob > romaji_prob)
        {
            aux_infos[eng_index] = LrcAuxiliaryInfoNative::Lyric;
        }
        else if (eng_prob < romaji_prob && (has_jp_or_kr_lyric || is_strong_separated_romaji))
        {
//            ATLTRACE(_T("info: romanization hit, line %s\n"), texts[eng_index].GetString());
            aux_infos[eng_index] = LrcAuxiliaryInfoNative::Romanization;
            if (is_strong_separated_romaji)
            {
                // 坑：英语实际为罗马音，中文应为歌词正文而非翻译
                // 注意此处要检查是否为强分隔罗马音，避免长得像英语的被一棒子打死
                aux_infos[zh_index] = LrcAuxiliaryInfoNative::Lyric;
            }
        }
        else if (jp_index != -1 && kr_index != -1)
        {
            ATLTRACE(_T("warn: unknown romaji, ignoring eng line: %s\n"), texts[eng_index].GetString());
            aux_infos[eng_index] = LrcAuxiliaryInfoNative::Ignored;
        }
    }
}

void LrcLanguageHelper::detect_eng_vs_jpn_romaji_prob(const CString& input, float* eng_prob, float* jpn_romaji_prob)
{
    CString lower = input;
    lower.MakeLower();
    CStringA str{CT2A(lower)};
    std::initializer_list<CStringA> romaji_syllables = {
        // 五十音图（去掉单元音，因为也同时包含在英语中）
        "ka", "ki", "ku", "ke", "ko", "sa", "shi", "su", "se", "so",
        "ta", "chi", "tsu", "te", "to", "na", "ni", "nu", "ne", "no",
        "ha", "hi", "fu", "he", "ho", "ma", "mi", "mu", "me", "mo",
        "ya", "yu", "yo", "ra", "ri", "ru", "re", "ro", "wa", "wo", "n"
    };
    std::initializer_list<CStringA> english_clusters = {
        // 常见英语辅音组合
        "th", "sh", "ph", "bl", "cl", "str", "spr", "pr", "tr", "dr"
    };

    int romaji_score = 0;
    int english_score = 0;
    float romaji_prob_out, eng_prob_out;

    for (auto& syl : romaji_syllables)
        if (str.Find(syl) != -1) romaji_score++;
    for (auto& cl : english_clusters)
        if (str.Find(cl) != -1) english_score++;

    int vowels = 0, consonants = 0;
    for (int i = 0; i < str.GetLength(); i++)
    {
        if (unsigned char c = str[i]; isalpha(c))
        {
            if (c == 'a' || c == 'e' || c == 'i' || c == 'o' || c == 'u')
                vowels++;
            else
                consonants++;
        }
    }
    double vowel_ratio = vowels + consonants > 0 ? (double)vowels / (vowels + consonants) : 0.0;
    if (vowel_ratio > 0.45) romaji_score++;
    else english_score++;

    if (auto total = static_cast<float>(romaji_score + english_score); total == 0)
    { // NOLINT(*-use-auto)
        eng_prob_out = 0.5f;
        romaji_prob_out = 0.5f;
    }
    else
    {
        eng_prob_out = (float)english_score / total;
        romaji_prob_out = (float)romaji_score / total;
    }
    if (fabs(eng_prob_out - romaji_prob_out) < 1e-6) 
    {
        eng_prob_out = 0.f;
        romaji_prob_out = 1.f;
    }
    *eng_prob = eng_prob_out;
    *jpn_romaji_prob = romaji_prob_out;
}

LrcLanguageHelper::LanguageType
LrcLanguageHelper::detect_language_type(const CString& input, float* probability)
{
    int zh = 0, jp = 0, kr = 0, en = 0;
    for (int i = 0; i < input.GetLength(); ++i)
    {
        if (wchar_t ch = input[i]; ch >= 0x4E00 && ch <= 0x9FFF) // zh character
            zh++;
        else if ((ch >= 0x3040 && ch <= 0x309F) || // hiragana
            (ch >= 0x30A0 && ch <= 0x30FF)) // katakana
            jp++;
        else if (ch >= 0xAC00 && ch <= 0xD7AF) // korean
            kr++;
        else if (ch <= 0x007F) // ANSI character, english
            en++;
    }

    float length = static_cast<float>(zh + jp + kr + en); // NOLINT(*-use-auto)
    if (length == 0) return LanguageType::others;

    float zh_score = static_cast<float>(zh); // NOLINT(*-use-auto)
    float jp_score = static_cast<float>(jp) * 2.f + static_cast<float>(zh) * 0.5f; // 日语中包含部分汉字，对假名进行加权
    float kr_score = static_cast<float>(kr); // NOLINT(*-use-auto)
    float en_score = static_cast<float>(en); // NOLINT(*-use-auto)

    auto write_prob = [&input, probability](const CString& out_type, float out_prob)
    {
        if (probability) *probability = out_prob;
//        ATLTRACE(_T("info: line %s, type = %s, max prob = %f\n"),
//                 input.GetString(), out_type.GetString(), out_prob);
    };

    if (zh > 0 && jp > 0)
    {
        write_prob(_T("jp"), jp_score / length);
        return LanguageType::jp;
    }

    if (zh > 0 && en > 0)
    {
        write_prob(_T("zh"), zh_score / length);
        return LanguageType::zh;
    }

	if (kr > 0 && en > 0)
    {
        write_prob(_T("kr"), kr_score / length);
        return LanguageType::kr;
    }

    if (jp > 0 && en > 0) {
        write_prob(_T("jp"), jp_score / length);
        return LanguageType::jp;
    }

    if (zh_score > jp_score && zh_score > en_score && zh_score > kr_score)
    {
        write_prob(_T("zh"), zh_score / length);
        return LanguageType::zh;
    }
    if (jp_score > zh_score && jp_score > en_score && jp_score > kr_score)
    {
        write_prob(_T("jp"), jp_score / length);
        return LanguageType::jp;
    }
    if (en_score > jp_score && en_score > kr_score && en_score > zh_score)
    {
        write_prob(_T("en"), en_score / length);
        return LanguageType::en;
    }
    if (kr_score > zh_score && kr_score > en_score && kr_score > jp_score)
    {
        write_prob(_T("kr"), kr_score / length);
        return LanguageType::kr;
    }
    return LanguageType::others;
}

LrcProgressNode::LrcProgressNode(int t, const CString& text_with_node)
    : LrcAbstractNode(t), node_count(0), end_time_ms(0)
{
    CString text = text_with_node;
    int find_right_brace_info;
    const TCHAR right_brace_type = text[text.GetLength() - 1];
    TCHAR left_brace_type;
    switch (right_brace_type)
    {
        case _T(']'): left_brace_type = _T('['); break;
        case _T('>'): left_brace_type = _T('<'); break;
        default: return;
    }
    do
    {
        find_right_brace_info = text.Find(right_brace_type);
        CString node = text.Left(find_right_brace_info + 1);
        if (node.IsEmpty())
            continue;
        auto node_controller_start_index = node.Find(left_brace_type);
        if (node_controller_start_index == -1)
        {
            ATLTRACE(_T("warn: invalid progress node: %s\n"), node.GetString());
            break;
        }
        auto time_stamp = node.Mid(node_controller_start_index + 1);
        time_stamp.Remove(right_brace_type);
        auto lyric_text = node.Left(node_controller_start_index);
        int minutes = _ttoi(time_stamp.Left(2));
        int seconds = _ttoi(time_stamp.Mid(3, 2));
        CString milliseconds_str = time_stamp.Mid(6);
        int milliseconds = _ttoi(milliseconds_str);
        if (milliseconds_str.GetLength() < 3)
        {
            auto multiples = 3 - milliseconds_str.GetLength();
            milliseconds *= std::floor(pow(10, multiples));
        }
        int total_ms = minutes * 60000 + seconds * 1000 + milliseconds;
        nodes.Add({
            .time_ms = total_ms,
            .node_text = lyric_text
        });
        node_count++;
        text = text.Mid(find_right_brace_info + 1);
    } while (find_right_brace_info != -1);
}

float LrcProgressNode::get_lrc_percentage(float current_timestamp) const
{
    auto base = nodes.GetSize();
    if (base == 0) return 1.0f;

    const int timestamp_in_ms = static_cast<int>(current_timestamp * 1000);
    if (timestamp_in_ms >= nodes[base - 1].time_ms)
    {
        return 1.0f;
    }
    if (timestamp_in_ms < time_ms)
    {
        return 0.0f;
    }

    int index;
    float percentage_in_node = 0.0f, distance = 0.0f, percentage = 0.0f;
    for (index = 0; index < base; ++index)
    {
        if (timestamp_in_ms < nodes[index].time_ms) break;
    }

    if (index != 0)
    {
        distance = static_cast<float>(nodes[index].time_ms - nodes[index - 1].time_ms);
        percentage_in_node = static_cast<float>(timestamp_in_ms - nodes[index - 1].time_ms) / distance;
    }
    else
    {
        // time_ms->start
        distance = static_cast<float>(nodes[0].time_ms - time_ms);
        if (distance > 0)
            percentage_in_node = static_cast<float>(timestamp_in_ms - time_ms) / distance;
        else
            percentage_in_node = 1.0f;
    }

    percentage = static_cast<float>(index) / base + (percentage_in_node / base);
    return percentage;
}

LrcProgressMultiNode::LrcProgressMultiNode
    (int t, const CString& str_1, const CSimpleArray<CString>& str_arr_2):
    LrcAbstractNode(t),
    LrcProgressNode(t, str_1),
    LrcMultiNode(t, SplitLrcForProgressMultiNode2(str_arr_2)) { }

LrcFileControllerNative::~LrcFileControllerNative()
{
    clear_lrc_nodes();
}

void LrcFileControllerNative::parse_lrc_file(const CString& file_path)
{
    clear_lrc_nodes();
    if (CFileStatus file_status;
        file_path.IsEmpty()
        || file_path.Find(_T(".lrc")) == -1
        || !CFile::GetStatus(file_path, file_status))
        return;
    try
    {
        CFile file(file_path, CFile::modeRead | CFile::typeBinary);
        parse_lrc_file_stream(&file);
    }
    catch (const CFileException*& ex)
    {
        CString err_msg;
        LPTSTR err_msg_buf = err_msg.GetBufferSetLength(1024);
        ex->GetErrorMessage(err_msg_buf, 1024);
        err_msg.ReleaseBuffer();
        ATLTRACE(_T("err: err in file open:%s\n"), err_msg.GetString());
    } // nothing happened, LrcFileController remains invalid
}

void LrcFileControllerNative::parse_lrc_file_stream(CFile* file_stream)
{
    // 目前仅支持逐行lrc解析
    if (file_stream == nullptr)
    {
        return;
    }
    clear_lrc_nodes();
    CStringA file_content_a;
    const int buf_size = 4096;
    char buffer[buf_size];
    UINT bytes_read = 0;
    do
    {
        bytes_read = file_stream->Read(buffer, buf_size - 1);
        buffer[bytes_read] = '\0';
        file_content_a += buffer;
    }
    while (bytes_read > 0);

    // 转换为宽字符
    int wide_len = MultiByteToWideChar(CP_UTF8, 0, file_content_a, -1, nullptr, 0);
    CString file_content_w;
    MultiByteToWideChar(CP_UTF8, 0, file_content_a, -1, file_content_w.GetBuffer(wide_len), wide_len);
    file_content_w.ReleaseBuffer();
    // 逐行解析
    int start = 0, flag_decoding_metadata = 1;
    std::stack<CString> lyrics_in_ms;
    int recorded_ms = 0;
    bool is_lrc_end = false;

    auto pump_stack = [&](bool is_lrc_ended)
    {
        CSimpleArray<CString> lrc_texts;
        while (!lyrics_in_ms.empty())
        {
            lrc_texts.Add(lyrics_in_ms.top());
            lyrics_in_ms.pop();
        }
        if (lrc_texts.GetSize() > 1)
            std::reverse(lrc_texts.GetData(), lrc_texts.GetData() + lrc_texts.GetSize());
        if (lrc_texts.GetSize() == 0)
            return;
        if (LrcAbstractNode* node = LrcNodeFactory::CreateLrcNode(recorded_ms, lrc_texts))
        {
            if (!lrc_nodes.IsEmpty())
            {
                lrc_nodes[lrc_nodes.GetCount() - 1]->set_lrc_end_timestamp(recorded_ms);
            }
            if (is_lrc_ended)
            {
                node->set_lrc_end_timestamp(std::floor(this->song_duration_sec * 1000));
            }
            lrc_nodes.Add(node);
            if (node->is_translation_enabled())
                this->set_auxiliary_info_enabled(LrcAuxiliaryInfoNative::Translation);
            if (node->is_romanization_enabled())
                this->set_auxiliary_info_enabled(LrcAuxiliaryInfoNative::Romanization);
        }
        else
        {
            // AfxMessageBox(_T("err: create lrc node failed, aborting!"), MB_ICONERROR);
            throw gcnew System::InvalidOperationException("Create lrc node failed, aborting!");
        }
    };

    while (start < file_content_w.GetLength())
    {
        int end = file_content_w.Find('\n', start);
        if (end == -1)
        {
            end = file_content_w.GetLength();
            is_lrc_end = true;
        }
        CString line = file_content_w.Mid(start, end - start).Trim();
        if (line.IsEmpty())
        {
            start = end + 1;
            continue;
        }
        if (line[0] == '{')
        {
            ATLTRACE(_T("warn: invalid ncm extension found, ignoring\n"));
            start = end + 1;
            continue;
        }
        auto line_start_index = line.Find(_T('['));
        // 剔除行开头的不合法字符
        if (line_start_index != -1 && line_start_index != 0)
        {
            ATLTRACE(_T("warn: invalid lrc format, ignoring start character: %s\n"), line.Left(line_start_index).GetString());
            line = line.Right(line.GetLength() - line_start_index);
        }
        if (flag_decoding_metadata)
        {
            // 走metadata解析，不遵守标准lrc解码
            switch (get_metadata_type(line))
            {
            case LrcMetadataType::Artist:
                metadata.artist = get_metadata_value(line);
                break;
            case LrcMetadataType::Album:
                metadata.album = get_metadata_value(line);
                break;
            case LrcMetadataType::Title:
                metadata.title = get_metadata_value(line);
                break;
            case LrcMetadataType::By:
                metadata.by = get_metadata_value(line);
                break;
            case LrcMetadataType::Offset:
                lrc_offset_ms = _ttoi(get_metadata_value(line));
                break;
            case LrcMetadataType::Author:
                metadata.author = get_metadata_value(line);
                break;
            case LrcMetadataType::Ignored:
                break;
            case LrcMetadataType::Error: default:
                flag_decoding_metadata = 0;
                break;
            }
            if (flag_decoding_metadata)
            {
                start = end + 1;
                continue;
            }
        }
        // 解析时间tag
        if (line.GetLength() < 10)
        {
            // AfxMessageBox(_T("err: invalid lrc line, aborting!"), MB_ICONERROR);
            // clear stack
            while (!lyrics_in_ms.empty())
            {
                lyrics_in_ms.pop();
            }
            clear_lrc_nodes();
            throw gcnew System::InvalidOperationException("Invalid lrc line, aborting!");
        }
        int time_tag_end_index = line.Find(']');
        if (time_tag_end_index == -1 || line[0] != '[' || line[3] != ':' || 
            (line[6] != '.' && line[6] != ':'))
        {
            // (_T("err: invalid lrc time tag, aborting!"), MB_ICONERROR);
            while (!lyrics_in_ms.empty())
            {
                lyrics_in_ms.pop();
            }
            clear_lrc_nodes();
            throw gcnew System::InvalidOperationException("Invalid lrc time tag, aborting!");
        }
        int minutes = _ttoi(line.Mid(1, 2));
        int seconds = _ttoi(line.Mid(4, 2));
        CString milliseconds_str = line.Mid(7, time_tag_end_index - 7);
        int milliseconds = _ttoi(milliseconds_str);
        if (milliseconds_str.GetLength() < 3)
        {
            auto multiples = 3 - milliseconds_str.GetLength();
            milliseconds *= std::floor(pow(10, multiples));
        }

        switch (int total_ms = minutes * 60000 + seconds * 1000 + milliseconds + lrc_offset_ms; WAY3RES(
            total_ms <=> recorded_ms))
        {
        case ThreeWayCompareResult::Less: // 歌词时间戳一定有序
            // AfxMessageBox(_T("err: invalid time stamp order!"), MB_ICONERROR);
            while (!lyrics_in_ms.empty())
            {
                lyrics_in_ms.pop();
            }
            clear_lrc_nodes();
            throw gcnew System::InvalidOperationException("Invalid time stamp order!");
        case ThreeWayCompareResult::Greater:
            // 先处理之前的歌词
            if (total_ms < 0) total_ms = 0;
            if (!lyrics_in_ms.empty())
                pump_stack(is_lrc_end);
            recorded_ms = total_ms;
            break;
        default:
            break;
        }
        lyrics_in_ms.push(line.Mid(time_tag_end_index + 1).Trim());
        start = end + 1;
    }
    pump_stack(is_lrc_end);
    cur_lrc_node_index = 0;
}

void LrcFileControllerNative::clear_lrc_nodes()
{
    for (size_t i = 0; i < lrc_nodes.GetCount(); i++)
    {
        delete lrc_nodes[i];
    }
    lrc_nodes.RemoveAll();
}

void LrcFileControllerNative::set_time_stamp(int time_stamp_ms_in)
{
    if (time_stamp_ms_in < time_stamp_ms)
    {
        // reverse query, set index to zero
        cur_lrc_node_index = 0;
    }
    bool found = false;
    for (size_t i = cur_lrc_node_index; i < lrc_nodes.GetCount(); i++)
    {
        if (lrc_nodes[i]->get_time_ms() > time_stamp_ms_in)
        {
            cur_lrc_node_index = i == 0 ? 0 : i - 1;
            found = true;
            break;
        }
    }
    if (!found)
    {
        cur_lrc_node_index = lrc_nodes.GetCount() - 1;
    }
    time_stamp_ms = time_stamp_ms_in;
}

void LrcFileControllerNative::time_stamp_increase(int ms)
{
    time_stamp_ms += ms;
    set_time_stamp(time_stamp_ms);
}

bool LrcFileControllerNative::valid() const
{
    return lrc_nodes.GetCount() > 0 && song_duration_sec >= 0;
}

int LrcFileControllerNative::get_current_lrc_lines_count() const
{
    return lrc_nodes[cur_lrc_node_index]->get_lrc_str_count();
}

int LrcFileControllerNative::get_current_lrc_line_at(int index, CString& out_str) const
{
    return lrc_nodes[cur_lrc_node_index]->get_lrc_str_at(index, out_str);
}

int LrcFileControllerNative::get_lrc_line_at(int lrc_node_index, int index, CString& out_str) const
{
    assert(
        lrc_node_index >= 0 && lrc_node_index < lrc_nodes.GetCount() && index >= 0 && index < lrc_nodes[lrc_node_index]
        ->get_lrc_str_count());
    return lrc_nodes[lrc_node_index]->get_lrc_str_at(index, out_str);
}

int LrcFileControllerNative::get_current_lrc_line_aux_index(LrcAuxiliaryInfoNative info) const
{
    return lrc_nodes[cur_lrc_node_index]->get_auxiliary_info_at(info);
}

int LrcFileControllerNative::get_lrc_line_aux_index(int lrc_node_index, LrcAuxiliaryInfoNative info) const
{
    assert(lrc_node_index >= 0 && lrc_node_index < lrc_nodes.GetCount());
    return lrc_nodes[lrc_node_index]->get_auxiliary_info_at(info);
}

LrcMetadataType LrcFileControllerNative::get_metadata_type(const CString& str)
{
    if (str.IsEmpty() || str.GetLength() < 3 || str[0] != '[')
    {
        return LrcMetadataType::Error;
    }
    // 逐字歌词有可能每个单位后都带有时间戳
    if (str.Find(']') != str.GetLength() - 1)
        return LrcMetadataType::Error;
    int metadata_end_index = str.Find(':', 1);
    if (metadata_end_index == -1)
        return LrcMetadataType::Error;

    switch (CString metadata_type_str = str.Left(metadata_end_index).Mid(1);
        cstring_hash_fnv_64bit_int(metadata_type_str))
    {
    case 0x645d220c: return LrcMetadataType::Artist;
    case 0x63d58dce: return LrcMetadataType::Album;
    case 0x0387b4f0: return LrcMetadataType::Title;
    case 0x27a9be4e: return LrcMetadataType::By;
    case 0x4f6518ce: return LrcMetadataType::Offset;
    case 0x642cb63f: return LrcMetadataType::Author;
    default: return LrcMetadataType::Ignored;
    }
}

int LrcFileControllerNative::cstring_hash_fnv_64bit_int(const CString& str)
{
    const TCHAR* p = str.GetString();
    const int len = str.GetLength();
    unsigned long long h = 14695981039346656037ull; // fnv offset basis
    const unsigned char* bytes = reinterpret_cast<const unsigned char*>(p); // NOLINT(*-use-auto)
    const size_t count = static_cast<size_t>(len) * sizeof(TCHAR);
    for (size_t i = 0; i < count; ++i)
    {
        h ^= bytes[i];
        h *= 1099511628211ull; // fnv prime
    }
    return static_cast<int>(h % 0x7fffffffull); // switch-case requires int
}

CString LrcFileControllerNative::get_metadata_value(const CString& str)
{
    int metadata_end_index = str.Find(':', 1);
    return str.Mid(metadata_end_index + 1).Trim(']').Trim();
}

// ============================================================
// Managed wrapper: LrcFileController
// ============================================================

static LrcAuxiliaryInfoNative ToNativeAuxInfo(LrcAuxiliaryInfo info)
{
    switch (info)
    {
    case LrcAuxiliaryInfo::Lyric:          return LrcAuxiliaryInfoNative::Lyric;
    case LrcAuxiliaryInfo::Translation:    return LrcAuxiliaryInfoNative::Translation;
    case LrcAuxiliaryInfo::Romanization:   return LrcAuxiliaryInfoNative::Romanization;
    case LrcAuxiliaryInfo::Ignored:        return LrcAuxiliaryInfoNative::Ignored;
    default:                               return LrcAuxiliaryInfoNative::Ignored;
    }
}

LrcFileController::LrcFileController()
{
    native_handle = new LrcFileControllerNative();
}

void LrcFileController::check_if_null()
{
    if (!native_handle)
        throw gcnew System::InvalidOperationException("LrcFileControllerNative is not initialized!");
}

void LrcFileController::ParseLrcFile(System::String^ filePath)
{
    check_if_null();
    pin_ptr<const wchar_t> wch = PtrToStringChars(filePath);
    CString mfcPath(wch);
    native_handle->parse_lrc_file(mfcPath);
}

void LrcFileController::ParseLrcStream(System::String^ lrcString)
{
    check_if_null();
    pin_ptr<const wchar_t> wch = PtrToStringChars(lrcString);
    int utf8Len = WideCharToMultiByte(CP_UTF8, 0, wch, -1, nullptr, 0, nullptr, nullptr);
    CStringA utf8Str;
    WideCharToMultiByte(CP_UTF8, 0, wch, -1, utf8Str.GetBuffer(utf8Len), utf8Len, nullptr, nullptr);
    utf8Str.ReleaseBuffer();
    CMemFile mfcMemFile;
    mfcMemFile.Write(utf8Str.GetString(), static_cast<UINT>(utf8Str.GetLength()));
    mfcMemFile.SeekToBegin();
    native_handle->parse_lrc_file_stream(&mfcMemFile);
}

void LrcFileController::ClearLrcNodes()
{
    check_if_null();
    native_handle->clear_lrc_nodes();
}

void LrcFileController::SetTimeStamp(int timeStampMs)
{
    check_if_null();
    native_handle->set_time_stamp(timeStampMs);
}

void LrcFileController::TimeStampIncrease(int ms)
{
    check_if_null();
    native_handle->time_stamp_increase(ms);
}

void LrcFileController::SetSongDuration(float durationSec)
{
    check_if_null();
    native_handle->set_song_duration(durationSec);
}

bool LrcFileController::Valid()
{
    check_if_null();
    return native_handle->valid();
}

int LrcFileController::GetCurrentTimeStamp()
{
    check_if_null();
    return native_handle->get_current_time_stamp();
}

int LrcFileController::GetCurrentLrcLinesCount()
{
    check_if_null();
    return native_handle->get_current_lrc_lines_count();
}

int LrcFileController::GetCurrentLrcNodeIndex()
{
    check_if_null();
    return native_handle->get_current_lrc_node_index();
}

int LrcFileController::GetLrcNodeCount()
{
    check_if_null();
    return native_handle->get_lrc_node_count();
}

int LrcFileController::GetLrcNodeTimeMs(int index)
{
    check_if_null();
    return native_handle->get_lrc_node_time_ms(index);
}

System::String^ LrcFileController::GetCurrentLrcLineAt(int index)
{
    check_if_null();
    CString out_str;
    int result = native_handle->get_current_lrc_line_at(index, out_str);
    if (result != 0)
        return nullptr;
    return msclr::interop::marshal_as<System::String^>(out_str.GetString());
}

System::String^ LrcFileController::GetLrcLineAt(int lrcNodeIndex, int index)
{
    check_if_null();
    CString out_str;
    int result = native_handle->get_lrc_line_at(lrcNodeIndex, index, out_str);
    if (result != 0)
        return nullptr;
    return msclr::interop::marshal_as<System::String^>(out_str.GetString());
}

int LrcFileController::GetCurrentLrcLineAuxIndex(LrcAuxiliaryInfo info)
{
    check_if_null();
    return native_handle->get_current_lrc_line_aux_index(ToNativeAuxInfo(info));
}

int LrcFileController::GetLrcLineAuxIndex(int lrcNodeIndex, LrcAuxiliaryInfo info)
{
    check_if_null();
    return native_handle->get_lrc_line_aux_index(lrcNodeIndex, ToNativeAuxInfo(info));
}

bool LrcFileController::IsAuxiliaryInfoEnabled(LrcAuxiliaryInfo enableInfo)
{
    check_if_null();
    return native_handle->is_auxiliary_info_enabled(ToNativeAuxInfo(enableInfo));
}

void LrcFileController::SetAuxiliaryInfoEnabled(LrcAuxiliaryInfo enableInfo)
{
    check_if_null();
    native_handle->set_auxiliary_info_enabled(ToNativeAuxInfo(enableInfo));
}

void LrcFileController::ClearAuxiliaryInfoEnabled(LrcAuxiliaryInfo enableInfo)
{
    check_if_null();
    native_handle->clear_auxiliary_info_enabled(ToNativeAuxInfo(enableInfo));
}

void LrcFileController::ResetAuxiliaryInfoEnabled()
{
    check_if_null();
    native_handle->reset_auxiliary_info_enabled();
}

bool LrcFileController::IsPercentageEnabled(int index)
{
    check_if_null();
    return native_handle->is_percentage_enabled(index);
}

float LrcFileController::GetLrcPercentage(int index)
{
    check_if_null();
    return native_handle->get_lrc_percentage(index);
}

LrcFileController::~LrcFileController()
{
    delete native_handle;
    native_handle = nullptr;
}

