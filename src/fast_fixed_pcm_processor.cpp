#include "fast_fixed_pcm_processor.h"

#include "logger.h"

#include <QDir>
#include <QElapsedTimer>
#include <QFile>
#include <QFileInfo>
#include <QFuture>
#include <QSaveFile>
#include <QThread>
#include <QtConcurrent>

#include <algorithm>
#include <cmath>
#include <cstring>
#include <limits>

namespace {

constexpr QChar kColumnKeySeparator(0x1f);

void setError(QString *error_message, const QString &message)
{
    if (error_message != nullptr) {
        *error_message = message;
    }
}

QString makeColumnKey(int stream_index, const QString &parameter_id, const QString &parameter_name)
{
    const QString normalized_id = parameter_id.trimmed();
    const QString normalized_name = parameter_name.trimmed();
    const QString parameter_key = normalized_id.isEmpty() ? normalized_name : normalized_id;
    if (parameter_key.isEmpty()) {
        return QString();
    }

    return QString::number(stream_index) + kColumnKeySeparator + parameter_key;
}

QString makeOutputTitle(const QString &parameter_id, const QString &parameter_name)
{
    const QString normalized_name = parameter_name.trimmed();
    if (!normalized_name.isEmpty()) {
        return normalized_name;
    }

    return parameter_id.trimmed();
}

QString safeFileName(QString file_name)
{
    file_name = file_name.trimmed();
    static const QString invalid_chars = QStringLiteral("<>:\"/\\|?*");
    for (int index = 0; index < file_name.size(); ++index) {
        const QChar ch = file_name.at(index);
        if (invalid_chars.contains(ch) || ch.unicode() < 0x20) {
            file_name[index] = QChar('_');
        }
    }

    return file_name.isEmpty() ? QStringLiteral("task") : file_name;
}

bool parameterValueLess(const ParameterValue &left, const ParameterValue &right)
{
    if (left.timestamp_seconds != right.timestamp_seconds) {
        return left.timestamp_seconds < right.timestamp_seconds;
    }
    if (left.stream_index != right.stream_index) {
        return left.stream_index < right.stream_index;
    }
    if (left.parameter_id != right.parameter_id) {
        return left.parameter_id < right.parameter_id;
    }
    return left.parameter_name < right.parameter_name;
}

quint64 bitMask(int bit_count)
{
    if (bit_count <= 0) {
        return 0;
    }
    if (bit_count >= 64) {
        return std::numeric_limits<quint64>::max();
    }

    return (quint64(1) << bit_count) - quint64(1);
}

quint64 readLittleEndianWordValue(const char *data, int byte_count)
{
    quint64 value = 0;
    const int readable_byte_count = qMin(byte_count, static_cast<int>(sizeof(quint64)));
    for (int index = 0; index < readable_byte_count; ++index) {
        value |= static_cast<quint64>(static_cast<uchar>(data[index])) << (index * 8);
    }

    return value;
}

void appendTextRow(QByteArray *data, const QByteArray &timestamp_text, const QList<QByteArray> &row_values)
{
    data->append(timestamp_text);
    for (const QByteArray &cell : row_values) {
        data->append('\t');
        data->append(cell);
    }
    data->append('\n');
}

QString parameterLabel(const StreamLayoutEntry &entry)
{
    if (!entry.parameter_id.trimmed().isEmpty() && !entry.parameter_name.trimmed().isEmpty()) {
        return QStringLiteral("%1/%2").arg(entry.parameter_id.trimmed(), entry.parameter_name.trimmed());
    }
    if (!entry.parameter_id.trimmed().isEmpty()) {
        return entry.parameter_id.trimmed();
    }
    if (!entry.parameter_name.trimmed().isEmpty()) {
        return entry.parameter_name.trimmed();
    }

    return QStringLiteral("#%1").arg(entry.serial_no);
}

bool hasEngineeringCalibration(CalibrationType type)
{
    return type == CalibrationType::Linear
        || type == CalibrationType::Polynomial
        || type == CalibrationType::Segmented;
}

bool normalizeLayoutIndex(int configured_index,
                          int item_count,
                          int *normalized_index,
                          QString *error_message)
{
    if (normalized_index == nullptr) {
        setError(error_message, QStringLiteral("normalized_index output pointer is null"));
        return false;
    }
    if (item_count <= 0) {
        setError(error_message, QStringLiteral("item_count must be > 0"));
        return false;
    }

    if (configured_index >= 0 && configured_index < item_count) {
        *normalized_index = configured_index;
        return true;
    }

    setError(error_message,
             QStringLiteral("configured_index=%1 is outside zero-based range [0,%2]")
                 .arg(configured_index)
                 .arg(item_count - 1));
    return false;
}

bool decodeSignedInteger(quint64 raw_bits,
                         int bit_count,
                         int sign_bit,
                         qlonglong *value,
                         QString *error_message)
{
    if (value == nullptr) {
        setError(error_message, QStringLiteral("value output pointer is null"));
        return false;
    }
    if (bit_count <= 0 || bit_count > 64) {
        setError(error_message, QStringLiteral("Signed integer bit_count must be in [1, 64]"));
        return false;
    }

    int resolved_sign_bit = sign_bit;
    if (resolved_sign_bit <= 0) {
        resolved_sign_bit = bit_count - 1;
    } else if (resolved_sign_bit == bit_count) {
        resolved_sign_bit = bit_count - 1;
    }

    if (resolved_sign_bit < 0 || resolved_sign_bit >= bit_count) {
        setError(
            error_message,
            QStringLiteral("sign_bit=%1 is outside assembled bit_count=%2").arg(sign_bit).arg(bit_count));
        return false;
    }

    const quint64 width_mask = bitMask(resolved_sign_bit + 1);
    const quint64 value_bits = raw_bits & width_mask;
    const quint64 sign_mask = quint64(1) << resolved_sign_bit;
    if ((value_bits & sign_mask) == 0) {
        *value = static_cast<qlonglong>(value_bits);
        return true;
    }

    if (resolved_sign_bit == 63) {
        const quint64 without_sign = value_bits & (~sign_mask);
        *value = std::numeric_limits<qlonglong>::min() + static_cast<qlonglong>(without_sign);
        return true;
    }

    const quint64 magnitude = ((~value_bits) & width_mask) + quint64(1);
    *value = -static_cast<qlonglong>(magnitude);
    return true;
}

bool decodeBcd(quint64 raw_bits, int bit_count, qlonglong *value, QString *error_message)
{
    if (value == nullptr) {
        setError(error_message, QStringLiteral("value output pointer is null"));
        return false;
    }
    if (bit_count <= 0 || bit_count > 64) {
        setError(error_message, QStringLiteral("BCD bit_count must be in [1, 64]"));
        return false;
    }

    const int nibble_count = (bit_count + 3) / 4;
    qlonglong decoded_value = 0;
    for (int nibble_index = nibble_count - 1; nibble_index >= 0; --nibble_index) {
        const int digit = static_cast<int>((raw_bits >> (nibble_index * 4)) & 0x0F);
        if (digit > 9) {
            setError(
                error_message,
                QStringLiteral("Invalid BCD digit %1 in raw value 0x%2")
                    .arg(digit)
                    .arg(QString::number(raw_bits, 16)));
            return false;
        }
        if (decoded_value > (std::numeric_limits<qlonglong>::max() - digit) / 10) {
            setError(error_message, QStringLiteral("BCD value overflows qlonglong"));
            return false;
        }
        decoded_value = (decoded_value * 10) + digit;
    }

    *value = decoded_value;
    return true;
}

bool decodeFloatingPoint(quint64 raw_bits, int bit_count, double *value, QString *error_message)
{
    if (value == nullptr) {
        setError(error_message, QStringLiteral("value output pointer is null"));
        return false;
    }

    if (bit_count == 32) {
        const quint32 bits = static_cast<quint32>(raw_bits);
        float float_value = 0.0F;
        std::memcpy(&float_value, &bits, sizeof(float_value));
        *value = static_cast<double>(float_value);
        return true;
    }
    if (bit_count == 64) {
        double double_value = 0.0;
        std::memcpy(&double_value, &raw_bits, sizeof(double_value));
        *value = double_value;
        return true;
    }

    setError(
        error_message,
        QStringLiteral("Floating point parameter requires 32 or 64 bits, got %1").arg(bit_count));
    return false;
}

bool applyCalibration(double raw_value,
                      const CalibrationDefinition &calibration,
                      double *engineering_value,
                      bool *calibration_applied,
                      QString *error_message)
{
    if (engineering_value == nullptr || calibration_applied == nullptr) {
        setError(error_message, QStringLiteral("calibration output pointer is null"));
        return false;
    }

    *engineering_value = raw_value;
    *calibration_applied = false;
    if (!hasEngineeringCalibration(calibration.type)) {
        return true;
    }

    *calibration_applied = true;
    switch (calibration.type) {
    case CalibrationType::Linear:
        if (calibration.coefficients.isEmpty()) {
            *engineering_value = raw_value;
        } else if (calibration.coefficients.size() == 1) {
            *engineering_value = calibration.coefficients.at(0) * raw_value;
        } else {
            *engineering_value = calibration.coefficients.at(0)
                + (calibration.coefficients.at(1) * raw_value);
        }
        return true;

    case CalibrationType::Polynomial: {
        if (calibration.coefficients.isEmpty()) {
            *engineering_value = raw_value;
            return true;
        }

        double result = 0.0;
        double power = 1.0;
        for (const double coefficient : calibration.coefficients) {
            result += coefficient * power;
            power *= raw_value;
        }
        *engineering_value = result;
        return true;
    }

    case CalibrationType::Segmented: {
        if (calibration.points.isEmpty()) {
            setError(error_message, QStringLiteral("Segmented calibration has no points"));
            return false;
        }

        const QList<CalibrationPoint> &points = calibration.points;
        if (points.size() == 1 || raw_value <= points.first().raw_value) {
            *engineering_value = points.first().engineering_value;
            return true;
        }
        if (raw_value >= points.last().raw_value) {
            *engineering_value = points.last().engineering_value;
            return true;
        }

        for (int index = 1; index < points.size(); ++index) {
            const CalibrationPoint &right = points.at(index);
            if (raw_value > right.raw_value) {
                continue;
            }

            const CalibrationPoint &left = points.at(index - 1);
            const double denominator = right.raw_value - left.raw_value;
            if (denominator == 0.0) {
                *engineering_value = right.engineering_value;
                return true;
            }

            const double ratio = (raw_value - left.raw_value) / denominator;
            *engineering_value = left.engineering_value
                + (ratio * (right.engineering_value - left.engineering_value));
            return true;
        }

        *engineering_value = points.last().engineering_value;
        return true;
    }

    case CalibrationType::None:
    case CalibrationType::Unknown:
    default:
        *calibration_applied = false;
        *engineering_value = raw_value;
        return true;
    }
}

} // namespace

