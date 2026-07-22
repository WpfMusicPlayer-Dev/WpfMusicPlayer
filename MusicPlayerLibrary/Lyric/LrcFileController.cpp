// SPDX-License-Identifier: MIT

#include "pch.h"
#include "Lyric/LrcFileController.h"
#include "Core/FileIO.h"
#include "Core/LocaleConverter.h"
#include "Core/StringUtilities.h"
#include "Lyric/MLPipeline/NCNNPipeline.h"

#include <algorithm>
#include <array>
#include <cstdlib>
#include <initializer_list>
#include <optional>
#include <stack>
#include <ranges>
#include <stdexcept>
#include <string_view>
#include <utility>

#include <rapidjson/stringbuffer.h>
#include <rapidjson/writer.h>
#include <rapidjson/prettywriter.h>

using namespace MusicPlayerLibrary;

namespace
{
template <typename TWriter>
void WriteUtf8String(TWriter& writer, const std::string& value)
{
    writer.String(value.data(), static_cast<rapidjson::SizeType>(value.size()));
}

const char* AuxInfoToRole(LrcAuxiliaryInfoNative info)
{
    switch (info)
    {
    case LrcAuxiliaryInfoNative::Lyric: return "lyric";
    case LrcAuxiliaryInfoNative::Translation: return "translation";
    case LrcAuxiliaryInfoNative::Romanization: return "romanization";
    case LrcAuxiliaryInfoNative::Ignored:
    default: return "ignored";
    }
}

std::string RomanizationSchemaFromClassification(LrcLanguageHelper::LanguageClassification classification)
{
    using LC = LrcLanguageHelper::LanguageClassification;
    switch (classification)
    {
    case LC::zh_jyut: return "jyutping";
    case LC::jp_roma: case LC::jp_zh_trans_roma:
    case LC::kr_roma: case LC::kr_zh_trans_roma: return "romaji";
    default: return std::string{};    
    }
    
}

template <typename TWriter>
void WriteMetadataField(TWriter& writer, const char* key, const std::string& value)
{
    if (value.empty())
        return;

    writer.Key(key);
    WriteUtf8String(writer, value);
}

constexpr double InlineSlashTranslationRatioThreshold = 0.5;

using LanguageType = LrcLanguageHelper::LanguageType;
using LanguageClassification = LrcLanguageHelper::LanguageClassification;

struct LanguageDescriptor final
{
    LanguageType type;
    std::string_view name;
};

constexpr std::array LanguageDescriptors{
    LanguageDescriptor{LanguageType::zh, "zh"},
    LanguageDescriptor{LanguageType::jp, "jp"},
    LanguageDescriptor{LanguageType::kr, "kr"},
    LanguageDescriptor{LanguageType::latin, "latin"},
    LanguageDescriptor{LanguageType::ru, "ru"},
    LanguageDescriptor{LanguageType::jyut, "jyut"},
    LanguageDescriptor{LanguageType::roma, "roma"},
    LanguageDescriptor{LanguageType::onomatopoeia, "onomatopoeia"},
};

struct MetadataDescriptor final
{
    std::string_view tag;
    std::string_view alias;
    const char* json_key;
    std::string LrcMetadata::* member;
};

constexpr std::array MetadataDescriptors{
    MetadataDescriptor{"ar", "artist", "artist", &LrcMetadata::artist},
    MetadataDescriptor{"al", "album", "album", &LrcMetadata::album},
    MetadataDescriptor{"au", "author", "author", &LrcMetadata::author},
    MetadataDescriptor{"by", "", "by", &LrcMetadata::by},
    MetadataDescriptor{"ti", "title", "title", &LrcMetadata::title},
};

struct ParsedMetadataTag final
{
    const MetadataDescriptor* descriptor = nullptr;
    bool is_offset = false;
    std::string value;
};

std::optional<ParsedMetadataTag> TryParseMetadataTag(
    const std::string_view text)
{
    if (text.size() < 3 || text.front() != '[' || text.back() != ']')
        return std::nullopt;
    const std::size_t separator = text.find(':', 1);
    if (separator == std::string_view::npos)
        return std::nullopt;

    std::string tag(text.substr(1, separator - 1));
    for (char& character : tag)
        character = ToLowerAscii(character);

    std::string_view value = text.substr(separator + 1);
    while (!value.empty() && value.front() == ']')
        value.remove_prefix(1);
    while (!value.empty() && value.back() == ']')
        value.remove_suffix(1);

    auto isDigit = [](const std::string& str) {
            return !str.empty() && std::ranges::all_of(str, ::isdigit);
        };
    if (isDigit(tag)) return std::nullopt;

    if (tag == "offset")
    {
        return ParsedMetadataTag{
            .is_offset = true,
            .value = LrcTextParser::TrimAsciiWhitespace(value)
        };
    }
    const MetadataDescriptor* descriptor = nullptr;
    for (const auto& candidate : MetadataDescriptors)
    {
        if (tag == candidate.tag ||
            (!candidate.alias.empty() && tag == candidate.alias))
        {
            descriptor = &candidate;
            break;
        }
    }
    std::string parsed_value = LrcTextParser::TrimAsciiWhitespace(value);
    return ParsedMetadataTag{
        .descriptor = descriptor,
        .value = parsed_value
    };
}

bool DecodeMetadataTag(
    const std::string_view text,
    LrcMetadata& metadata,
    int& offset_ms)
{
    const auto parsed = TryParseMetadataTag(text);
    if (!parsed)
        return false;
    if (parsed->is_offset)
    {
        offset_ms = static_cast<int>(
            std::strtol(parsed->value.c_str(), nullptr, 10));
    }
    else if (parsed->descriptor != nullptr)
    {
        metadata.*(parsed->descriptor->member) = parsed->value;
    }
    return true;
}

// Keep this order synchronized with the song-classification model labels.
constexpr std::array ClassificationModelOrder{
    LanguageClassification::zh_only,
    LanguageClassification::jp_only,
    LanguageClassification::kr_only,
    LanguageClassification::latin_only,
    LanguageClassification::ru_only,
    LanguageClassification::jp_zh_trans,
    LanguageClassification::jp_roma,
    LanguageClassification::latin_zh_trans,
    LanguageClassification::ru_zh_trans,
    LanguageClassification::kr_zh_trans,
    LanguageClassification::kr_roma,
    LanguageClassification::zh_jyut,
    LanguageClassification::jp_zh_trans_roma,
    LanguageClassification::kr_zh_trans_roma,
};

}

