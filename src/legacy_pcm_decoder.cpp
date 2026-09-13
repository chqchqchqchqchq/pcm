#include "legacy_pcm_decoder.h"

#include <cmath>
#include <limits>

namespace {
/** @brief 设置可选错误输出。 */
bool fail(QString *error, const QString &message)
{
    if (error != nullptr) {
        *error = message;
    }
    return false;
}

/** @brief 查找旧版固定时间参数名，不受用户参数组过滤。 */
const StreamLayoutEntry *timeEntry(const StreamLayoutTable &layout, const QString &name)
{
    for (const StreamLayoutEntry &entry : layout.entries) {
        if (entry.parameter_name.compare(name + QStringLiteral("_%1").arg(layout.stream_index),
                                         Qt::CaseInsensitive) == 0 ||
            entry.parameter_name.compare(name, Qt::CaseInsensitive) == 0) {
            return &entry;
        }
    }
    return nullptr;
}

/** @brief 选择需要输出的参数。 */
bool selected(const StreamLayoutEntry &entry, const QSet<QString> &ids)
{
    return ids.isEmpty() || ids.contains(entry.parameter_id);
}

/** @brief 旧版每长周的最大出现次数，包括四个时间参数。 */
int maximumOccurrences(const StreamLayoutTable &layout, const QSet<QString> &ids)
{
    int maximum = 1;
    for (const StreamLayoutEntry &entry : layout.entries) {
        if (selected(entry, ids)) {
            maximum = qMax(maximum, static_cast<int>(entry.rate_hz));
        }
    }
    for (const QString &name : {QStringLiteral("HOUR"), QStringLiteral("MINUTE"),
                               QStringLiteral("SECOND"), QStringLiteral("MSSECOND")}) {
        const StreamLayoutEntry *entry = timeEntry(layout, name);
        if (entry != nullptr) {
            maximum = qMax(maximum, static_cast<int>(entry->rate_hz));
        }
    }
    return maximum;
}

/** @brief 在长周内定位参数第 occurrence 次出现，复现旧版正常/超采地址矩阵。 */
qint64 occurrenceAddress(const StreamLayoutTable &layout, const StreamLayoutEntry &entry,
                        const ParameterWordSegment &segment, int occurrence)
{
    const int rate = static_cast<int>(entry.rate_hz);
    const qint64 frame_words = layout.frame_length_words;
    if (rate <= layout.subframe_depth) {
        return segment.frame_no * frame_words + segment.word_no +
            occurrence * (frame_words * layout.subframe_depth / rate);
    }
    const int per_subframe = rate / layout.subframe_depth;
    return (occurrence / per_subframe) * frame_words + segment.word_no +
        (occurrence % per_subframe) * (frame_words / per_subframe);
}

/** @brief 时间字按旧版读取原始 16 位字，不应用参数校准或 inverse。 */
bool readTimeWord(const FrameTask &frame, qint64 address, quint16 *word, QString *error)
{
    if (address < 0 || address >= frame.frame_data.size() / 2) {
        return fail(error, QStringLiteral("时间字地址超出长周"));
    }
    const uchar *bytes = reinterpret_cast<const uchar *>(frame.frame_data.constData()) + address * 2;
    *word = static_cast<quint16>(bytes[0] | (quint16(bytes[1]) << 8));
    return true;
}
}