struct FastFixedPcmProcessor::CompiledSegmentPlan
{
    qint64 word_ordinal = 0;
    qint64 word_byte_offset = 0;
    int word_byte_count = 0;
    int source_bit = 0;
    int bit_length = 0;
    int computed_object_shift = 0;
    bool fast_access = false;
    int source_right_shift = 0;
    quint64 source_mask = 0;
};

struct FastFixedPcmProcessor::CompiledParameterPlan
{
    QString parameter_id;
    QString parameter_name;
    QString unit;
    ParameterCodeType code_type = ParameterCodeType::Unknown;
    int sign_bit = 0;
    bool inverse = false;
    CalibrationDefinition calibration;
    QList<CompiledSegmentPlan> segments;
    int bit_count = 0;
    int output_column_index = -1;
};

struct FastFixedPcmProcessor::CompiledStreamDecodePlan
{
    int stream_index = 0;
    QString stream_id;
    int word_length_bits = 0;
    int word_byte_count = 0;
    QList<CompiledParameterPlan> parameters;
};

struct FastFixedPcmProcessor::StreamPlan
{
    StreamDefinition stream;
    StreamLayoutTable layout_table;
    QSet<QString> selected_parameter_ids;
    CompiledStreamDecodePlan decode_plan;
    QString raw_file_path;
    qsizetype frame_bytes = 0;
    qint64 start_offset = 0;
};

struct FastFixedPcmProcessor::ExecutionPlan
{
    QList<StreamPlan> streams;
    qint64 total_bytes = 0;
    qsizetype max_frame_bytes = 0;
};

struct FastFixedPcmProcessor::StreamBlock
{
    StreamPlan plan;
    QByteArray block_data;
    quint64 first_frame_index = 0;
    quint64 frame_count = 0;
};

struct FastFixedPcmProcessor::ChunkResult
{
    QList<ParameterValue> values;
    quint64 processed_frame_count = 0;
    quint64 produced_value_count = 0;
    QString error_message;
};

struct FastFixedPcmProcessor::FastOutputColumn
{
    int stream_index = 0;
    QString parameter_id;
    QString parameter_name;
    QByteArray header_text;
};

struct FastFixedPcmProcessor::FastTxtBlock
{
    QByteArray data;
    quint64 processed_frame_count = 0;
    quint64 produced_value_count = 0;
    QString error_message;
};

FastFixedPcmProcessor::FastFixedPcmProcessor(const FastFixedPcmTask &task,
                                             const FastFixedPcmProcessorOptions &options,
                                             QObject *parent)
    : QObject(parent)
    , task_(task)
    , options_(options)
{
    qRegisterMetaType<FastFixedPcmProcessorOptions>("FastFixedPcmProcessorOptions");
    qRegisterMetaType<FastFixedPcmTask>("FastFixedPcmTask");
}

bool FastFixedPcmProcessor::canUseFastPath(
    const TaskDefinition &definition,
    const QList<StreamDefinition> &streams,
    const QList<StreamLayoutTable> &layout_tables,
    const QHash<QString, QSet<QString>> &selected_parameter_ids_by_stream,
    QString *reason,
    qint64 resync_window_bytes)
{
    ExecutionPlan plan;
    return buildExecutionPlanForTask(definition,
                                     streams,
                                     layout_tables,
                                     selected_parameter_ids_by_stream,
                                     resync_window_bytes,
                                     &plan,
                                     reason);
}

