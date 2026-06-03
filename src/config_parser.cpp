#include "config_parser.h"

#include <QPair>
#include <QSet>

#include <limits>

namespace {

void setError(QString *error_message, const QString &message)
{
    if (error_message != nullptr) {
        *error_message = message;
    }
}

QString parameterLabel(const ParameterDescription &parameter)
{
    if (!parameter.id_no.trimmed().isEmpty() && !parameter.name.trimmed().isEmpty()) {
        return QStringLiteral("%1/%2").arg(parameter.id_no.trimmed(), parameter.name.trimmed());
    }
    if (!parameter.id_no.trimmed().isEmpty()) {
        return parameter.id_no.trimmed();
    }
    if (!parameter.name.trimmed().isEmpty()) {
        return parameter.name.trimmed();
    }

    return QStringLiteral("#%1").arg(parameter.serial_no);
}

bool isZeroOrOneBasedInRange(int value, int count)
{
    return (value >= 0 && value < count) || (value >= 1 && value <= count);
}

bool validateStreamGeometry(const StreamFormatDescription &stream, QString *error_message)
{
    if (stream.word_length_bits <= 0) {
        setError(error_message,
                 QStringLiteral("Stream %1 word_length_bits must be > 0").arg(stream.stream_index));
        return false;
    }

    if (stream.frame_length_words <= 0) {
        setError(error_message,
                 QStringLiteral("Stream %1 frame_length_words must be > 0").arg(stream.stream_index));
        return false;
    }

    if (stream.subframe_depth <= 0) {
        setError(error_message,
                 QStringLiteral("Stream %1 subframe_depth must be > 0").arg(stream.stream_index));
        return false;
    }

    return true;
}

bool validateObjectRange(const ParameterDescription &parameter,
                         const ParameterWordSegment &segment,
                         int segment_index,
                         const QList<QPair<int, int>> &object_ranges,
                         QString *error_message)
{
    if (segment.object_bit < 0) {
        setError(error_message,
                 QStringLiteral("Parameter %1 segment %2 object_bit must be >= 0")
                     .arg(parameterLabel(parameter))
                     .arg(segment_index));
        return false;
    }

    if (segment.object_bit > std::numeric_limits<int>::max() - segment.bit_length) {
        setError(error_message,
                 QStringLiteral("Parameter %1 segment %2 object bit range overflows int")
                     .arg(parameterLabel(parameter))
                     .arg(segment_index));
        return false;
    }

    const int range_begin = segment.object_bit;
    const int range_end = segment.object_bit + segment.bit_length;
    for (const QPair<int, int> &existing_range : object_ranges) {
        const bool overlaps = range_begin < existing_range.second && existing_range.first < range_end;
        if (overlaps) {
            setError(error_message,
                     QStringLiteral("Parameter %1 segment %2 overlaps object bits [%3, %4)")
                         .arg(parameterLabel(parameter))
                         .arg(segment_index)
                         .arg(existing_range.first)
                         .arg(existing_range.second));
            return false;
        }
    }

    return true;
}

bool validateSegment(const StreamFormatDescription &stream,
                     const ParameterDescription &parameter,
                     const ParameterWordSegment &segment,
                     int segment_index,
                     const QList<QPair<int, int>> &object_ranges,
                     QString *error_message)
{
    if (!segment.isActive()) {
        setError(error_message,
                 QStringLiteral("Parameter %1 segment %2 must have bit_length > 0")
                     .arg(parameterLabel(parameter))
                     .arg(segment_index));
        return false;
    }

    if (!isZeroOrOneBasedInRange(segment.frame_no, stream.subframe_depth)) {
        setError(error_message,
                 QStringLiteral("Parameter %1 segment %2 frame_no=%3 is outside subframe_depth=%4")
                     .arg(parameterLabel(parameter))
                     .arg(segment_index)
                     .arg(segment.frame_no)
                     .arg(stream.subframe_depth));
        return false;
    }

    if (!isZeroOrOneBasedInRange(segment.word_no, stream.frame_length_words)) {
        setError(error_message,
                 QStringLiteral("Parameter %1 segment %2 word_no=%3 is outside frame_length_words=%4")
                     .arg(parameterLabel(parameter))
                     .arg(segment_index)
                     .arg(segment.word_no)
                     .arg(stream.frame_length_words));
        return false;
    }

    if (segment.source_bit < 0) {
        setError(error_message,
                 QStringLiteral("Parameter %1 segment %2 source_bit must be >= 0")
                     .arg(parameterLabel(parameter))
                     .arg(segment_index));
        return false;
    }

    if (segment.bit_length > stream.word_length_bits) {
        setError(error_message,
                 QStringLiteral("Parameter %1 segment %2 bit_length=%3 exceeds word_length_bits=%4")
                     .arg(parameterLabel(parameter))
                     .arg(segment_index)
                     .arg(segment.bit_length)
                     .arg(stream.word_length_bits));
        return false;
    }

    if (segment.source_bit > stream.word_length_bits - segment.bit_length) {
        setError(error_message,
                 QStringLiteral("Parameter %1 segment %2 source range [%3, %4) exceeds word_length_bits=%5")
                     .arg(parameterLabel(parameter))
                     .arg(segment_index)
                     .arg(segment.source_bit)
                     .arg(segment.source_bit + segment.bit_length)
                     .arg(stream.word_length_bits));
        return false;
    }

    return validateObjectRange(parameter, segment, segment_index, object_ranges, error_message);
}

bool buildLayoutEntry(const StreamFormatDescription &stream,
                      const ParameterDescription &parameter,
                      StreamLayoutEntry *entry,
                      QString *error_message)
{
    if (parameter.pcm_words <= 0) {
        setError(error_message,
                 QStringLiteral("Parameter %1 pcm_words must be > 0").arg(parameterLabel(parameter)));
        return false;
    }

    if (parameter.pcm_words > parameter.word_segments.size()) {
        setError(error_message,
                 QStringLiteral("Parameter %1 pcm_words=%2 exceeds configured word segment count=%3")
                     .arg(parameterLabel(parameter))
                     .arg(parameter.pcm_words)
                     .arg(parameter.word_segments.size()));
        return false;
    }

    StreamLayoutEntry generated_entry;
    generated_entry.parameter_id = parameter.id_no.trimmed();
    generated_entry.parameter_name = parameter.name.trimmed();
    generated_entry.unit = parameter.unit.trimmed();
    generated_entry.serial_no = parameter.serial_no;
    generated_entry.rate_hz = parameter.rate_hz;
    generated_entry.code_type = parameter.code_type;
    generated_entry.sign_bit = parameter.sign_bit;
    generated_entry.inverse = parameter.inverse;
    generated_entry.calibration = parameter.calibration;
    generated_entry.description = parameter.description;
    generated_entry.word_segments.reserve(parameter.pcm_words);

    QList<QPair<int, int>> object_ranges;
    object_ranges.reserve(parameter.pcm_words);
    for (int segment_index = 0; segment_index < parameter.pcm_words; ++segment_index) {
        const ParameterWordSegment segment = parameter.word_segments.at(segment_index);
        if (!validateSegment(stream, parameter, segment, segment_index, object_ranges, error_message)) {
            return false;
        }

        generated_entry.word_segments.append(segment);
        object_ranges.append(qMakePair(segment.object_bit, segment.object_bit + segment.bit_length));
    }

    *entry = generated_entry;
    return true;
}

} // namespace