bool LegacyPcmDecoder::validate(const StreamLayoutTable &layout, double output_rate,
                               const QSet<QString> &selected_ids, QString *error_message)
{
    if (layout.word_length_bits != 16 || layout.frame_length_words <= 0 || layout.subframe_depth <= 0) {
        return fail(error_message, QStringLiteral("旧版解算要求 16 位 PCM 存储字和有效长周布局"));
    }
    const qint64 byte_count = qint64(layout.frame_length_words) * layout.subframe_depth * 2;
    if (byte_count > 256 * 1024 * 1024 ||
        (layout.subframe_depth > 1 && layout.id_position_word < 0) ||
        layout.id_position_word >= layout.frame_length_words || layout.id_lsb < 0 || layout.id_lsb > 15 ||
        (layout.id_lsb >= 0 && layout.id_lsb <= 15 &&
         layout.subframe_depth - 1 > (0xffff >> layout.id_lsb))) {
        return fail(error_message, QStringLiteral("长周大小或子帧 ID 布局无效"));
    }
    if (!std::isfinite(output_rate) || output_rate <= 0 || output_rate > 1e9 ||
        !std::isfinite(layout.major_frame_rate_hz) || layout.major_frame_rate_hz < 1 ||
        layout.major_frame_rate_hz > 1e9 || std::floor(output_rate) != output_rate ||
        std::floor(layout.major_frame_rate_hz) != layout.major_frame_rate_hz) {
        return fail(error_message, QStringLiteral("旧版用户采样率和 major_frame_rate 必须为正整数"));
    }
    for (const QString &name : {QStringLiteral("HOUR"), QStringLiteral("MINUTE"), QStringLiteral("SECOND")}) {
        if (timeEntry(layout, name) == nullptr) {
            return fail(error_message, QStringLiteral("流 %1 缺少时间参数 %2_%1")
                .arg(layout.stream_index).arg(name));
        }
    }
    const qint64 words = qint64(layout.frame_length_words) * layout.subframe_depth;
    for (const StreamLayoutEntry &entry : layout.entries) {
        if (!std::isfinite(entry.rate_hz) || entry.rate_hz < 1 || entry.rate_hz > words ||
            entry.rate_hz > std::numeric_limits<int>::max() || std::floor(entry.rate_hz) != entry.rate_hz) {
            return fail(error_message, QStringLiteral("参数 %1 的 rate 必须为长周内有效出现次数").arg(entry.parameter_name));
        }
        if (entry.word_segments.isEmpty()) {
            return fail(error_message, QStringLiteral("参数 %1 缺少拼字信息").arg(entry.parameter_name));
        }
        const int rate = static_cast<int>(entry.rate_hz);
        if (rate > layout.subframe_depth && rate % layout.subframe_depth != 0) {
            return fail(error_message, QStringLiteral("参数 %1 的超采次数必须整除子帧深度").arg(entry.parameter_name));
        }
        for (const ParameterWordSegment &segment : entry.word_segments) {
            if (segment.frame_no < 0 || segment.frame_no >= layout.subframe_depth ||
                segment.word_no < 0 || segment.word_no >= layout.frame_length_words ||
                segment.bit_length <= 0 || segment.bit_length > 16 || segment.source_bit < 0 ||
                segment.source_bit > 16 - segment.bit_length || segment.object_bit < 0 ||
                segment.object_bit > 64 - segment.bit_length ||
                occurrenceAddress(layout, entry, segment, rate - 1) >= words) {
                return fail(error_message, QStringLiteral("参数 %1 最后一次采样地址越界").arg(entry.parameter_name));
            }
        }
    }
    const int maximum = maximumOccurrences(layout, selected_ids);
    for (const StreamLayoutEntry &entry : layout.entries) {
        if (selected(entry, selected_ids) && maximum % static_cast<int>(entry.rate_hz) != 0) {
            return fail(error_message, QStringLiteral("最大出现次数不能整除参数 %1 的 rate").arg(entry.parameter_name));
        }
    }
    return true;
}

bool LegacyPcmDecoder::decodeTimeWords(const QString &equipment, const QList<quint16> &words,
                                      double *seconds, QString *error_message)
{
    if (seconds == nullptr || words.size() < 3) {
        return fail(error_message, QStringLiteral("时间解码需要至少三个时间字"));
    }
    const quint64 a = words.at(0), b = words.at(1), c = words.at(2);
    const QString type = equipment.trimmed().toLower();
    int hour = 0, minute = 0, second = 0, millisecond = 0;
    if (type == QStringLiteral("xfc100") || type == QStringLiteral("xma")) {
        const quint64 milliseconds = ((a << 32) | (b << 16) | c) / 1000;
        *seconds = double(milliseconds % 86400000) / 1000.0;
        return true;
    }
    if (type == QStringLiteral("uma2000")) {
        const quint64 milliseconds = ((a << 16) + b) * 10 + c / 1000;
        *seconds = double(milliseconds % 86400000) / 1000.0;
        return true;
    }
    if (type == QStringLiteral("ttc")) {
        *seconds = double((a << 16) | b) * 0.01;
        return true;
    }
    if (type == QStringLiteral("秦川")) {
        hour = ((a & 0x180) >> 7) * 10 + ((a & 0x78) >> 3);
        minute = (a & 7) * 10 + ((b & 0x780) >> 7);
        second = ((b & 0x70) >> 4) * 10 + (b & 15);
    } else if (type == QStringLiteral("kam500")) {
        hour = ((a & 0x1800) >> 11) * 10 + ((a & 0x780) >> 7);
        minute = ((a & 0x70) >> 4) * 10 + (a & 15);
        second = ((b & 0xf000) >> 12) * 10 + ((b & 0xf00) >> 8);
        millisecond = ((b & 0xf0) >> 4) * 100 + (b & 15) * 10 + ((c & 0xf000) >> 12);
    } else if (type == QStringLiteral("770")) {
        hour = ((a & 0x30) >> 4) * 10 + (a & 15);
        minute = ((b & 0xe000) >> 13) * 10 + ((b & 0x1e00) >> 9);
        second = ((b & 0x1c0) >> 6) * 10 + ((b & 0x3c) >> 2);
        millisecond = ((b & 3) * 4 + ((c & 0xc000) >> 14)) * 100 +
            ((c & 0x3c00) >> 10) * 10 + ((c & 0x3c0) >> 6);
    } else if (type == QStringLiteral("jqd")) {
        hour = ((a & 0x3000) >> 12) * 10 + ((a & 0xf00) >> 8);
        minute = ((a & 0x70) >> 4) * 10 + (a & 15);
        second = ((b & 0xe000) >> 13) * 10 + ((b & 0x1e00) >> 9);
        millisecond = (c & 0xffc0) >> 6;
    } else if (type == QStringLiteral("xyd")) {
        minute = b & 0x3f;
        hour = (b & 0x1f80) >> 7;
        second = (c & 0xff00) >> 8;
        millisecond = c & 0xff;
    } else {
        // 旧版未识别的设备名使用 DM6 普通时间字。
        hour = a & 0xfff;
        minute = b & 0xfff;
        second = c & 0xfff;
        millisecond = words.size() >= 4 ? words.at(3) & 0xfff : 0;
    }
    if (hour > 23 || minute > 59 || second > 59 || millisecond > 999) {
        return fail(error_message, QStringLiteral("时间码越界：%1:%2:%3.%4")
            .arg(hour).arg(minute).arg(second).arg(millisecond));
    }
    *seconds = hour * 3600.0 + minute * 60.0 + second + millisecond / 1000.0;
    return true;
}