void FastFixedPcmProcessor::process()
{
    stop_requested_.storeRelease(0);
    QElapsedTimer total_timer;
    total_timer.start();
    qint64 read_elapsed_ms = 0;
    qint64 sync_elapsed_ms = 0;
    qint64 decode_write_elapsed_ms = 0;
    qint64 written_text_bytes = 0;

    ExecutionPlan plan;
    QString error_message;
    if (!buildExecutionPlan(&plan, &error_message)) {
        log(LogLevel::Error, error_message);
        emit failed(task_.definition.task_id, error_message);
        return;
    }

    QList<FastOutputColumn> output_columns;
    if (!prepareOutputColumns(&plan, task_.definition, &output_columns, &error_message)) {
        log(LogLevel::Error, error_message);
        emit failed(task_.definition.task_id, error_message);
        return;
    }

    QSaveFile output_file;
    QString output_path;
    if (!openFastTextOutput(task_.definition,
                            task_.writer_options,
                            output_columns,
                            &output_file,
                            &output_path,
                            &error_message)) {
        log(LogLevel::Error, error_message);
        emit failed(task_.definition.task_id, error_message);
        return;
    }
    written_text_bytes += output_file.pos();

    struct SourceState
    {
        StreamPlan plan;
        QFile *file = nullptr;
        qint64 file_size = 0;
        qint64 next_offset = 0;
        qint64 end_offset = 0;
        quint64 next_frame_index = 0;
        bool finished = false;
    };

    QList<SourceState> sources;
    auto cleanup_sources = [&sources]() {
        for (SourceState &source : sources) {
            delete source.file;
            source.file = nullptr;
        }
        sources.clear();
    };

    qint64 range_total_bytes = 0;

    for (const StreamPlan &stream_plan : plan.streams) {
        SourceState source;
        source.plan = stream_plan;
        source.file = new QFile(stream_plan.raw_file_path);
        if (!source.file->open(QIODevice::ReadOnly)) {
            error_message = QStringLiteral("Failed to open raw PCM file '%1': %2")
                                .arg(stream_plan.raw_file_path, source.file->errorString());
            delete source.file;
            cleanup_sources();
            output_file.cancelWriting();
            log(LogLevel::Error, error_message);
            emit failed(task_.definition.task_id, error_message);
            return;
        }

        source.file_size = source.file->size();
        const qint64 available_frame_count = qMax<qint64>(
            0, (source.file_size - stream_plan.start_offset) / stream_plan.frame_bytes);
        qint64 start_frame = 0;
        qint64 end_frame = available_frame_count;
        if (task_.definition.user_sample_rate_hz > 0.0) {
            const double frames_per_second = task_.definition.user_sample_rate_hz /
                static_cast<double>(stream_plan.stream.samples_per_frame);
            start_frame = qBound<qint64>(0,
                                         static_cast<qint64>(std::ceil(
                                             task_.definition.time_range.start_seconds * frames_per_second)),
                                         available_frame_count);
            if (task_.definition.time_range.hasEnd()) {
                end_frame = qBound<qint64>(start_frame,
                                           static_cast<qint64>(std::ceil(
                                               task_.definition.time_range.end_seconds * frames_per_second)),
                                           available_frame_count);
            }
        }
        source.next_frame_index = static_cast<quint64>(start_frame);
        source.next_offset = stream_plan.start_offset + (start_frame * stream_plan.frame_bytes);
        source.end_offset = stream_plan.start_offset + (end_frame * stream_plan.frame_bytes);
        source.finished = source.next_offset >= source.end_offset;
        range_total_bytes += qMax<qint64>(0, source.end_offset - source.next_offset);
        if (source.next_offset > stream_plan.start_offset) {
            log(LogLevel::Warning,
                QStringLiteral("Fast path range for '%1' starts at offset %2")
                    .arg(stream_plan.raw_file_path)
                    .arg(source.next_offset));
        }
        sources.append(source);
    }

    const qint64 frames_per_block =
        qMax<qint64>(1, options_.block_size_bytes / qMax<qsizetype>(1, plan.max_frame_bytes));

    quint64 processed_frame_count = 0;
    quint64 produced_value_count = 0;
    qint64 processed_bytes = 0;

    emit started(task_.definition.task_id);
    log(LogLevel::Info,
        QStringLiteral("Per-stream fixed fast path started, sources=%1, frames_per_block=%2")
            .arg(sources.size())
            .arg(frames_per_block));

    while (!stop_requested_.loadAcquire()) {
        QList<StreamBlock> stream_blocks;

        for (SourceState &source : sources) {
            if (source.finished) {
                continue;
            }

            const qint64 remaining_bytes = source.end_offset - source.next_offset;
            const qint64 available_frames = remaining_bytes / source.plan.frame_bytes;
            if (available_frames <= 0) {
                if (remaining_bytes > 0) {
                    log(LogLevel::Warning,
                        QStringLiteral("Dropping incomplete fixed-frame tail of %1 bytes in '%2'")
                            .arg(remaining_bytes)
                            .arg(source.plan.raw_file_path));
                }
                source.finished = true;
                continue;
            }

            const qint64 current_frame_count = std::min(frames_per_block, available_frames);
            const qint64 read_bytes = current_frame_count * source.plan.frame_bytes;
            if (read_bytes <= 0 || read_bytes > std::numeric_limits<int>::max()) {
                error_message = QStringLiteral("Invalid fixed fast block size for stream '%1'")
                                    .arg(source.plan.stream.stream_id);
                cleanup_sources();
                output_file.cancelWriting();
                log(LogLevel::Error, error_message);
                emit failed(task_.definition.task_id, error_message);
                return;
            }

            if (!source.file->seek(source.next_offset)) {
                error_message = QStringLiteral("Failed to seek '%1' to offset %2")
                                    .arg(source.plan.raw_file_path)
                                    .arg(source.next_offset);
                cleanup_sources();
                output_file.cancelWriting();
                log(LogLevel::Error, error_message);
                emit failed(task_.definition.task_id, error_message);
                return;
            }

            QElapsedTimer stage_timer;
            stage_timer.start();
            const QByteArray block_data = source.file->read(static_cast<int>(read_bytes));
            read_elapsed_ms += stage_timer.elapsed();
            if (block_data.size() != read_bytes) {
                error_message = QStringLiteral("Failed to read fixed block at offset %1 from '%2'")
                                    .arg(source.next_offset)
                                    .arg(source.plan.raw_file_path);
                cleanup_sources();
                output_file.cancelWriting();
                log(LogLevel::Error, error_message);
                emit failed(task_.definition.task_id, error_message);
                return;
            }

            stage_timer.restart();
            for (qint64 frame_index = 0; frame_index < current_frame_count; ++frame_index) {
                const qsizetype frame_offset = static_cast<qsizetype>(frame_index * source.plan.frame_bytes);
                if (!blockHasFrameAt(block_data.constData() + frame_offset,
                                     block_data.size() - frame_offset,
                                     source.plan)) {
                    error_message = QStringLiteral("Fixed fast path lost sync in '%1' at offset %2")
                                        .arg(source.plan.raw_file_path)
                                        .arg(source.next_offset + frame_offset);
                    cleanup_sources();
                    output_file.cancelWriting();
                    log(LogLevel::Error, error_message);
                    emit failed(task_.definition.task_id, error_message);
                    return;
                }
            }
            sync_elapsed_ms += stage_timer.elapsed();

            StreamBlock stream_block;
            stream_block.plan = source.plan;
            stream_block.block_data = block_data;
            stream_block.first_frame_index = source.next_frame_index;
            stream_block.frame_count = static_cast<quint64>(current_frame_count);
            stream_blocks.append(stream_block);

            source.next_offset += read_bytes;
            source.next_frame_index += static_cast<quint64>(current_frame_count);
            processed_bytes += read_bytes;
        }

        if (stream_blocks.isEmpty()) {
            break;
        }

        QElapsedTimer stage_timer;
        stage_timer.start();
        if (!processAndWriteTxtBlocks(stream_blocks,
                                      &output_file,
                                      output_columns,
                                      &processed_frame_count,
                                      &produced_value_count,
                                      &written_text_bytes,
                                      &error_message)) {
            cleanup_sources();
            output_file.cancelWriting();
            log(LogLevel::Error, error_message);
            emit failed(task_.definition.task_id, error_message);
            return;
        }
        decode_write_elapsed_ms += stage_timer.elapsed();

        emit progressChanged(task_.definition.task_id, processed_bytes, range_total_bytes);
    }

    cleanup_sources();

    if (stop_requested_.loadAcquire()) {
        output_file.cancelWriting();
        log(LogLevel::Warning, QStringLiteral("Per-stream fixed fast path stopped by request"));
        emit stopped(task_.definition.task_id);
        return;
    }

    QElapsedTimer commit_timer;
    commit_timer.start();
    if (!output_file.commit()) {
        error_message = QStringLiteral("Failed to commit fast result file '%1': %2")
                            .arg(output_path, output_file.errorString());
        log(LogLevel::Error, error_message);
        emit failed(task_.definition.task_id, error_message);
        return;
    }
    const qint64 commit_elapsed_ms = commit_timer.elapsed();

    const double read_mib_per_second = read_elapsed_ms > 0
        ? (static_cast<double>(processed_bytes) / (1024.0 * 1024.0)) / (static_cast<double>(read_elapsed_ms) / 1000.0)
        : 0.0;
    const double write_mib_per_second = decode_write_elapsed_ms > 0
        ? (static_cast<double>(written_text_bytes) / (1024.0 * 1024.0)) /
            (static_cast<double>(decode_write_elapsed_ms) / 1000.0)
        : 0.0;

    log(LogLevel::Info,
        QStringLiteral("Per-stream fixed fast path finished, frames=%1, values=%2, txt_bytes=%3, "
                       "read_ms=%4, sync_ms=%5, decode_write_ms=%6, commit_ms=%7, total_ms=%8, "
                       "read_mib_s=%9, txt_mib_s=%10")
            .arg(processed_frame_count)
            .arg(produced_value_count)
            .arg(written_text_bytes)
            .arg(read_elapsed_ms)
            .arg(sync_elapsed_ms)
            .arg(decode_write_elapsed_ms)
            .arg(commit_elapsed_ms)
            .arg(total_timer.elapsed())
            .arg(read_mib_per_second, 0, 'f', 2)
            .arg(write_mib_per_second, 0, 'f', 2));
    emit finished(task_.definition.task_id, output_path, processed_frame_count, produced_value_count);
}

void FastFixedPcmProcessor::requestStop()
{
    stop_requested_.storeRelease(1);
}

bool FastFixedPcmProcessor::buildExecutionPlan(ExecutionPlan *plan, QString *error_message) const
{
    return buildExecutionPlanForTask(task_.definition,
                                     task_.streams,
                                     task_.layout_tables,
                                     task_.selected_parameter_ids_by_stream,
                                     options_.resync_window_bytes,
                                     plan,
                                     error_message);
}

