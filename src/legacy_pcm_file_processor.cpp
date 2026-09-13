#include "fast_fixed_pcm_processor.h"
#include "legacy_pcm_decoder.h"

#include <QElapsedTimer>
#include <QFile>
#include <algorithm>
#include <cstring>
#include <memory>
#include <vector>

namespace {
/** @brief 一路流的有界读缓冲与单长周结果；所有资源仅在当前工作线程访问。 */
struct LegacySource
{
    QFile file;
    StreamDefinition stream;
    StreamLayoutTable layout;
    QSet<QString> selected_ids;
    QByteArray buffer;
    int position = 0;
    bool eof = false;
    quint64 frame_index = 0; // 相对于本时间段起点的采样相位。
    QList<ParameterValue> pending;
    int value_index = 0;
    double last_timestamp = -1;
    double last_recorded_time = -1;
};

/** @brief 检查长周内各子帧同步字及 ID 顺序，避免从长周中部误解算时间字。 */
bool alignedFrame(const char *data, const LegacySource &source)
{
    const int subframe_bytes = source.layout.frame_length_words * 2;
    for (int index = 0; index < source.layout.subframe_depth; ++index) {
        const char *subframe = data + index * subframe_bytes;
        if (std::memcmp(subframe, source.stream.sync_word.constData(),
                        static_cast<size_t>(source.stream.sync_word.size())) != 0) {
            return false;
        }
        if (source.layout.id_position_word >= 0) {
            const uchar *id = reinterpret_cast<const uchar *>(subframe) + source.layout.id_position_word * 2;
            const quint16 value = id[0] | (quint16(id[1]) << 8);
            int mask = 1;
            while (mask < source.layout.subframe_depth - 1) {
                mask = (mask << 1) | 1;
            }
            if (((value >> source.layout.id_lsb) & mask) != index) {
                return false;
            }
        }
    }
    return true;
}
}

