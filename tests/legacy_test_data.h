#pragma once
#include "config_file_document.h"
#include "config_parser.h"
#include "config_calibration_parser.h"
#include <QFile>

/** @brief 手工定义旧版 CFTE 夹具，不从生产解码器反推期望结果。 */
namespace LegacyTestData {
/** @brief 返回一个旧版参数段。rate 是每长周出现次数。 */
inline QString parameter(const QString &name, int serial, int word, int rate = 1,
                         const QString &calibration = QStringLiteral("NO"),
                         const QString &extra = QString())
{
    return QStringLiteral(" serial_no.: %1\n id_no.: %1\n name: %2\n unit: V\n cal_type: %3\n"
                          " code_type: uint\n cal_date: 2026-02-26\n cal_number: \n rate: %4\n"
                          " word_interval: 0\n sign_bit: 0\n inverse: 0\n pcm_words: 1\n"
                          " frame_no: 0,0,0,0,0,0\n word_no: %5,0,0,0,0,0\n"
                          " source_bit: 0,0,0,0,0,0\n bit_length: 16,0,0,0,0,0\n"
                          " object_bit: 0,0,0,0,0,0\n%6 Description: fixture\n")
        .arg(serial).arg(name, calibration).arg(rate).arg(word).arg(extra);
}
/** @brief 一路 16 位字、8 字长周，时间位于第 1..4 字，TEMP 位于第 5 字。 */
inline QString config(int stream_count = 1, int fps = 1, const QString &calibration = QStringLiteral("NO"),
                      const QString &extra = QString())
{
    QString text = QStringLiteral(" #CFTE/filemain/rec01.00\n Header_informations:\n"
                                  " Create_date: 2026.02.26 12:00:00\n test_id: legacy\n"
                                  " stream_number: %1\n derpar_number: 0\n").arg(stream_count);
    for (int stream = 1; stream <= stream_count; ++stream) {
        text += QStringLiteral(" The format description of stream %1:\n aircraft_name: demo\n"
                               " test_name: old\n parameter_number: 5\n bit_rate: 128\n"
                               " pcm_code: NRZ-L\n loop_width: 0.1\n word_length: 16\n parity: NONE\n"
                               " Onboard_acq._equipments_type: DM6\n frame_length: 8\n subframe_deepth: 1\n"
                               " major_frame_rate: %2\n sync_word_length: 16\n sync_pattern: 0xAA55\n"
                               " dating_type: counter\n time_interpolation: 0\n").arg(stream).arg(fps);
    }
    for (int stream = 1; stream <= stream_count; ++stream) {
        text += QStringLiteral(" the_parameter_description_of_stream %1:\n").arg(stream);
        const QString suffix = QStringLiteral("_%1").arg(stream);
        text += parameter("HOUR" + suffix, 1, 1);
        text += parameter("MINUTE" + suffix, 2, 2);
        text += parameter("SECOND" + suffix, 3, 3);
        text += parameter("MSSECOND" + suffix, 4, 4);
        text += parameter(stream_count == 1 ? "TEMP" : "TEMP" + suffix, 5, 5, 1, calibration, extra);
    }
    return text;
}
/** @brief 按明确字值构建小端长周。 */
inline QByteArray frame(int second, int value, int hour = 0, int minute = 0, int ms = 0)
{
    QByteArray bytes;
    for (quint16 word : {quint16(0xaa55), quint16(hour), quint16(minute), quint16(second),
                         quint16(ms), quint16(value), quint16(0), quint16(0)}) {
        bytes.append(char(word & 255));
        bytes.append(char(word >> 8));
    }
    return bytes;
}
/** @brief 写入夹具并检查实际写入字节数。 */
inline bool write(const QString &path, const QByteArray &data)
{
    QFile file(path);
    return file.open(QIODevice::WriteOnly) && file.write(data) == data.size();
}
/** @brief 手动构造用于旧采样算法测试的已解析布局。 */
inline StreamLayoutTable layout(int fps = 1)
{
    StreamLayoutTable table;
    table.legacy_semantics = true;
    table.stream_index = 1;
    table.stream_id = "stream_1";
    table.word_length_bits = 16;
    table.frame_length_words = 8;
    table.subframe_depth = 1;
    table.major_frame_rate_hz = fps;
    table.time_code_type = "DM6";
    const QStringList names = {"HOUR_1", "MINUTE_1", "SECOND_1", "MSSECOND_1", "TEMP"};
    for (int index = 0; index < names.size(); ++index) {
        StreamLayoutEntry entry;
        entry.parameter_id = QString::number(index + 1);
        entry.parameter_name = names.at(index);
        entry.code_type = ParameterCodeType::UnsignedInteger;
        entry.rate_hz = 1;
        ParameterWordSegment segment;
        segment.word_no = index + 1;
        segment.bit_length = 16;
        entry.word_segments.append(segment);
        table.entries.append(entry);
    }
    table.rebuildIndexes();
    return table;
}
}