bool FastFixedPcmProcessor::buildExecutionPlanForTask(
    const TaskDefinition &definition,
    const QList<StreamDefinition> &streams,
    const QList<StreamLayoutTable> &layout_tables,
    const QHash<QString, QSet<QString>> &selected_parameter_ids_by_stream,
    qint64 resync_window_bytes,
    ExecutionPlan *plan,
    QString *error_message)
{
    if (plan == nullptr) {
        setError(error_message, QStringLiteral("execution plan output pointer is null"));
        return false;
    }

    if (streams.isEmpty()) {
        setError(error_message, QStringLiteral("fixed fast path requires at least one stream"));
        return false;
    }

    QHash<QString, StreamLayoutTable> layout_by_stream_id;
    for (const StreamLayoutTable &layout_table : layout_tables) {
        layout_by_stream_id.insert(layout_table.stream_id, layout_table);
    }

    ExecutionPlan generated_plan;
    for (const StreamDefinition &stream : streams) {
        if (!selected_parameter_ids_by_stream.isEmpty()
            && !selected_parameter_ids_by_stream.contains(stream.stream_id)) {
            continue;
        }

        if (stream.stream_index <= 0) {
            setError(error_message,
                     QStringLiteral("fixed fast path stream '%1' has invalid stream_index %2")
                         .arg(stream.stream_id)
                         .arg(stream.stream_index));
            return false;
        }

        if (!layout_by_stream_id.contains(stream.stream_id)) {
            setError(error_message,
                     QStringLiteral("fixed fast path has no layout table for stream '%1'").arg(stream.stream_id));
            return false;
        }

        const StreamLayoutTable layout_table = layout_by_stream_id.value(stream.stream_id);
        const qint64 frame_bytes = syncUnitByteCount(layout_table);
        if (stream.sync_word.isEmpty() || frame_bytes <= 0) {
            setError(error_message, QStringLiteral("fixed fast path requires valid stream sync and frame size"));
            return false;
        }
        if (frame_bytes < stream.sync_word.size()) {
            setError(error_message, QStringLiteral("fixed fast path frame is shorter than sync word"));
            return false;
        }
        if (frame_bytes > std::numeric_limits<int>::max()) {
            setError(error_message, QStringLiteral("fixed fast path frame is too large for QByteArray blocks"));
            return false;
        }

        StreamPlan stream_plan;
        stream_plan.stream = stream;
        stream_plan.layout_table = layout_table;
        stream_plan.selected_parameter_ids = selected_parameter_ids_by_stream.value(stream.stream_id);
        stream_plan.raw_file_path = rawFilePathForStream(definition.raw_file_path, stream.stream_index);
        stream_plan.frame_bytes = static_cast<qsizetype>(frame_bytes);
        if (!compileDecodePlan(layout_table,
                               stream_plan.selected_parameter_ids,
                               &stream_plan.decode_plan,
                               error_message)) {
            return false;
        }

        QFile file(stream_plan.raw_file_path);
        if (!file.open(QIODevice::ReadOnly)) {
            setError(error_message,
                     QStringLiteral("failed to open raw file for fast-path probe '%1': %2")
                         .arg(stream_plan.raw_file_path, file.errorString()));
            return false;
        }

        if (file.size() < stream_plan.frame_bytes) {
            setError(error_message,
                     QStringLiteral("raw file '%1' is shorter than one fixed frame")
                         .arg(stream_plan.raw_file_path));
            return false;
        }

        qint64 first_frame_offset = -1;
        if (!findFirstFrameOffset(&file,
                                  stream_plan,
                                  resync_window_bytes,
                                  &first_frame_offset,
                                  error_message)) {
            return false;
        }

        if (first_frame_offset < 0) {
            setError(error_message,
                     QStringLiteral("raw file '%1' does not contain stream '%2' sync word within first %3 bytes")
                         .arg(stream_plan.raw_file_path)
                         .arg(stream.stream_id)
                         .arg(qMax<qint64>(stream_plan.frame_bytes, resync_window_bytes)));
            return false;
        }

        stream_plan.start_offset = first_frame_offset;
        generated_plan.total_bytes += file.size() - stream_plan.start_offset;
        generated_plan.max_frame_bytes = qMax(generated_plan.max_frame_bytes, stream_plan.frame_bytes);
        generated_plan.streams.append(stream_plan);
    }

    if (generated_plan.streams.isEmpty()) {
        setError(error_message, QStringLiteral("fixed fast path has no selected streams"));
        return false;
    }

    *plan = generated_plan;
    return true;
}

bool FastFixedPcmProcessor::compileDecodePlan(const StreamLayoutTable &layout_table,
                                              const QSet<QString> &selected_parameter_ids,
                                              CompiledStreamDecodePlan *decode_plan,
                                              QString *error_message)
{
    if (decode_plan == nullptr) {
        setError(error_message, QStringLiteral("decode plan output pointer is null"));
        return false;
    }

    if (layout_table.word_length_bits <= 0 || layout_table.frame_length_words <= 0
        || layout_table.subframe_depth <= 0) {
        setError(error_message, QStringLiteral("fixed fast decode plan requires valid layout geometry"));
        return false;
    }

    if ((layout_table.word_length_bits % 8) != 0) {
        setError(error_message,
                 QStringLiteral("fixed fast decode plan requires byte-aligned word_length_bits for stream '%1'")
                     .arg(layout_table.stream_id));
        return false;
    }

    const int word_byte_count = layout_table.word_length_bits / 8;
    const qint64 frame_byte_count =
        static_cast<qint64>(layout_table.frame_length_words) * layout_table.subframe_depth * word_byte_count;

    CompiledStreamDecodePlan generated_plan;
    generated_plan.stream_index = layout_table.stream_index;
    generated_plan.stream_id = layout_table.stream_id;
    generated_plan.word_length_bits = layout_table.word_length_bits;
    generated_plan.word_byte_count = word_byte_count;
    generated_plan.parameters.reserve(layout_table.entries.size());

    for (const StreamLayoutEntry &entry : layout_table.entries) {
        if (!selected_parameter_ids.isEmpty() && !selected_parameter_ids.contains(entry.parameter_id)) {
            continue;
        }

        CompiledParameterPlan parameter_plan;
        parameter_plan.parameter_id = entry.parameter_id;
        parameter_plan.parameter_name = entry.parameter_name;
        parameter_plan.unit = entry.unit;
        parameter_plan.code_type = entry.code_type;
        parameter_plan.sign_bit = entry.sign_bit;
        parameter_plan.inverse = entry.inverse;
        parameter_plan.calibration = entry.calibration;
        if (parameter_plan.calibration.type == CalibrationType::Segmented) {
            std::sort(parameter_plan.calibration.points.begin(),
                      parameter_plan.calibration.points.end(),
                      [](const CalibrationPoint &left, const CalibrationPoint &right) {
                          return left.raw_value < right.raw_value;
                      });
        }

        QList<CompiledSegmentPlan> segments;
        for (int segment_index = 0; segment_index < entry.word_segments.size(); ++segment_index) {
            const ParameterWordSegment &segment = entry.word_segments.at(segment_index);
            if (!segment.isActive()) {
                continue;
            }

            if (segment.bit_length > 64 || parameter_plan.bit_count > 64 - segment.bit_length) {
                setError(error_message,
                         QStringLiteral("Parameter %1 exceeds 64 assembled bits")
                             .arg(parameterLabel(entry)));
                return false;
            }

            if (segment.source_bit < 0 || segment.bit_length > layout_table.word_length_bits
                || segment.source_bit > layout_table.word_length_bits - segment.bit_length) {
                setError(error_message,
                         QStringLiteral("Parameter %1 segment %2 source range is outside word length")
                             .arg(parameterLabel(entry))
                             .arg(segment_index));
                return false;
            }

            int frame_index = 0;
            QString local_error;
            if (!normalizeLayoutIndex(segment.frame_no,
                                      layout_table.subframe_depth,
                                      &frame_index,
                                      &local_error)) {
                setError(error_message,
                         QStringLiteral("Parameter %1 segment %2 has invalid frame_no: %3")
                             .arg(parameterLabel(entry))
                             .arg(segment_index)
                             .arg(local_error));
                return false;
            }

            int word_index = 0;
            if (!normalizeLayoutIndex(segment.word_no,
                                      layout_table.frame_length_words,
                                      &word_index,
                                      &local_error)) {
                setError(error_message,
                         QStringLiteral("Parameter %1 segment %2 has invalid word_no: %3")
                             .arg(parameterLabel(entry))
                             .arg(segment_index)
                             .arg(local_error));
                return false;
            }

            CompiledSegmentPlan compiled_segment;
            compiled_segment.word_ordinal =
                (static_cast<qint64>(frame_index) * layout_table.frame_length_words) + word_index;
            compiled_segment.word_byte_offset = compiled_segment.word_ordinal * word_byte_count;
            compiled_segment.word_byte_count = word_byte_count;
            compiled_segment.source_bit = segment.source_bit;
            compiled_segment.bit_length = segment.bit_length;
            const int word_bit_count = word_byte_count * 8;
            compiled_segment.fast_access = word_byte_count <= static_cast<int>(sizeof(quint64));
            compiled_segment.source_right_shift = word_bit_count - segment.source_bit - segment.bit_length;
            compiled_segment.source_mask = bitMask(segment.bit_length);
            if (compiled_segment.source_right_shift < 0) {
                compiled_segment.fast_access = false;
            }
            if (compiled_segment.word_byte_offset < 0
                || compiled_segment.word_byte_offset > frame_byte_count
                || word_byte_count > frame_byte_count - compiled_segment.word_byte_offset) {
                setError(error_message,
                         QStringLiteral("Parameter %1 segment %2 word offset is outside frame")
                             .arg(parameterLabel(entry))
                             .arg(segment_index));
                return false;
            }

            parameter_plan.bit_count += segment.bit_length;
            segments.append(compiled_segment);
        }

        if (segments.isEmpty()) {
            setError(error_message,
                     QStringLiteral("Parameter %1 has no active word segments").arg(parameterLabel(entry)));
            return false;
        }

        int computed_shift = 0;
        for (int index = segments.size() - 1; index >= 0; --index) {
            segments[index].computed_object_shift = computed_shift;
            computed_shift += segments.at(index).bit_length;
        }
        parameter_plan.segments = segments;
        generated_plan.parameters.append(parameter_plan);
    }

    if (generated_plan.parameters.isEmpty()) {
        setError(error_message,
                 QStringLiteral("fixed fast decode plan for stream '%1' has no selected parameters")
                     .arg(layout_table.stream_id));
        return false;
    }

    *decode_plan = generated_plan;
    return true;
}