LrcMultiNode::LrcMultiNode(
    const int t,
    std::vector<std::string>&& texts,
    const LrcLanguageHelper::LanguageClassification classification,
    std::vector<LrcLanguageHelper::LanguageType>&& recommend_slot)
    : LrcAbstractNode(t),
      lrc_texts(std::move(texts))
{
    aux_infos.resize(lrc_texts.size(), LrcAuxiliaryInfoNative::Ignored);
    using LC = LrcLanguageHelper::LanguageClassification;
    if (recommend_slot.size() == lrc_texts.size())
    {
        lang_types = std::move(recommend_slot);
    }
    else
    {
        lang_types.reserve(lrc_texts.size());
        for (std::size_t i = 0; i < lrc_texts.size(); ++i)
        {
            lang_types.push_back(
                LrcLanguageHelper::GetSingleton().detect_line_language_type(lrc_texts[i]));
        }
    }

    std::array<int, LanguageDescriptors.size()> first_indices;
    first_indices.fill(-1);
    int second_zh_index = -1;
    for (int index = 0; index < static_cast<int>(lang_types.size()); ++index)
    {
        const int model_index = LrcLanguageHelper::LanguageTypeModelIndex(lang_types[index]);
        if (model_index >= 0 && first_indices[model_index] == -1)
            first_indices[model_index] = index;
        else if (lang_types[index] == LrcLanguageHelper::LanguageType::zh &&
            second_zh_index == -1)
        {
            second_zh_index = index;
        }
    }
    const auto first_index = [&](const LrcLanguageHelper::LanguageType type)
    {
        return first_indices[LrcLanguageHelper::LanguageTypeModelIndex(type)];
    };
    const int jp_index = first_index(LrcLanguageHelper::LanguageType::jp);
    const int kr_index = first_index(LrcLanguageHelper::LanguageType::kr);
    const int latin_index = first_index(LrcLanguageHelper::LanguageType::latin);
    const int zh_index = first_index(LrcLanguageHelper::LanguageType::zh);
    const int ru_index = first_index(LrcLanguageHelper::LanguageType::ru);
    const int jyut_index = first_index(LrcLanguageHelper::LanguageType::jyut);
    const int roma_index = first_index(LrcLanguageHelper::LanguageType::roma);
    const int onomatopoeia_index = first_index(LrcLanguageHelper::LanguageType::onomatopoeia);
    auto assign_with_language = [&](int index, LrcAuxiliaryInfoNative type)
    {
        if (index != -1) aux_infos[index] = type;
    };
    const auto assign_first_available =
        [&](const LrcAuxiliaryInfoNative role,
            const std::initializer_list<int> candidates)
    {
        for (const int index : candidates)
        {
            if (index != -1)
            {
                assign_with_language(index, role);
                return;
            }
        }
    };
    const auto assign_lyric_translation =
        [&](const int lyric_index, const int translation_index)
    {
        assign_with_language(lyric_index, LrcAuxiliaryInfoNative::Lyric);
        assign_with_language(
            translation_index, LrcAuxiliaryInfoNative::Translation);
    };
    switch (classification)
    {
    case LC::latin_only:
        {
            assign_with_language(latin_index, LrcAuxiliaryInfoNative::Lyric);
            break;
        }
    case LC::jp_only:
        {
            assign_with_language(jp_index, LrcAuxiliaryInfoNative::Lyric);
            break;
        }
    case LC::zh_only:
        {
            assign_with_language(zh_index, LrcAuxiliaryInfoNative::Lyric);
            break;
        }
    case LC::kr_only:
        {
            assign_with_language(kr_index, LrcAuxiliaryInfoNative::Lyric); 
            break;
        }
    case LC::ru_only:
        {
            assign_with_language(ru_index, LrcAuxiliaryInfoNative::Lyric);
            break;
        }
    case LC::zh_jyut:
        {
            assign_with_language(zh_index, LrcAuxiliaryInfoNative::Lyric);
            assign_with_language(jyut_index, LrcAuxiliaryInfoNative::Romanization);
            if (jyut_index == -1)
            {
                if (latin_index != -1)
                    assign_with_language(latin_index, LrcAuxiliaryInfoNative::Romanization);
                if (roma_index != -1)
                    assign_with_language(roma_index, LrcAuxiliaryInfoNative::Romanization);
                else
                    assign_with_language(onomatopoeia_index, LrcAuxiliaryInfoNative::Romanization);
            }
            break;
        }
    case LC::jp_roma:
        {
            assign_with_language(jp_index, LrcAuxiliaryInfoNative::Lyric);
            assign_with_language(roma_index, LrcAuxiliaryInfoNative::Romanization);
            if (roma_index == -1)
            {
                assign_with_language(onomatopoeia_index, LrcAuxiliaryInfoNative::Romanization);
            }
            if (jp_index == -1)
                assign_first_available(
                    LrcAuxiliaryInfoNative::Lyric, {zh_index, latin_index});
            break;
        }
    case LC::kr_roma:
        {
            assign_with_language(kr_index, LrcAuxiliaryInfoNative::Lyric);
            assign_with_language(roma_index, LrcAuxiliaryInfoNative::Romanization);
            if (kr_index == -1)
                assign_first_available(
                    LrcAuxiliaryInfoNative::Lyric, {zh_index, latin_index});
            break;
        }
    case LC::latin_zh_trans:
        {
            assign_lyric_translation(latin_index, zh_index);
            if (latin_index == -1)
                assign_first_available(LrcAuxiliaryInfoNative::Lyric,
                    {jp_index, kr_index, jyut_index, onomatopoeia_index});
            break;
        }
    case LC::ru_zh_trans:
        {
            assign_lyric_translation(ru_index, zh_index);
            if (ru_index == -1)
                assign_first_available(LrcAuxiliaryInfoNative::Lyric,
                    {jp_index, kr_index, jyut_index, latin_index,
                        onomatopoeia_index});
            break;
        }
    case LC::jp_zh_trans:
        {
            assign_lyric_translation(jp_index, zh_index);
            if (jp_index == -1)
            {
                if (zh_index != -1)
                {
                    if (second_zh_index != -1)
                        assign_lyric_translation(zh_index, second_zh_index);
                    else if (latin_index != -1)
                        assign_lyric_translation(latin_index, zh_index);
                    else if (onomatopoeia_index != -1)
                    {
                        assign_with_language(onomatopoeia_index, LrcAuxiliaryInfoNative::Lyric);
                    }
                    else
                    {
                        assign_with_language(zh_index, LrcAuxiliaryInfoNative::Lyric);
                    }
                }
                else
                    assign_first_available(LrcAuxiliaryInfoNative::Lyric,
                        {latin_index, onomatopoeia_index});
            } 
            else if (zh_index == -1)
            {
                if (latin_index != -1)
                    assign_with_language(latin_index, LrcAuxiliaryInfoNative::Translation);
            }
            break;
        }
    case LC::kr_zh_trans:
        {
            assign_lyric_translation(kr_index, zh_index);
            if (kr_index == -1)
                assign_first_available(LrcAuxiliaryInfoNative::Lyric,
                    {zh_index, latin_index, onomatopoeia_index});
            break;
        }
    case LC::jp_zh_trans_roma:
        {
            assign_lyric_translation(jp_index, zh_index);
            assign_with_language(roma_index, LrcAuxiliaryInfoNative::Romanization);
            if (jp_index == -1)
            {
                if (zh_index != -1)
                {
                    if (second_zh_index != -1)
                        assign_lyric_translation(zh_index, second_zh_index);
                    if (roma_index == -1)
                    {
                        // 信任ML分类器产生的eng识别结果，假定其为歌词。
                        if (latin_index != -1)
                            assign_lyric_translation(latin_index, zh_index);
                        else
                        {
                            assign_with_language(zh_index, LrcAuxiliaryInfoNative::Lyric);
                            assign_with_language(onomatopoeia_index, LrcAuxiliaryInfoNative::Romanization);
                        }
                    }
                    else
                    {
                        assign_with_language(zh_index, LrcAuxiliaryInfoNative::Lyric);
                        assign_with_language(roma_index, LrcAuxiliaryInfoNative::Romanization);
                    }
                }
                else if (latin_index != -1)
                {
                    assign_with_language(latin_index, LrcAuxiliaryInfoNative::Lyric);
                    assign_with_language(onomatopoeia_index, LrcAuxiliaryInfoNative::Romanization);
                }
            }
            else if (roma_index == -1)
            {
                if (latin_index != -1)
                    assign_with_language(latin_index, LrcAuxiliaryInfoNative::Romanization);
                else
                    assign_with_language(onomatopoeia_index, LrcAuxiliaryInfoNative::Romanization);
            }
            break;
        }
    case LC::kr_zh_trans_roma:
        {
            assign_lyric_translation(kr_index, zh_index);
            assign_with_language(roma_index, LrcAuxiliaryInfoNative::Romanization);
            if (kr_index == -1)
                assign_first_available(
                    LrcAuxiliaryInfoNative::Lyric, {zh_index, latin_index});
            if (roma_index == -1)
            {
                assign_with_language(onomatopoeia_index, LrcAuxiliaryInfoNative::Romanization);
            }
            break;
        }
    default:
        break;
    }
}