bool ConfigParser::generateLayoutTable(const StreamFormatDescription &stream,
                                       StreamLayoutTable *layout_table,
                                       QString *error_message)
{
    if (layout_table == nullptr) {
        setError(error_message, QStringLiteral("layout_table output pointer is null"));
        return false;
    }

    QString local_error;
    if (!validateStreamGeometry(stream, &local_error)) {
        setError(error_message, local_error);
        return false;
    }

    StreamLayoutTable generated_table;
    generated_table.stream_index = stream.stream_index;
    generated_table.stream_id = stream.stream_id.trimmed();
    generated_table.word_length_bits = stream.word_length_bits;
    generated_table.frame_length_words = stream.frame_length_words;
    generated_table.subframe_depth = stream.subframe_depth;
    generated_table.entries.reserve(stream.parameters.size());

    QSet<QString> used_parameter_ids;
    QSet<QString> used_parameter_names;
    for (const ParameterDescription &parameter : stream.parameters) {
        StreamLayoutEntry entry;
        if (!buildLayoutEntry(stream, parameter, &entry, &local_error)) {
            setError(error_message,
                     QStringLiteral("Failed to build layout table for stream %1: %2")
                         .arg(stream.stream_index)
                         .arg(local_error));
            return false;
        }

        if (entry.parameter_id.isEmpty()) {
            setError(error_message,
                     QStringLiteral("Stream %1 contains a parameter with an empty id")
                         .arg(stream.stream_index));
            return false;
        }

        if (used_parameter_ids.contains(entry.parameter_id)) {
            setError(error_message,
                     QStringLiteral("Stream %1 contains duplicate parameter id '%2'")
                         .arg(stream.stream_index)
                         .arg(entry.parameter_id));
            return false;
        }

        if (!entry.parameter_name.isEmpty() && used_parameter_names.contains(entry.parameter_name)) {
            setError(error_message,
                     QStringLiteral("Stream %1 contains duplicate parameter name '%2'")
                         .arg(stream.stream_index)
                         .arg(entry.parameter_name));
            return false;
        }

        used_parameter_ids.insert(entry.parameter_id);
        if (!entry.parameter_name.isEmpty()) {
            used_parameter_names.insert(entry.parameter_name);
        }

        generated_table.entries.append(entry);
    }

    generated_table.rebuildIndexes();
    *layout_table = generated_table;
    return true;
}

bool ConfigParser::generateLayoutTables(const QList<StreamFormatDescription> &streams,
                                        QList<StreamLayoutTable> *layout_tables,
                                        QString *error_message)
{
    if (layout_tables == nullptr) {
        setError(error_message, QStringLiteral("layout_tables output pointer is null"));
        return false;
    }

    QList<StreamLayoutTable> generated_tables;
    generated_tables.reserve(streams.size());

    QSet<int> used_stream_indexes;
    for (const StreamFormatDescription &stream : streams) {
        if (used_stream_indexes.contains(stream.stream_index)) {
            setError(error_message,
                     QStringLiteral("Duplicate stream index %1 while generating layout tables")
                         .arg(stream.stream_index));
            return false;
        }

        StreamLayoutTable layout_table;
        if (!generateLayoutTable(stream, &layout_table, error_message)) {
            return false;
        }

        used_stream_indexes.insert(stream.stream_index);
        generated_tables.append(layout_table);
    }

    *layout_tables = generated_tables;
    return true;
}

bool ConfigParser::rebuildLayoutTables(Config *config, QString *error_message)
{
    if (config == nullptr) {
        setError(error_message, QStringLiteral("config pointer is null"));
        return false;
    }

    QList<StreamLayoutTable> generated_tables;
    if (!generateLayoutTables(config->streams, &generated_tables, error_message)) {
        return false;
    }

    config->layout_tables = generated_tables;
    return true;
}