FastFixedPcmProcessor::ChunkResult FastFixedPcmProcessor::processStreamFrameRange(
    const QByteArray &block_data,
    const StreamPlan &stream_plan,
    quint64 first_frame,
    quint64 frame_offset_in_block,
    quint64 frame_count) const
{
    ChunkResult result;
    const char *base = block_data.constData();
    for (quint64 index = 0; index < frame_count; ++index) {
        const quint64 frame_index = first_frame + index;
        const qint64 block_offset =
            static_cast<qint64>(frame_offset_in_block + index) * stream_plan.frame_bytes;
        const char *frame_data = base + block_offset;
        const double timestamp_seconds = task_.definition.user_sample_rate_hz > 0.0
            ? static_cast<double>(frame_index * stream_plan.stream.samples_per_frame) /
                task_.definition.user_sample_rate_hz
            : 0.0;

        QList<ParameterValue> frame_values;
        QString local_error;
        if (!decodeFrameWithCompiledPlan(frame_data,
                                         stream_plan.frame_bytes,
                                         task_.definition.task_id,
                                         timestamp_seconds,
                                         stream_plan.decode_plan,
                                         &frame_values,
                                         &local_error)) {
            result.error_message = local_error;
            return result;
        }

        result.values.append(frame_values);
        ++result.processed_frame_count;
        result.produced_value_count += static_cast<quint64>(frame_values.size());
    }

    std::stable_sort(result.values.begin(), result.values.end(), parameterValueLess);
    return result;
}

bool FastFixedPcmProcessor::decodeFrameWithCompiledPlan(const char *frame_data,
                                                        qsizetype frame_size,
                                                        const QString &task_id,
                                                        double timestamp_seconds,
                                                        const CompiledStreamDecodePlan &decode_plan,
                                                        QList<ParameterValue> *values,
                                                        QString *error_message)
{
    if (values == nullptr) {
        setError(error_message, QStringLiteral("values output pointer is null"));
        return false;
    }
    if (frame_data == nullptr || frame_size <= 0) {
        setError(error_message, QStringLiteral("frame_data is empty"));
        return false;
    }

    values->clear();
    values->reserve(decode_plan.parameters.size());
    for (const CompiledParameterPlan &parameter : decode_plan.parameters) {
        quint64 raw_bits = 0;
        for (const CompiledSegmentPlan &segment : parameter.segments) {
            quint64 segment_value = 0;
            QString local_error;
            if (!readCompiledSegment(frame_data, frame_size, segment, &segment_value, &local_error)) {
                setError(error_message,
                         QStringLiteral("Parameter %1/%2 segment read failed: %3")
                             .arg(parameter.parameter_id, parameter.parameter_name, local_error));
                return false;
            }

            raw_bits |= (segment_value & bitMask(segment.bit_length)) << segment.computed_object_shift;
        }

        QVariant converted_value;
        QString local_error;
        CompiledParameterPlan raw_parameter = parameter;
        raw_parameter.calibration.type = CalibrationType::None;
        QVariant decoded_raw_value;
        if (!convertCompiledValue(raw_bits, parameter.bit_count, raw_parameter, &decoded_raw_value, &local_error)) {
            setError(error_message, local_error);
            return false;
        }

        if (!convertCompiledValue(raw_bits,
                                  parameter.bit_count,
                                  parameter,
                                  &converted_value,
                                  &local_error)) {
            setError(error_message,
                     QStringLiteral("Parameter %1/%2 conversion failed: %3")
                         .arg(parameter.parameter_id, parameter.parameter_name, local_error));
            return false;
        }

        ParameterValue parameter_value;
        parameter_value.task_id = task_id;
        parameter_value.stream_index = decode_plan.stream_index;
        parameter_value.parameter_id = parameter.parameter_id;
        parameter_value.parameter_name = parameter.parameter_name;
        parameter_value.timestamp_seconds = timestamp_seconds;
        parameter_value.raw_value = decoded_raw_value;
        parameter_value.value = converted_value;
        parameter_value.unit = parameter.unit;
        values->append(parameter_value);
    }

    return true;
}

bool FastFixedPcmProcessor::readCompiledSegment(const char *frame_data,
                                                qsizetype frame_size,
                                                const CompiledSegmentPlan &segment,
                                                quint64 *value,
                                                QString *error_message)
{
    if (value == nullptr) {
        setError(error_message, QStringLiteral("value output pointer is null"));
        return false;
    }
    if (frame_data == nullptr || frame_size <= 0) {
        setError(error_message, QStringLiteral("frame_data is empty"));
        return false;
    }
    if (segment.word_byte_count <= 0 || segment.source_bit < 0 || segment.bit_length < 0
        || segment.bit_length > 64) {
        setError(error_message, QStringLiteral("compiled segment has invalid bit geometry"));
        return false;
    }
    if (segment.word_byte_offset < 0 || segment.word_byte_offset > frame_size
        || segment.word_byte_count > frame_size - segment.word_byte_offset) {
        setError(error_message,
                 QStringLiteral("compiled segment word range [%1, %2) exceeds frame size %3")
                     .arg(segment.word_byte_offset)
                     .arg(segment.word_byte_offset + segment.word_byte_count)
                     .arg(frame_size));
        return false;
    }

    if (segment.fast_access) {
        const char *word_data = frame_data + segment.word_byte_offset;
        const quint64 word_value = readLittleEndianWordValue(word_data, segment.word_byte_count);
        *value = (word_value >> segment.source_right_shift) & segment.source_mask;
        return true;
    }

    quint64 extracted_value = 0;
    for (int index = 0; index < segment.bit_length; ++index) {
        const int bit_in_word = segment.source_bit + index;
        const int logical_byte_index = bit_in_word / 8;
        const int bit_in_logical_byte = bit_in_word % 8;
        if (logical_byte_index >= segment.word_byte_count) {
            setError(error_message, QStringLiteral("compiled segment bit range exceeds word byte count"));
            return false;
        }

        const qint64 raw_byte_index =
            segment.word_byte_offset + (segment.word_byte_count - 1 - logical_byte_index);
        const int bit_in_byte = 7 - bit_in_logical_byte;
        const uchar byte = static_cast<uchar>(frame_data[raw_byte_index]);
        extracted_value = (extracted_value << 1) | ((byte >> bit_in_byte) & 0x01);
    }

    *value = extracted_value;
    return true;
}

bool FastFixedPcmProcessor::convertCompiledValue(quint64 raw_bits,
                                                 int bit_count,
                                                 const CompiledParameterPlan &parameter,
                                                 QVariant *value,
                                                 QString *error_message)
{
    if (value == nullptr) {
        setError(error_message, QStringLiteral("value output pointer is null"));
        return false;
    }
    if (bit_count <= 0 || bit_count > 64) {
        setError(error_message,
                 QStringLiteral("Parameter %1 has invalid bit_count=%2")
                     .arg(parameter.parameter_id)
                     .arg(bit_count));
        return false;
    }

    const quint64 mask = bitMask(bit_count);
    quint64 normalized_bits = raw_bits & mask;
    if (parameter.inverse) {
        normalized_bits = (~normalized_bits) & mask;
    }

    QVariant decoded_value;
    double numeric_value = 0.0;
    switch (parameter.code_type) {
    case ParameterCodeType::SignedInteger: {
        qlonglong signed_value = 0;
        if (!decodeSignedInteger(normalized_bits, bit_count, parameter.sign_bit, &signed_value, error_message)) {
            return false;
        }
        decoded_value = QVariant::fromValue(signed_value);
        numeric_value = static_cast<double>(signed_value);
        break;
    }
    case ParameterCodeType::Bcd: {
        qlonglong bcd_value = 0;
        if (!decodeBcd(normalized_bits, bit_count, &bcd_value, error_message)) {
            return false;
        }
        decoded_value = QVariant::fromValue(bcd_value);
        numeric_value = static_cast<double>(bcd_value);
        break;
    }
    case ParameterCodeType::FloatingPoint: {
        double floating_value = 0.0;
        if (!decodeFloatingPoint(normalized_bits, bit_count, &floating_value, error_message)) {
            return false;
        }
        decoded_value = floating_value;
        numeric_value = floating_value;
        break;
    }
    case ParameterCodeType::UnsignedInteger:
    case ParameterCodeType::Unknown:
    default:
        decoded_value = QVariant::fromValue(normalized_bits);
        numeric_value = static_cast<double>(normalized_bits);
        break;
    }

    bool calibration_applied = false;
    double engineering_value = numeric_value;
    if (!applyCalibration(numeric_value,
                          parameter.calibration,
                          &engineering_value,
                          &calibration_applied,
                          error_message)) {
        return false;
    }

    *value = calibration_applied ? QVariant(engineering_value) : decoded_value;
    return true;
}