LrcLanguageHelper::LrcLanguageHelper()
{
    auto line_model_file = MLPipeline::NcnnModelFiles{
        .param = L"lyric_lang_mlp.ncnn.param",
        .weights = L"lyric_lang_mlp.ncnn.bin"
    };
    auto song_model_file = MLPipeline::NcnnModelFiles{
        .param = L"song_structure_mlp.ncnn.param",
        .weights = L"song_structure_mlp.ncnn.bin"
    };

    line_net_reasoning =
        std::make_unique<MLPipeline::NcnnClassifier>(line_model_file);
    line_vocab_reasoning = MLPipeline::load_vocab(
        L"lyric_lang_vocab.bin",
        line_net_reasoning->model_fingerprint());
    if (line_vocab_reasoning.size() != line_net_reasoning->input_size())
    {
        throw std::runtime_error(
            "vocabulary has " + std::to_string(line_vocab_reasoning.size()) +
            " entries; NCNN model expects " + std::to_string(line_net_reasoning->input_size()));
    }
    song_net_reasoning =
        std::make_unique<MLPipeline::NcnnClassifier>(song_model_file);
}

LrcLanguageHelper::~LrcLanguageHelper()
{
    release_native_resources();
}

void LrcLanguageHelper::release_native_resources() noexcept
{
    accepting_inference.store(false, std::memory_order_release);
    std::lock_guard lock(dlib_mutex);
    song_net_reasoning.reset();
    line_net_reasoning.reset();
    line_vocab_reasoning.clear();
}