void FastFixedPcmProcessor::processLegacy()
{
    const QString task_id = task_.definition.task_id;
    ResultWriter writer;
    writer.setOptions(task_.writer_options);
    QString error;
    std::vector<std::unique_ptr<LegacySource>> sources;
    qint64 total_bytes = 0;
    qint64 read_bytes = 0;
    quint64 frames = 0;
    quint64 value_count = 0;
    auto failure = [&]() {
        writer.abortTask(task_id);
        log(LogLevel::Error, error);
        emit failed(task_id, error);
    };
    for (const StreamLayoutTable &layout : task_.layout_tables) {
        if (!task_.selected_parameter_ids_by_stream.isEmpty() &&
            !task_.selected_parameter_ids_by_stream.contains(layout.stream_id)) {
            continue;
        }
        auto source = std::make_unique<LegacySource>();
        source->layout = layout;
        source->selected_ids = task_.selected_parameter_ids_by_stream.value(layout.stream_id);
        if (!LegacyPcmDecoder::validate(layout, task_.definition.user_sample_rate_hz,
                                       source->selected_ids, &error)) {
            failure();
            return;
        }
        for (const StreamDefinition &stream : task_.streams) {
            if (stream.stream_id == layout.stream_id) {
                source->stream = stream;
            }
        }
        if (source->stream.sync_word.isEmpty() ||
            source->stream.sync_word.size() > layout.frame_length_words * 2) {
            error = QStringLiteral("流 %1 的子帧同步字无效").arg(layout.stream_index);
            failure();
            return;
        }
        source->file.setFileName(rawFilePathForStream(task_.definition.raw_file_path, layout.stream_index));
        if (!source->file.open(QIODevice::ReadOnly)) {
            error = source->file.errorString();
            failure();
            return;
        }
        total_bytes += source->file.size();
        sources.push_back(std::move(source));
    }
    if (sources.empty()) {
        error = QStringLiteral("没有可输出的数据流");
        failure();
        return;
    }
    if (!writer.startStreamingTask(task_.definition, task_.layout_tables, &error)) {
        failure();
        return;
    }
    emit started(task_id);
    QElapsedTimer progress_timer;
    progress_timer.start();
    const qint64 block_size = qBound<qint64>(4096, options_.block_size_bytes / qint64(sources.size()),
                                             256 * 1024 * 1024);
    // 每路只保存当前长周的结果；最低时间戳优先输出，避免任务级结果积压。
    auto advance = [&](LegacySource &source) {
        if (source.value_index < source.pending.size()) {
            return true;
        }
        source.pending.clear();
        source.value_index = 0;
        qint64 skipped = 0;
        const qint64 frame_size = qint64(source.layout.frame_length_words) * source.layout.subframe_depth * 2;
        while (!stop_requested_.loadAcquire()) {
            if (source.buffer.size() - source.position < frame_size) {
                source.buffer.remove(0, source.position);
                source.position = 0;
                if (!source.eof) {
                    const QByteArray chunk = source.file.read(qMax(block_size, frame_size - source.buffer.size()));
                    if (chunk.isEmpty() && source.file.error() != QFile::NoError) {
                        error = source.file.errorString();
                        return false;
                    }
                    read_bytes += chunk.size();
                    source.buffer.append(chunk);
                    source.eof = source.file.atEnd();
                    if (progress_timer.elapsed() >= 100) {
                        emit progressChanged(task_id, read_bytes, total_bytes);
                        progress_timer.restart();
                    }
                    continue;
                }
                if (!source.buffer.isEmpty()) {
                    log(LogLevel::Warning, QStringLiteral("流 %1 丢弃末尾不完整长周：%2 字节")
                        .arg(source.layout.stream_index).arg(source.buffer.size()));
                    source.buffer.clear();
                }
                return true;
            }
            const char *data = source.buffer.constData() + source.position;
            if (!alignedFrame(data, source)) {
                ++source.position;
                if (++skipped > options_.resync_window_bytes) {
                    error = QStringLiteral("流 %1 丢失同步（lost sync），超过重同步窗口")
                        .arg(source.layout.stream_index);
                    return false;
                }
                continue;
            }
            if (skipped > 0) {
                log(LogLevel::Warning, QStringLiteral("流 %1 跳过 %2 字节后恢复长周同步")
                    .arg(source.layout.stream_index).arg(skipped));
                skipped = 0;
            }
            FrameTask frame;
            frame.task_id = task_id;
            frame.stream_id = source.layout.stream_id;
            frame.frame_index = source.frame_index;
            frame.output_sample_rate_hz = task_.definition.user_sample_rate_hz;
            frame.range_start_seconds = task_.definition.time_range.start_seconds;
            frame.range_end_seconds = task_.definition.time_range.end_seconds;
            frame.frame_data = QByteArray(data, int(frame_size));
            source.position += int(frame_size);
            ++frames;
            double recorded_time = 0;
            if (!LegacyPcmDecoder::decodeFrameTime(frame, source.layout, &recorded_time, &error)) {
                return false;
            }
            if (source.last_recorded_time > recorded_time) {
                error = QStringLiteral("流 %1 时间码倒退，无法按时间归并").arg(source.layout.stream_index);
                return false;
            }
            source.last_recorded_time = recorded_time;
            // 旧版先定位到起始时间对应的长周，再从该长周重新计算降采样步长。
            if (recorded_time < frame.range_start_seconds) {
                continue;
            }
            if (frame.range_end_seconds >= 0 && recorded_time >= frame.range_end_seconds) {
                source.eof = true;
                source.buffer.clear();
                source.position = 0;
                return true;
            }
            ++source.frame_index;
            if (!LegacyPcmDecoder::decode(frame, source.layout, source.selected_ids, &source.pending, &error)) {
                return false;
            }
            if (!source.pending.isEmpty()) {
                if (source.last_timestamp > source.pending.first().timestamp_seconds) {
                    error = QStringLiteral("流 %1 时间码倒退，无法按时间归并").arg(source.layout.stream_index);
                    return false;
                }
                source.last_timestamp = source.pending.last().timestamp_seconds;
                return true;
            }
        }
        return true;
    };
    QList<ParameterValue> batch;
    while (!stop_requested_.loadAcquire()) {
        LegacySource *next = nullptr;
        for (auto &source : sources) {
            if (!advance(*source)) {
                failure();
                return;
            }
            if (source->value_index < source->pending.size() &&
                (next == nullptr || source->pending.at(source->value_index).timestamp_seconds <
                                    next->pending.at(next->value_index).timestamp_seconds)) {
                next = source.get();
            }
        }
        if (next == nullptr) {
            break;
        }
        batch.append(next->pending.at(next->value_index++));
        ++value_count;
        if (batch.size() >= 1024) {
            if (!writer.appendDecodedBlock(task_id, batch, &error)) {
                failure();
                return;
            }
            batch.clear();
        }
    }
    if (stop_requested_.loadAcquire()) {
        writer.abortTask(task_id);
        emit stopped(task_id);
        return;
    }
    QString output_path;
    if ((!batch.isEmpty() && !writer.appendDecodedBlock(task_id, batch, &error)) ||
        !writer.finishTask(task_id, &output_path, &error)) {
        failure();
        return;
    }
    emit progressChanged(task_id, total_bytes, total_bytes);
    emit finished(task_id, output_path, frames, value_count);
}