bool FastFixedPcmProcessor::convertCompiledValueToText(quint64 raw_bits,
                                                       int bit_count,
                                                       const CompiledParameterPlan &parameter,
                                                       QByteArray *value,
                                                       QString *error_message)
{
    if (value == nullptr) {
        setError(error_message, QStringLiteral("value output pointer is null"));
        return false;
    }
    if (bit_count <= 0 || bit_count > 64) {
        setError(error_message,
                 QStringLiteral("Parameter %1 has invalid bit_count=%2")
                     .arg(parameter.parameter_id)
                     .arg(bit_count));
        return false;
    }

    const quint64 mask = bitMask(bit_count);
    quint64 normalized_bits = raw_bits & mask;
    if (parameter.inverse) {
        normalized_bits = (~normalized_bits) & mask;
    }

    double numeric_value = 0.0;
    QByteArray decoded_text;
    switch (parameter.code_type) {
    case ParameterCodeType::SignedInteger: {
        qlonglong signed_value = 0;
        if (!decodeSignedInteger(normalized_bits, bit_count, parameter.sign_bit, &signed_value, error_message)) {
            return false;
        }
        numeric_value = static_cast<double>(signed_value);
        decoded_text = QByteArray::number(signed_value);
        break;
    }
    case ParameterCodeType::Bcd: {
        qlonglong bcd_value = 0;
        if (!decodeBcd(normalized_bits, bit_count, &bcd_value, error_message)) {
            return false;
        }
        numeric_value = static_cast<double>(bcd_value);
        decoded_text = QByteArray::number(bcd_value);
        break;
    }
    case ParameterCodeType::FloatingPoint: {
        double floating_value = 0.0;
        if (!decodeFloatingPoint(normalized_bits, bit_count, &floating_value, error_message)) {
            return false;
        }
        numeric_value = floating_value;
        decoded_text = QByteArray::number(floating_value, 'g', 17);
        break;
    }
    case ParameterCodeType::UnsignedInteger:
    case ParameterCodeType::Unknown:
    default:
        numeric_value = static_cast<double>(normalized_bits);
        decoded_text = QByteArray::number(normalized_bits);
        break;
    }

    bool calibration_applied = false;
    double engineering_value = numeric_value;
    if (!applyCalibration(numeric_value,
                          parameter.calibration,
                          &engineering_value,
                          &calibration_applied,
                          error_message)) {
        return false;
    }

    *value = calibration_applied ? QByteArray::number(engineering_value, 'g', 17) : decoded_text;
    return true;
}

bool FastFixedPcmProcessor::decodeFrameToTextRow(const char *frame_data,
                                                 qsizetype frame_size,
                                                 const CompiledStreamDecodePlan &decode_plan,
                                                 bool include_raw_values,
                                                 QList<QByteArray> *row_values,
                                                 quint64 *produced_value_count,
                                                 QString *error_message)
{
    if (row_values == nullptr) {
        setError(error_message, QStringLiteral("row_values output pointer is null"));
        return false;
    }
    if (frame_data == nullptr || frame_size <= 0) {
        setError(error_message, QStringLiteral("frame_data is empty"));
        return false;
    }

    for (const CompiledParameterPlan &parameter : decode_plan.parameters) {
        if (parameter.output_column_index < 0 || parameter.output_column_index >= row_values->size()) {
            setError(error_message,
                     QStringLiteral("Parameter %1/%2 has invalid output column index %3")
                         .arg(parameter.parameter_id)
                         .arg(parameter.parameter_name)
                         .arg(parameter.output_column_index));
            return false;
        }

        quint64 raw_bits = 0;
        for (const CompiledSegmentPlan &segment : parameter.segments) {
            quint64 segment_value = 0;
            QString local_error;
            if (!readCompiledSegment(frame_data, frame_size, segment, &segment_value, &local_error)) {
                setError(error_message,
                         QStringLiteral("Parameter %1/%2 segment read failed: %3")
                             .arg(parameter.parameter_id, parameter.parameter_name, local_error));
                return false;
            }

            raw_bits |= (segment_value & segment.source_mask) << segment.computed_object_shift;
        }

        QByteArray text_value;
        CompiledParameterPlan raw_parameter = parameter;
        raw_parameter.calibration.type = CalibrationType::None;
        QByteArray raw_text;
        QString local_error;
        if (!convertCompiledValueToText(raw_bits, parameter.bit_count, raw_parameter, &raw_text, &local_error)) {
            setError(error_message, local_error);
            return false;
        }

        if (!convertCompiledValueToText(raw_bits,
                                        parameter.bit_count,
                                        parameter,
                                        &text_value,
                                        &local_error)) {
            setError(error_message,
                     QStringLiteral("Parameter %1/%2 conversion failed: %3")
                         .arg(parameter.parameter_id, parameter.parameter_name, local_error));
            return false;
        }

        if (include_raw_values) {
            (*row_values)[parameter.output_column_index * 2] = raw_text;
            (*row_values)[(parameter.output_column_index * 2) + 1] = text_value;
        } else {
            (*row_values)[parameter.output_column_index] = text_value;
        }
        if (produced_value_count != nullptr) {
            ++(*produced_value_count);
        }
    }

    return true;
}

bool FastFixedPcmProcessor::processAndWriteBlocks(const QList<StreamBlock> &stream_blocks,
                                                  ResultWriter *writer,
                                                  quint64 *processed_frame_count,
                                                  quint64 *produced_value_count,
                                                  QString *error_message) const
{
    if (writer == nullptr) {
        setError(error_message, QStringLiteral("result writer pointer is null"));
        return false;
    }

    const int worker_count = options_.worker_count > 0
        ? options_.worker_count
        : qMax(1, QThread::idealThreadCount() - 1);

    QList<QFuture<ChunkResult>> futures;
    for (const StreamBlock &stream_block : stream_blocks) {
        if (stream_block.frame_count == 0) {
            continue;
        }

        const int chunk_count = qMin<int>(qMax(1, worker_count), static_cast<int>(stream_block.frame_count));
        const quint64 chunk_frames = (stream_block.frame_count + static_cast<quint64>(chunk_count) - 1) /
            static_cast<quint64>(chunk_count);

        for (int chunk_index = 0; chunk_index < chunk_count; ++chunk_index) {
            const quint64 frame_offset = static_cast<quint64>(chunk_index) * chunk_frames;
            if (frame_offset >= stream_block.frame_count) {
                break;
            }

            const quint64 current_frame_count = qMin(chunk_frames, stream_block.frame_count - frame_offset);
            futures.append(QtConcurrent::run([this, stream_block, frame_offset, current_frame_count]() {
                return processStreamFrameRange(stream_block.block_data,
                                               stream_block.plan,
                                               stream_block.first_frame_index + frame_offset,
                                               frame_offset,
                                               current_frame_count);
            }));
        }
    }

    QList<ParameterValue> block_values;
    quint64 block_processed_frames = 0;
    quint64 block_produced_values = 0;

    for (QFuture<ChunkResult> &future : futures) {
        future.waitForFinished();
        const ChunkResult result = future.result();
        if (!result.error_message.isEmpty()) {
            setError(error_message, result.error_message);
            return false;
        }

        block_values.append(result.values);
        block_processed_frames += result.processed_frame_count;
        block_produced_values += result.produced_value_count;
    }

    std::stable_sort(block_values.begin(), block_values.end(), parameterValueLess);
    if (!writer->appendDecodedBlock(task_.definition.task_id, block_values, error_message)) {
        return false;
    }

    if (processed_frame_count != nullptr) {
        *processed_frame_count += block_processed_frames;
    }
    if (produced_value_count != nullptr) {
        *produced_value_count += block_produced_values;
    }
    return true;
}