std::string_view LrcLanguageHelper::LanguageTypeName(const LanguageType type) noexcept
{
    return LanguageDescriptors[LanguageTypeModelIndex(type)].name;
}

int LrcLanguageHelper::LanguageTypeModelIndex(const LanguageType type) noexcept
{
    for (std::size_t index = 0; index < LanguageDescriptors.size(); ++index)
        if (LanguageDescriptors[index].type == type)
            return static_cast<int>(index);
    return static_cast<int>(LanguageDescriptors.size() - 1);
}

LrcLanguageHelper::LanguageType LrcLanguageHelper::LanguageTypeFromModelIndex(
    const int index) noexcept
{
    if (index < 0 || index >= static_cast<int>(LanguageDescriptors.size()))
        return LanguageType::onomatopoeia;
    return LanguageDescriptors[index].type;
}

LrcLanguageHelper::LanguageClassification LrcLanguageHelper::ClassificationFromModelIndex(
    const int index) noexcept
{
    if (index < 0 || index >= static_cast<int>(ClassificationModelOrder.size()))
        return LanguageClassification::latin_only;
    return ClassificationModelOrder[index];
}

std::vector<double> LrcLanguageHelper::extract_line_features(const std::string& text,
                                                             const MLPipeline::Vocabulary& vocab)
{
    std::vector x(vocab.size(), 0.0);

    for (size_t i = 0; i < text.size(); ++i)
    {
        for (std::size_t n = MLPipeline::minimum_token_size;
             n <= MLPipeline::maximum_token_size; ++n)
        {
            if (i + n > text.size()) continue;
            std::string gram = text.substr(i, n);
            auto it = vocab.find(gram);
            if (it != vocab.end())
                x[it->second] += 1.0;
        }
    }
    return x;
}

int LrcLanguageHelper::predict_locked(
    MLPipeline::NcnnClassifier* const classifier,
    const std::vector<double>& features)
{
    if (!accepting_inference.load(std::memory_order_acquire) || !classifier)
        throw std::runtime_error("native lyric inference is shutting down");
    const std::vector<float> float_features(features.begin(), features.end());
    return classifier->predict(float_features);
}

LrcLanguageHelper::LanguageType
LrcLanguageHelper::detect_line_language_type(const std::string& input)
{
    std::lock_guard lock(dlib_mutex);
    const auto features = extract_line_features(input, line_vocab_reasoning);
    return LanguageTypeFromModelIndex(
        predict_locked(line_net_reasoning.get(), features));
}