bool LegacyPcmDecoder::decodeFrameTime(const FrameTask &frame, const StreamLayoutTable &layout,
                                      double *seconds, QString *error_message)
{
    QList<quint16> time_words;
    qint64 first_time_address = 0;
    int index = 0;
    for (const QString &name : {QStringLiteral("HOUR"), QStringLiteral("MINUTE"),
                               QStringLiteral("SECOND"), QStringLiteral("MSSECOND")}) {
        quint16 word = 0;
        const StreamLayoutEntry *entry = timeEntry(layout, name);
        if (entry != nullptr) {
            if (entry->word_segments.isEmpty() || entry->rate_hz < 1) {
                return fail(error_message, QStringLiteral("时间参数布局无效"));
            }
            const ParameterWordSegment &segment = entry->word_segments.first();
            qint64 address = occurrenceAddress(layout, *entry, segment, 0);
            if (index == 0) {
                first_time_address = address;
            }
            if (layout.time_code_type.compare(QStringLiteral("uma2000"), Qt::CaseInsensitive) == 0 && index < 3) {
                address = first_time_address + index;
            }
            if (!readTimeWord(frame, address, &word, error_message)) {
                return false;
            }
        }
        if (entry == nullptr && index < 3) {
            return fail(error_message, QStringLiteral("缺少时间参数：%1").arg(name));
        }
        time_words.append(word);
        ++index;
    }
    return decodeTimeWords(layout.time_code_type, time_words, seconds, error_message);
}

bool LegacyPcmDecoder::decode(const FrameTask &frame, const StreamLayoutTable &layout,
                             const QSet<QString> &selected_ids, QList<ParameterValue> *values,
                             QString *error_message)
{
    if (values == nullptr || !validate(layout, frame.output_sample_rate_hz, selected_ids, error_message)) {
        return false;
    }
    values->clear();
    const int fps = static_cast<int>(layout.major_frame_rate_hz);
    const int requested = static_cast<int>(frame.output_sample_rate_hz);
    const int maximum = maximumOccurrences(layout, selected_ids);
    const int frame_stride = requested <= fps ? fps / requested : 1;
    if (frame.frame_index % static_cast<quint64>(frame_stride) != 0) {
        return true;
    }
    double base_time = 0;
    if (!decodeFrameTime(frame, layout, &base_time, error_message)) {
        return false;
    }
    if (base_time < frame.range_start_seconds) {
        return true;
    }
    const int count = requested <= fps ? 1 : maximum;
    const int step = requested <= fps ? 1 : qMax(1, int(double(maximum) / (requested / fps) + 0.5));
    for (int slot = 0; slot < count; slot += step) {
        const double timestamp = base_time + double(slot) / (fps * double(maximum));
        if (timestamp < frame.range_start_seconds ||
            (frame.range_end_seconds >= 0 && timestamp >= frame.range_end_seconds)) {
            continue;
        }
        for (const StreamLayoutEntry &entry : layout.entries) {
            if (!selected(entry, selected_ids)) {
                continue;
            }
            const int rate = static_cast<int>(entry.rate_hz);
            const int occurrence = slot / (maximum / rate);
            StreamLayoutEntry shifted = entry;
            for (ParameterWordSegment &segment : shifted.word_segments) {
                const qint64 address = occurrenceAddress(layout, entry, segment, occurrence);
                segment.frame_no = static_cast<int>(address / layout.frame_length_words);
                segment.word_no = static_cast<int>(address % layout.frame_length_words);
            }
            quint64 bits = 0;
            int width = 0;
            if (!FrameProcessor::extractRawBits(frame.frame_data, layout, shifted, &bits, &width, error_message)) {
                return false;
            }
            ParameterValue value;
            value.task_id = frame.task_id;
            value.stream_index = layout.stream_index;
            value.parameter_id = entry.parameter_id;
            value.parameter_name = entry.parameter_name;
            value.unit = entry.unit;
            value.timestamp_seconds = timestamp;
            if (!FrameProcessor::convertRawValue(bits, width, shifted, &value.value, error_message)) {
                return false;
            }
            shifted.calibration.type = CalibrationType::None;
            if (!FrameProcessor::convertRawValue(bits, width, shifted, &value.raw_value, error_message)) {
                return false;
            }
            values->append(value);
        }
    }
    return true;
}