bool FastFixedPcmProcessor::processAndWriteTxtBlocks(const QList<StreamBlock> &stream_blocks,
                                                     QSaveFile *output_file,
                                                     const QList<FastOutputColumn> &columns,
                                                     quint64 *processed_frame_count,
                                                     quint64 *produced_value_count,
                                                     qint64 *written_text_bytes,
                                                     QString *error_message) const
{
    if (output_file == nullptr) {
        setError(error_message, QStringLiteral("fast text output file pointer is null"));
        return false;
    }
    if (stream_blocks.isEmpty()) {
        return true;
    }

    bool synchronous_timestamps = true;
    quint64 samples_per_frame = 0;
    quint64 max_frame_count = 0;
    for (const StreamBlock &stream_block : stream_blocks) {
        if (stream_block.frame_count == 0) {
            continue;
        }
        max_frame_count = qMax(max_frame_count, stream_block.frame_count);
        if (samples_per_frame == 0) {
            samples_per_frame = stream_block.plan.stream.samples_per_frame;
        } else if (samples_per_frame != stream_block.plan.stream.samples_per_frame) {
            synchronous_timestamps = false;
        }
    }

    if (max_frame_count == 0) {
        return true;
    }

    if (!synchronous_timestamps) {
        return processMergedTxtBlocks(stream_blocks,
                                      output_file,
                                      columns,
                                      processed_frame_count,
                                      produced_value_count,
                                      written_text_bytes,
                                      error_message);
    }

    const int worker_count = options_.worker_count > 0
        ? options_.worker_count
        : qMax(1, QThread::idealThreadCount() - 1);
    const int chunk_count = qMin<int>(qMax(1, worker_count), static_cast<int>(max_frame_count));
    const quint64 chunk_frames = (max_frame_count + static_cast<quint64>(chunk_count) - 1) /
        static_cast<quint64>(chunk_count);

    QList<QFuture<FastTxtBlock>> futures;
    for (int chunk_index = 0; chunk_index < chunk_count; ++chunk_index) {
        const quint64 frame_offset = static_cast<quint64>(chunk_index) * chunk_frames;
        if (frame_offset >= max_frame_count) {
            break;
        }

        const quint64 current_frame_count = qMin(chunk_frames, max_frame_count - frame_offset);
        futures.append(QtConcurrent::run([this, stream_blocks, columns, frame_offset, current_frame_count]() {
            return processSynchronousTxtFrameRange(stream_blocks, columns, frame_offset, current_frame_count);
        }));
    }

    quint64 block_processed_frames = 0;
    quint64 block_produced_values = 0;
    for (QFuture<FastTxtBlock> &future : futures) {
        future.waitForFinished();
        const FastTxtBlock block = future.result();
        if (!block.error_message.isEmpty()) {
            setError(error_message, block.error_message);
            return false;
        }

        if (!block.data.isEmpty() && output_file->write(block.data) != block.data.size()) {
            setError(error_message,
                     QStringLiteral("Failed while writing fast result file '%1': %2")
                         .arg(output_file->fileName(), output_file->errorString()));
            return false;
        }

        block_processed_frames += block.processed_frame_count;
        block_produced_values += block.produced_value_count;
        if (written_text_bytes != nullptr) {
            *written_text_bytes += block.data.size();
        }
    }

    if (processed_frame_count != nullptr) {
        *processed_frame_count += block_processed_frames;
    }
    if (produced_value_count != nullptr) {
        *produced_value_count += block_produced_values;
    }
    return true;
}

FastFixedPcmProcessor::FastTxtBlock FastFixedPcmProcessor::processSynchronousTxtFrameRange(
    const QList<StreamBlock> &stream_blocks,
    const QList<FastOutputColumn> &columns,
    quint64 first_frame_offset,
    quint64 frame_count) const
{
    FastTxtBlock result;
    const QByteArray missing_text = task_.writer_options.missing_value_text.toUtf8();
    const qsizetype estimated_row_bytes = 24 + (columns.size() * 12);
    result.data.reserve(static_cast<int>(qMin<quint64>(frame_count * estimated_row_bytes,
                                                       static_cast<quint64>(std::numeric_limits<int>::max()))));

    for (quint64 index = 0; index < frame_count; ++index) {
        const quint64 frame_offset = first_frame_offset + index;
        bool has_frame = false;
        double timestamp_seconds = 0.0;
        QList<QByteArray> row_values;
        const int physical_column_count = columns.size() * (task_.writer_options.include_raw_values ? 2 : 1);
        row_values.reserve(physical_column_count);
        for (int column_index = 0; column_index < physical_column_count; ++column_index) {
            row_values.append(missing_text);
        }

        for (const StreamBlock &stream_block : stream_blocks) {
            if (frame_offset >= stream_block.frame_count) {
                continue;
            }

            const quint64 frame_index = stream_block.first_frame_index + frame_offset;
            const qint64 block_offset = static_cast<qint64>(frame_offset) * stream_block.plan.frame_bytes;
            const char *frame_data = stream_block.block_data.constData() + block_offset;
            if (!has_frame) {
                timestamp_seconds = task_.definition.user_sample_rate_hz > 0.0
                    ? static_cast<double>(frame_index * stream_block.plan.stream.samples_per_frame) /
                        task_.definition.user_sample_rate_hz
                    : 0.0;
                has_frame = true;
            }

            QString local_error;
            if (!decodeFrameToTextRow(frame_data,
                                      stream_block.plan.frame_bytes,
                                      stream_block.plan.decode_plan,
                                      task_.writer_options.include_raw_values,
                                      &row_values,
                                      &result.produced_value_count,
                                      &local_error)) {
                result.error_message = local_error;
                return result;
            }
            ++result.processed_frame_count;
        }

        if (has_frame) {
            appendTextRow(&result.data, formatTimestamp(timestamp_seconds, task_.writer_options), row_values);
        }
    }

    return result;
}

bool FastFixedPcmProcessor::processMergedTxtBlocks(const QList<StreamBlock> &stream_blocks,
                                                   QSaveFile *output_file,
                                                   const QList<FastOutputColumn> &columns,
                                                   quint64 *processed_frame_count,
                                                   quint64 *produced_value_count,
                                                   qint64 *written_text_bytes,
                                                   QString *error_message) const
{
    QList<quint64> frame_offsets;
    frame_offsets.reserve(stream_blocks.size());
    for (int index = 0; index < stream_blocks.size(); ++index) {
        frame_offsets.append(0);
    }

    const QByteArray missing_text = task_.writer_options.missing_value_text.toUtf8();
    QByteArray output_buffer;
    output_buffer.reserve(8 * 1024 * 1024);
    quint64 block_processed_frames = 0;
    quint64 block_produced_values = 0;

    auto flush_output = [&]() -> bool {
        if (output_buffer.isEmpty()) {
            return true;
        }
        if (output_file->write(output_buffer) != output_buffer.size()) {
            setError(error_message,
                     QStringLiteral("Failed while writing fast result file '%1': %2")
                         .arg(output_file->fileName(), output_file->errorString()));
            return false;
        }
        if (written_text_bytes != nullptr) {
            *written_text_bytes += output_buffer.size();
        }
        output_buffer.clear();
        return true;
    };

    while (!stop_requested_.loadAcquire()) {
        int minimum_stream_index = -1;
        double minimum_timestamp = 0.0;
        for (int stream_index = 0; stream_index < stream_blocks.size(); ++stream_index) {
            const StreamBlock &stream_block = stream_blocks.at(stream_index);
            if (frame_offsets.at(stream_index) >= stream_block.frame_count) {
                continue;
            }

            const quint64 frame_index = stream_block.first_frame_index + frame_offsets.at(stream_index);
            const double timestamp_seconds = task_.definition.user_sample_rate_hz > 0.0
                ? static_cast<double>(frame_index * stream_block.plan.stream.samples_per_frame) /
                    task_.definition.user_sample_rate_hz
                : 0.0;
            if (minimum_stream_index < 0 || timestamp_seconds < minimum_timestamp) {
                minimum_stream_index = stream_index;
                minimum_timestamp = timestamp_seconds;
            }
        }

        if (minimum_stream_index < 0) {
            break;
        }

        const QByteArray timestamp_text = formatTimestamp(minimum_timestamp, task_.writer_options);
        QList<QByteArray> row_values;
        const int physical_column_count = columns.size() * (task_.writer_options.include_raw_values ? 2 : 1);
        row_values.reserve(physical_column_count);
        for (int column_index = 0; column_index < physical_column_count; ++column_index) {
            row_values.append(missing_text);
        }

        for (int stream_index = 0; stream_index < stream_blocks.size(); ++stream_index) {
            const StreamBlock &stream_block = stream_blocks.at(stream_index);
            while (frame_offsets[stream_index] < stream_block.frame_count) {
                const quint64 frame_index = stream_block.first_frame_index + frame_offsets.at(stream_index);
                const double timestamp_seconds = task_.definition.user_sample_rate_hz > 0.0
                    ? static_cast<double>(frame_index * stream_block.plan.stream.samples_per_frame) /
                        task_.definition.user_sample_rate_hz
                    : 0.0;
                if (formatTimestamp(timestamp_seconds, task_.writer_options) != timestamp_text) {
                    break;
                }

                const qint64 block_offset =
                    static_cast<qint64>(frame_offsets.at(stream_index)) * stream_block.plan.frame_bytes;
                const char *frame_data = stream_block.block_data.constData() + block_offset;

                QString local_error;
                if (!decodeFrameToTextRow(frame_data,
                                          stream_block.plan.frame_bytes,
                                          stream_block.plan.decode_plan,
                                          task_.writer_options.include_raw_values,
                                          &row_values,
                                          &block_produced_values,
                                          &local_error)) {
                    setError(error_message, local_error);
                    return false;
                }

                ++block_processed_frames;
                ++frame_offsets[stream_index];
            }
        }

        appendTextRow(&output_buffer, timestamp_text, row_values);
        if (output_buffer.size() >= 8 * 1024 * 1024 && !flush_output()) {
            return false;
        }
    }

    if (!flush_output()) {
        return false;
    }

    if (processed_frame_count != nullptr) {
        *processed_frame_count += block_processed_frames;
    }
    if (produced_value_count != nullptr) {
        *produced_value_count += block_produced_values;
    }
    return true;
}

void FastFixedPcmProcessor::log(LogLevel level, const QString &message) const
{
    Logger::instance().log(level,
                           QStringLiteral("FastFixedPcmProcessor"),
                           task_.definition.task_id,
                           QString(),
                           message);
}