std::vector<double>
LrcLanguageHelper::extract_song_features(const std::vector<LrcLanguageHelper::LanguageType>& seq)
{
    constexpr std::size_t LanguageCount = LanguageDescriptors.size();
    std::vector<double> feat;
    feat.reserve(LanguageCount * LanguageCount + LanguageCount * 2 + 4);

    // language count map
    std::array<int, LanguageCount> count{};
    for (const auto type : seq)
        ++count[LanguageTypeModelIndex(type)];

    // language count (8 rows)
    for (const int language_count : count)
        feat.push_back(language_count);

    // language proportion (8 rows)
    const double total = static_cast<double>(seq.size());
    for (const int language_count : count)
        feat.push_back(seq.empty() ? 0.0 : language_count / total);

    // bigram matrix (64 rows)
    std::array<std::array<int, LanguageCount>, LanguageCount> trans{};
    for (size_t i = 1; i < seq.size(); i++)
    {
        ++trans[LanguageTypeModelIndex(seq[i - 1])][LanguageTypeModelIndex(seq[i])];
    }

    for (const auto& source : trans)
        for (const int transition_count : source)
            feat.push_back(transition_count);

    // language switching count (1 row)
    int switches = 0;
    for (size_t i = 1; i < seq.size(); i++)
        if (seq[i] != seq[i - 1])
            switches++;
    feat.push_back(switches);

    // translation included? (1 row)
    const auto count_of = [&count](const LanguageType type)
    {
        return count[LanguageTypeModelIndex(type)];
    };
    feat.push_back(count_of(LanguageType::zh) > 0 &&
        (count_of(LanguageType::jp) > 0 ||
            count_of(LanguageType::kr) > 0 ||
            count_of(LanguageType::latin) > 0 ||
            count_of(LanguageType::ru) > 0));

    // jyutping included? (1 row)
    feat.push_back(count_of(LanguageType::jyut) > 0);

    // romaji included? (1 row)
    feat.push_back(count_of(LanguageType::roma) > 0);

    return feat;
}

LrcLanguageHelper::LanguageClassification LrcLanguageHelper::detect_song_language_classification(
    const std::vector<LrcLanguageHelper::LanguageType>& lyric_lang_type)
{
    auto song_feat = extract_song_features(lyric_lang_type);
    int reasoning_result;
    {
        std::lock_guard lock(dlib_mutex);
        reasoning_result = predict_locked(song_net_reasoning.get(), song_feat);
    }
    return ClassificationFromModelIndex(reasoning_result);
}

auto LrcLanguageHelper::detect_language_slot(
    const std::vector<std::vector<LanguageType>>& lines) -> std::vector<LanguageType>
{
    // language序列转int，不固定
    std::unordered_map<int, std::vector<LanguageType>> slot_map_table;
    // 桶，记录每个序列的得分
    std::unordered_map<int, int> slot_bucket;
    
    int lang_id = 0;
    for (const auto& line: lines)
    {
        int this_lang_id;
        auto it = std::ranges::find_if(slot_map_table, [line](const std::pair<int, std::vector<LanguageType>>& key)
        {
            return key.second == line;
        });
        if (it == slot_map_table.end())
        {
            this_lang_id = lang_id;
            slot_map_table[lang_id++] = line;
        }
        else
        {
            this_lang_id = it->first;
        }
        slot_bucket[this_lang_id]++;
    }
    auto max_it = std::ranges::max_element(slot_bucket,
        [](const auto& a, const auto& b) {
            return a.second < b.second; // 按 value 比较
        });

    if (max_it == slot_bucket.end())
        return {};

    int best_lang_id = max_it->first;
    const auto& best_slot_type = slot_map_table[best_lang_id];
    std::string best_slot_debug_str = "[ ";
    for (const auto& slot : best_slot_type)
    {
        best_slot_debug_str += LanguageTypeName(slot);
        best_slot_debug_str += ' ';
    }
    best_slot_debug_str += "]";
    NATIVE_TRACE("info: detect slot type = %s", best_slot_debug_str.c_str());
    return best_slot_type;
}

LrcLanguageHelper& LrcLanguageHelper::GetSingleton()
{
    static LrcLanguageHelper helper_instance;
    return helper_instance;
}

void LrcLanguageHelper::InitializeSingleton()
{
    (void)GetSingleton();
}

void LrcLanguageHelper::ShutdownSingleton() noexcept
{
    GetSingleton().release_native_resources();
}

LrcLanguageHelper::LanguageType LrcAbstractNode::get_language_type(int index) const
{
    std::string text;
    if (get_lrc_str_at(index, text) != 0)
        return LrcLanguageHelper::LanguageType::onomatopoeia;

    return LrcLanguageHelper::GetSingleton().detect_line_language_type(text);
}

LrcLanguageHelper::LanguageType LrcMultiNode::get_language_type(int index) const
{
    if (index >= 0 && static_cast<size_t>(index) < lang_types.size())
        return lang_types[index];

    return LrcAbstractNode::get_language_type(index);
}

LrcProgressNode::LrcProgressNode(
    const int t,
    const LrcTextParser::ProgressLine& parsed_line,
    const int controller_line_index)
    : LrcAbstractNode(t),
      controller_line_index(controller_line_index)
{
    nodes.reserve(parsed_line.segments.size());
    for (const auto& segment : parsed_line.segments)
    {
        nodes.push_back({
            .time_ms = segment.end_time_ms,
            .node_text = segment.text,
        });
    }
}

int LrcProgressNode::get_intrinsic_end_time_ms() const
{
    if (explicit_end_time_ms > 0)
        return explicit_end_time_ms;

    if (!nodes.empty())
        return nodes.back().time_ms;

    return time_ms;
}

int LrcProgressNode::get_controller_node_count(int line_index) const
{
    if (line_index != controller_line_index)
        return 0;

    int count = 0;
    for (const auto& node : nodes)
    {
        if (!node.node_text.empty())
            ++count;
    }
    return count;
}