qint64 FastFixedPcmProcessor::syncUnitByteCount(const StreamLayoutTable &layout_table)
{
    const qint64 sync_unit_bits =
        static_cast<qint64>(layout_table.frame_length_words) * layout_table.word_length_bits;
    if (sync_unit_bits <= 0 || (sync_unit_bits % 8) != 0) {
        return -1;
    }

    return sync_unit_bits / 8;
}

bool FastFixedPcmProcessor::prepareOutputColumns(ExecutionPlan *plan,
                                                 const TaskDefinition &definition,
                                                 QList<FastOutputColumn> *columns,
                                                 QString *error_message)
{
    if (plan == nullptr || columns == nullptr) {
        setError(error_message, QStringLiteral("output column preparation received a null pointer"));
        return false;
    }

    columns->clear();
    QHash<int, int> stream_plan_index_by_stream_index;
    for (int index = 0; index < plan->streams.size(); ++index) {
        stream_plan_index_by_stream_index.insert(plan->streams.at(index).stream.stream_index, index);
    }

    QHash<QString, int> column_index_by_key;
    auto append_column = [&](int stream_index,
                             const QString &parameter_id,
                             const QString &parameter_name) -> int {
        const QString key = makeColumnKey(stream_index, parameter_id, parameter_name);
        if (key.isEmpty()) {
            return -1;
        }

        const auto iterator = column_index_by_key.constFind(key);
        if (iterator != column_index_by_key.constEnd()) {
            return iterator.value();
        }

        FastOutputColumn column;
        column.stream_index = stream_index;
        column.parameter_id = parameter_id.trimmed();
        column.parameter_name = parameter_name.trimmed();
        column.header_text = makeOutputTitle(column.parameter_id, column.parameter_name).toUtf8();
        column_index_by_key.insert(key, columns->size());
        columns->append(column);
        return columns->size() - 1;
    };

    for (const ParameterGroupSelection &group : definition.parameter_groups) {
        const int stream_plan_index = stream_plan_index_by_stream_index.value(group.stream_index, -1);
        if (stream_plan_index < 0) {
            continue;
        }

        const StreamPlan &stream_plan = plan->streams.at(stream_plan_index);
        for (const QString &requested_parameter : group.parameter_ids) {
            const StreamLayoutEntry *entry = stream_plan.layout_table.findById(requested_parameter);
            if (entry == nullptr) {
                entry = stream_plan.layout_table.findByName(requested_parameter);
            }

            if (entry == nullptr) {
                append_column(group.stream_index, requested_parameter, requested_parameter);
                continue;
            }

            append_column(group.stream_index, entry->parameter_id, entry->parameter_name);
        }
    }

    for (StreamPlan &stream_plan : plan->streams) {
        for (CompiledParameterPlan &parameter : stream_plan.decode_plan.parameters) {
            const int column_index = append_column(stream_plan.stream.stream_index,
                                                   parameter.parameter_id,
                                                   parameter.parameter_name);
            if (column_index < 0) {
                setError(error_message,
                         QStringLiteral("Parameter %1/%2 cannot be assigned to an output column")
                             .arg(parameter.parameter_id, parameter.parameter_name));
                return false;
            }

            parameter.output_column_index = column_index;
        }
    }

    if (columns->isEmpty()) {
        setError(error_message, QStringLiteral("fixed fast TXT output has no columns"));
        return false;
    }

    return true;
}

QByteArray FastFixedPcmProcessor::formatTimestamp(double timestamp_seconds, const ResultWriterOptions &options)
{
    const int precision = qBound(0, options.timestamp_precision, 15);
    return QString::number(timestamp_seconds, 'f', precision).toLatin1();
}

bool FastFixedPcmProcessor::openFastTextOutput(const TaskDefinition &definition,
                                               const ResultWriterOptions &options,
                                               const QList<FastOutputColumn> &columns,
                                               QSaveFile *output_file,
                                               QString *output_path,
                                               QString *error_message)
{
    if (output_file == nullptr || output_path == nullptr) {
        setError(error_message, QStringLiteral("fast text output received a null pointer"));
        return false;
    }

    const QString resolved_output_path = !options.output_file_path.trimmed().isEmpty()
        ? QFileInfo(options.output_file_path).absoluteFilePath()
        : QDir(definition.output_directory.trimmed().isEmpty() ? QDir::currentPath() : definition.output_directory)
              .absoluteFilePath(safeFileName(definition.task_id) + options.file_name_suffix);

    QFileInfo output_info(resolved_output_path);
    QDir output_dir(output_info.absolutePath());
    if (!output_dir.exists() && !output_dir.mkpath(QStringLiteral("."))) {
        setError(error_message,
                 QStringLiteral("Failed to create result output directory '%1'").arg(output_dir.absolutePath()));
        return false;
    }

    output_file->setFileName(resolved_output_path);
    if (!output_file->open(QIODevice::WriteOnly)) {
        setError(error_message,
                 QStringLiteral("Failed to open fast result file '%1': %2")
                     .arg(resolved_output_path, output_file->errorString()));
        return false;
    }

    QByteArray header = QByteArrayLiteral("timestamp_seconds");
    for (const FastOutputColumn &column : columns) {
        if (options.include_raw_values) {
            header.append('\t');
            header.append(column.header_text);
            header.append(QByteArrayLiteral("_raw"));
        }
        header.append('\t');
        header.append(column.header_text);
    }
    header.append('\n');

    if (output_file->write(header) != header.size()) {
        setError(error_message,
                 QStringLiteral("Failed to write fast result header '%1': %2")
                     .arg(resolved_output_path, output_file->errorString()));
        output_file->cancelWriting();
        return false;
    }

    *output_path = resolved_output_path;
    return true;
}

bool FastFixedPcmProcessor::blockHasFrameAt(const char *data,
                                            qsizetype available,
                                            const StreamPlan &stream_plan)
{
    if (data == nullptr || available < stream_plan.frame_bytes) {
        return false;
    }

    const qsizetype sync_size = stream_plan.stream.sync_word.size();
    if (available < sync_size) {
        return false;
    }

    if (sync_size == static_cast<qsizetype>(sizeof(quint32))) {
        quint32 left = 0;
        quint32 right = 0;
        std::memcpy(&left, data, sizeof(left));
        std::memcpy(&right, stream_plan.stream.sync_word.constData(), sizeof(right));
        return left == right;
    }

    if (sync_size == static_cast<qsizetype>(sizeof(quint64))) {
        quint64 left = 0;
        quint64 right = 0;
        std::memcpy(&left, data, sizeof(left));
        std::memcpy(&right, stream_plan.stream.sync_word.constData(), sizeof(right));
        return left == right;
    }

    return std::memcmp(data,
                       stream_plan.stream.sync_word.constData(),
                       static_cast<size_t>(sync_size)) == 0;
}

bool FastFixedPcmProcessor::findFirstFrameOffset(QFile *file,
                                                 const StreamPlan &stream_plan,
                                                 qint64 resync_window_bytes,
                                                 qint64 *offset,
                                                 QString *error_message)
{
    if (file == nullptr || offset == nullptr) {
        setError(error_message, QStringLiteral("fast path sync probe received a null pointer"));
        return false;
    }

    *offset = -1;
    const qint64 file_size = file->size();
    if (file_size < stream_plan.frame_bytes) {
        return true;
    }

    const qint64 requested_probe_bytes =
        resync_window_bytes > 0 ? resync_window_bytes : static_cast<qint64>(stream_plan.frame_bytes);
    const qint64 probe_bytes = qMin(file_size, qMax<qint64>(stream_plan.frame_bytes, requested_probe_bytes));
    if (probe_bytes > std::numeric_limits<int>::max()) {
        setError(error_message, QStringLiteral("fast path sync probe window is too large for QByteArray"));
        return false;
    }

    if (!file->seek(0)) {
        setError(error_message,
                 QStringLiteral("failed to seek raw file '%1' for fast-path sync probe")
                     .arg(stream_plan.raw_file_path));
        return false;
    }

    const QByteArray probe_data = file->read(static_cast<int>(probe_bytes));
    if (probe_data.size() != probe_bytes) {
        setError(error_message,
                 QStringLiteral("failed to read fast-path sync probe from '%1'")
                     .arg(stream_plan.raw_file_path));
        return false;
    }

    const qint64 last_offset = probe_data.size() - stream_plan.frame_bytes;
    for (qint64 candidate_offset = 0; candidate_offset <= last_offset; ++candidate_offset) {
        const char *candidate = probe_data.constData() + candidate_offset;
        const qsizetype available = probe_data.size() - static_cast<qsizetype>(candidate_offset);
        if (blockHasFrameAt(candidate, available, stream_plan)) {
            *offset = candidate_offset;
            return true;
        }
    }

    return true;
}

void FastFixedPcmProcessor::setError(QString *error_message, const QString &message)
{
    if (error_message != nullptr) {
        *error_message = message;
    }
}