int LrcProgressNode::get_controller_node_at(
    int line_index,
    int node_index,
    int& start_time_ms,
    int& end_time_ms,
    std::string& out_str) const
{
    if (line_index != controller_line_index || node_index < 0)
        return -1;

    int visible_index = 0;
    for (int i = 0; i < static_cast<int>(nodes.size()); ++i)
    {
        if (nodes[i].node_text.empty())
            continue;

        if (visible_index == node_index)
        {
            start_time_ms = i == 0 ? time_ms : nodes[i - 1].time_ms;
            end_time_ms = nodes[i].time_ms;
            out_str = nodes[i].node_text;
            return 0;
        }
        ++visible_index;
    }

    return -1;
}

LrcProgressMultiNode::LrcProgressMultiNode(
    const int t,
    LrcTextParser::ProgressLines parsed_lines,
    const LrcLanguageHelper::LanguageClassification classification,
    std::vector<LrcLanguageHelper::LanguageType> recommend_slot)
    :
    LrcAbstractNode(t),
    LrcProgressNode(
        t,
        *parsed_lines.controller_line,
        static_cast<int>(parsed_lines.controller_line_index)),
    LrcMultiNode(
        t,
        std::move(parsed_lines.plain_texts),
        classification,
        std::move(recommend_slot))
{
}

LrcFileController::LrcFileController(int song_end_time_ms)
    : song_end_time_ms(song_end_time_ms)
{
}

std::unique_ptr<LrcAbstractNode> LrcFileController::CreateLrcNode(
    const int time_ms,
    std::vector<std::string> lrc_texts,
    LrcLanguageHelper::LanguageClassification classification,
    std::vector<LrcLanguageHelper::LanguageType> recommend_slot)
{
    auto parsed_lines = LrcTextParser::ParseProgressLines(lrc_texts);
    if (lrc_texts.size() == 1)
    {
        if (parsed_lines.controller_line)
        {
            return std::unique_ptr<LrcAbstractNode>(new LrcProgressNode(
                time_ms, *parsed_lines.controller_line,
                static_cast<int>(parsed_lines.controller_line_index)));
        }
        return std::make_unique<LrcNode>(time_ms, lrc_texts[0]);
    }

    if (lrc_texts.size() > 1)
    {
        if (parsed_lines.controller_line)
        {
            return std::unique_ptr<LrcAbstractNode>(new LrcProgressMultiNode(
                time_ms,
                std::move(parsed_lines),
                classification,
                std::move(recommend_slot)));
        }
        return std::unique_ptr<LrcAbstractNode>(new LrcMultiNode(
            time_ms, std::move(lrc_texts), classification,
            std::move(recommend_slot)));
    }
    return {};
}

void LrcFileController::parse_lrc_file_stream(IFile* file_stream)
{
    // 当前支持的格式：
    // 逐行LRC，逐字LRC，Extended LRC，交错翻译，同步翻译
    if (file_stream == nullptr)
    {
        return;
    }
    clear_lrc_nodes();
    metadata = {};
    lrc_offset_ms = 0;
	const auto file_content_bytes = ReadToEnd(*file_stream);
    const std::string file_content_utf8 =
		LocaleConverter::GetUtf8StringFromBytes(
			reinterpret_cast<const char*>(file_content_bytes.data()),
			file_content_bytes.size());

    // fix issue #12
    // 关于歌词文件/歌曲内嵌歌词内出现时间tag非强制有序的翻译歌词时程序的错误/闪退问题
    // struct definition: caching each line for strong_ordering sort
    struct CachedTimeLine
    {
        int time_stamp_ms;
        std::string text;
    };
    std::vector<CachedTimeLine> time_lines;
    
    // 逐行解析
    size_t start = 0;
    int flag_decoding_metadata = 1;
    std::stack<std::string> lyrics_in_ms;
    int recorded_ms = 0;

    while (start < file_content_utf8.size())
    {
        size_t end = file_content_utf8.find('\n', start);
        if (end == std::string::npos)
        {
            end = file_content_utf8.size();
            // 因为现在缓存所有歌词行，所以不需要设置is_lrc_end flag
        }
        std::string line = LrcTextParser::TrimAsciiWhitespace(
            std::string_view(file_content_utf8).substr(start, end - start));
        if (line.empty())
        {
            start = end + 1;
            continue;
        }
        if (line[0] == '{')
        {
            NATIVE_TRACE("warn: invalid ncm extension found, ignoring\n");
            start = end + 1;
            continue;
        }
        const size_t line_start_index = line.find('[');
        // 剔除行开头的不合法字符
        if (line_start_index != std::string::npos && line_start_index != 0)
        {
            NATIVE_TRACE("warn: invalid lrc format, ignoring start character: %s\n",
                line.substr(0, line_start_index).c_str());
            line.erase(0, line_start_index);
        }

        if (flag_decoding_metadata)
        {
            if (DecodeMetadataTag(line, metadata, lrc_offset_ms))
            {
                start = end + 1;
            }
            else {
                flag_decoding_metadata = false;
            }
            continue;
        }
        // 解析时间tag
        if (line.size() < 7) // 现在能解析的最小tag为7位(如：[11:45]
        {
            clear_lrc_nodes();
			throw std::runtime_error("Invalid lrc line, aborting!");
        }

        std::string lyric_text = line;
        // 处理同一行多个时间戳的问题
        std::vector<int> time_stamps;
        while (!lyric_text.empty() && lyric_text[0] == '[')
        {
            const size_t time_tag_end_index_multi = lyric_text.find(']');
            if (time_tag_end_index_multi == std::string::npos)
				throw std::runtime_error("Invalid lrc time tag, aborting!");
            const std::string_view time_tag(lyric_text.data(), time_tag_end_index_multi + 1);
            const auto time_stamp = LrcTextParser::TryParseTimestamp(
                time_tag.substr(1, time_tag.size() - 2));
            if (!time_stamp)
            {
                // malformed time tag
                // guess: metadata tag?
                // fix issue #12
                auto metadata_substr = lyric_text.substr(0, time_tag_end_index_multi + 1);
                (void)DecodeMetadataTag(
                    metadata_substr, metadata, lrc_offset_ms);
                lyric_text = LrcTextParser::TrimAsciiWhitespace(
                    std::string_view(lyric_text).substr(time_tag_end_index_multi + 1));
                continue;
            }
            time_stamps.push_back(*time_stamp);
            lyric_text = LrcTextParser::TrimAsciiWhitespace(
                std::string_view(lyric_text).substr(time_tag_end_index_multi + 1));
        }
        if (time_stamps.empty())
			throw std::runtime_error("Invalid lrc time tag, aborting!");
        if (lyric_text.empty()) {
            // move to next line
            start = end + 1;
            continue;
        }
        for (int time_stamp : time_stamps) 
            time_lines.push_back({ time_stamp, lyric_text });

        start = end + 1;
    }

    // stable sort lrc lines
    // 使用快排会打乱时间戳原始数据
    std::ranges::stable_sort(time_lines,
                             [](const CachedTimeLine& a, const CachedTimeLine& b)
                             {
                                 return a.time_stamp_ms < b.time_stamp_ms;
                             });
    int slash_inline_translation_count = 0;
    for (const auto& line : time_lines)
    {
        if (LrcTextParser::TryParseSlashInlineTranslation(line.text))
            ++slash_inline_translation_count;
    }
    const bool slash_translation_enabled =
        !time_lines.empty()
        && static_cast<double>(slash_inline_translation_count) / static_cast<double>(time_lines.size())
            > InlineSlashTranslationRatioThreshold;
    std::vector<CachedTimeLine> expanded_time_lines;
    expanded_time_lines.reserve(time_lines.size() + slash_inline_translation_count);
    bool has_inline_translation = false;
    for (const auto& line : time_lines)
    {
        const auto split_texts = LrcTextParser::SplitInlineTranslationText(
            line.text,
            slash_translation_enabled);
        if (split_texts.size() > 1)
            has_inline_translation = true;

        for (const auto& split_text : split_texts)
            expanded_time_lines.push_back({ line.time_stamp_ms, split_text });
    }
    if (has_inline_translation)
        time_lines = std::move(expanded_time_lines);

    std::vector<std::vector<LrcLanguageHelper::LanguageType>> lang_types_node_seq;
    std::vector<LrcLanguageHelper::LanguageType> lang_types;
    if (time_lines.empty())
		throw std::runtime_error("Empty LRC file or no data found, aborting!");
    int time_stamp_ms_cur = time_lines[0].time_stamp_ms;
    auto& detector_instance = LrcLanguageHelper::GetSingleton();
    // 清洗控制点
    for (const CachedTimeLine& line : time_lines)
    {
        if (time_stamp_ms_cur != line.time_stamp_ms)
        {
            lang_types_node_seq.push_back(lang_types);
            lang_types.clear();
            time_stamp_ms_cur = line.time_stamp_ms;
        }
        lang_types.push_back(detector_instance.detect_line_language_type(line.text));
    }
    lang_types_node_seq.push_back(lang_types);
    lang_types.clear();
    for (const auto& multi_line_tag : lang_types_node_seq)
        for (const auto& single_line_tag : multi_line_tag)
            lang_types.push_back(single_line_tag);
    auto classification = detector_instance.detect_song_language_classification(lang_types);
    romanization_schema = RomanizationSchemaFromClassification(classification);
    NATIVE_TRACE("info: detected classification = %d\n", classification);
    auto slot_type = detector_instance.detect_language_slot(lang_types_node_seq);
    
    auto pump_stack = [&]()
    {
        std::vector<std::string> lrc_texts;
        while (!lyrics_in_ms.empty())
        {
            lrc_texts.push_back(lyrics_in_ms.top());
            lyrics_in_ms.pop();
        }
        if (lrc_texts.size() > 1)
            std::reverse(lrc_texts.begin(), lrc_texts.end());
        if (lrc_texts.empty())
            return;
        if (auto node = CreateLrcNode(
            recorded_ms, std::move(lrc_texts), classification, slot_type))
        {
            if (!lrc_nodes.empty() && !lrc_nodes.back()->is_progress_node())
            {
                lrc_nodes[lrc_nodes.size() - 1]->set_lrc_end_timestamp(recorded_ms);
            }
            lrc_nodes.push_back(std::move(node));
        }
        else
        {
            // AfxMessageBox(_T("err: create lrc node failed, aborting!"), MB_ICONERROR);
			throw std::runtime_error("Create lrc node failed, aborting!");
        }
    };

    for (size_t i = 0; i < time_lines.size(); ++i)
    {
        int total_ms = time_lines[i].time_stamp_ms;

        if (total_ms != recorded_ms)
        {
            // 新的时间戳，先处理之前的歌词
            if (!lyrics_in_ms.empty())
                pump_stack();
            recorded_ms = total_ms;
        }
        lyrics_in_ms.push(time_lines[i].text);
    }
    // 处理最后一组
    if (!lyrics_in_ms.empty())
        pump_stack();

    if (!lrc_nodes.empty()
        && !lrc_nodes.back()->is_progress_node()
        && song_end_time_ms > lrc_nodes.back()->get_time_ms())
    {
        lrc_nodes.back()->set_lrc_end_timestamp(song_end_time_ms);
    }
}

void LrcFileController::clear_lrc_nodes()
{
    lrc_nodes.clear();
}

std::string LrcFileController::to_intermediate_json(bool pretty) const
{
    rapidjson::StringBuffer buffer;
    auto write_json = [&](auto& writer)
    {
        writer.StartObject();
        writer.Key("format_version");
        writer.Int(2);
        if (!romanization_schema.empty())
        {
            writer.Key("romanization_schema");
            writer.String(romanization_schema.c_str());
        }
        writer.Key("offset");
        writer.Int(lrc_offset_ms);
        writer.Key("metadata");
        writer.StartObject();
        for (const auto& descriptor : MetadataDescriptors)
        {
            WriteMetadataField(
                writer, descriptor.json_key, metadata.*(descriptor.member));
        }
        writer.EndObject();
        writer.Key("lyric_lines");
        writer.StartArray();
        for (int i = 0; i < static_cast<int>(lrc_nodes.size()); ++i)
        {
            const auto* node = lrc_nodes[i].get();
            const int start_ms = node->get_time_ms();
            const int intrinsic_end_ms = node->get_intrinsic_end_time_ms();
            int end_ms = start_ms;
            if (i + 1 < static_cast<int>(lrc_nodes.size()))
            {
                const int next_start_ms = lrc_nodes[i + 1]->get_time_ms();
                end_ms = next_start_ms;
                if (node->is_progress_node()
                    && intrinsic_end_ms > start_ms
                    && intrinsic_end_ms < next_start_ms)
                {
                    end_ms = intrinsic_end_ms;
                }
            }
            else
            {
                end_ms = std::max(start_ms, intrinsic_end_ms);
            }
            if (end_ms <= start_ms)
            {
                end_ms = start_ms + 1;
            }

            writer.StartObject();
            writer.Key("time_start_ms");
            writer.Int(start_ms);
            writer.Key("time_end_ms");
            writer.Int(end_ms);
            writer.Key("lines");
            writer.StartArray();

            const int line_count = node->get_lrc_str_count();
            for (int line_index = 0; line_index < line_count; ++line_index)
            {
                std::string text;
                node->get_lrc_str_at(line_index, text);
                const auto aux_info = node->get_auxiliary_info(line_index);
                const auto language = node->get_language_type(line_index);
                const auto language_str = LrcLanguageHelper::LanguageTypeName(language);
                const int controller_node_count = node->get_controller_node_count(line_index);

                writer.StartObject();
                writer.Key("role");
                writer.String(AuxInfoToRole(aux_info));
                writer.Key("language");
                writer.String(
                    language_str.data(),
                    static_cast<rapidjson::SizeType>(language_str.size()));
                if (controller_node_count > 0)
                {
                    writer.Key("sync");
                    writer.String("controller_nodes");
                    writer.Key("controller_nodes");
                    writer.StartArray();
                    for (int node_index = 0; node_index < controller_node_count; ++node_index)
                    {
                        int controller_start_ms = 0;
                        int controller_end_ms = 0;
                        std::string controller_text;
                        node->get_controller_node_at(
                            line_index,
                            node_index,
                            controller_start_ms,
                            controller_end_ms,
                            controller_text);

                        writer.StartObject();
                        writer.Key("time_start_ms");
                        writer.Int(controller_start_ms);
                        writer.Key("time_end_ms");
                        writer.Int(controller_end_ms);
                        writer.Key("text");
                        WriteUtf8String(writer, controller_text);
                        writer.EndObject();
                    }

                    writer.EndArray();
                }
                else
                {
                    writer.Key("text");
                    WriteUtf8String(writer, text);
                }

                writer.EndObject();
            }

            writer.EndArray();
            writer.EndObject();
        }

        writer.EndArray();
        writer.EndObject();
    };

    if (pretty)
    {
        rapidjson::PrettyWriter writer(buffer);
        write_json(writer);
    }
    else
    {
        rapidjson::Writer writer(buffer);
        write_json(writer);
    }

    return { buffer.GetString(), buffer.GetSize() };
}

