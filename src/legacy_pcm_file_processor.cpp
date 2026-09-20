#include "fast_fixed_pcm_processor.h"
#include "legacy_pcm_decoder.h"

#include <QElapsedTimer>
#include <QFile>
#include <QtConcurrent>
#include <algorithm>
#include <cstring>
#include <memory>
#include <vector>

namespace {
/** @brief 一个预读块；仅通过 QFuture 交接所有权，不跨线程访问 QFile。 */
struct LegacyReadChunk
{
    QByteArray bytes;
    QString error;
    bool eof = false;
};

/** @brief 一路流的双缓冲和单长周结果；下一块由线程池独立读取，其余状态仅在工作线程访问。 */
struct LegacySource
{
    QFile file;
    StreamDefinition stream;
    StreamLayoutTable layout;
    QSet<QString> selected_ids;
    LegacyPcmDecodePlan decoder;
    QByteArray buffer;
    QFuture<LegacyReadChunk> read_future;
    qint64 next_read_offset = 0;
    bool read_pending = false;
    int position = 0;
    bool eof = false;
    quint64 frame_index = 0; // 相对于本时间段起点的采样相位。
    QList<ParameterValue> pending;
    int value_index = 0;
    double last_timestamp = -1;
    double last_recorded_time = -1;

    /** @brief 退出、失败或取消时等待唯一预读结束，不遗留后台 I/O。 */
    ~LegacySource()
    {
        if (read_pending) {
            try {
                read_future.waitForFinished();
            } catch (...) {
                // 正常消费通过 result() 传播错误；退出时只负责回收已在执行的预读。
            }
        }
    }
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
    QElapsedTimer elapsed;
    elapsed.start();
    qint64 read_ns = 0, decode_ns = 0, write_ns = 0;
    const QString task_id = task_.definition.task_id;
    ResultWriter writer;
    writer.setOptions(task_.writer_options);
    QString error;
    const auto write_block = [&](const QList<ParameterValue> &values) {
        const qint64 begin = elapsed.nsecsElapsed();
        const bool ok = writer.appendDecodedBlock(task_id, values, &error);
        write_ns += elapsed.nsecsElapsed() - begin;
        return ok;
    };
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
        if (!source->decoder.prepare(layout, task_.definition.user_sample_rate_hz,
                                     source->selected_ids, task_.writer_options.include_raw_values, &error)) {
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
                                             32 * 1024 * 1024);
    // 每路最多一个在途预读。QFile 在预读线程内创建/关闭，双缓冲轮换复用容量。
    const auto schedule_read = [&](LegacySource &source, QByteArray spare_buffer) {
        const QString path = source.file.fileName();
        const qint64 offset = source.next_read_offset;
        const qint64 frame_bytes = qint64(source.layout.frame_length_words) * source.layout.subframe_depth * 2;
        const qint64 requested = qMax(block_size, frame_bytes);
        source.read_pending = true;
        source.read_future = QtConcurrent::run([path, offset, requested, buffer = std::move(spare_buffer)]() mutable {
            LegacyReadChunk chunk;
            QFile file(path);
            if (!file.open(QIODevice::ReadOnly) || !file.seek(offset)) {
                chunk.error = file.errorString();
                return chunk;
            }
            buffer.resize(int(requested));
            const qint64 count = file.read(buffer.data(), requested);
            if (count < 0 || (count == 0 && file.error() != QFile::NoError)) {
                chunk.error = file.errorString();
                return chunk;
            }
            buffer.resize(int(count));
            chunk.bytes = std::move(buffer);
            chunk.eof = file.atEnd();
            return chunk;
        });
    };
    for (auto &source : sources) {
        schedule_read(*source, QByteArray());
    }
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
                    const qint64 read_begin = elapsed.nsecsElapsed();
                    LegacyReadChunk chunk = source.read_future.result();
                    read_ns += elapsed.nsecsElapsed() - read_begin;
                    source.read_future = QFuture<LegacyReadChunk>();
                    source.read_pending = false;
                    if (!chunk.error.isEmpty()) {
                        error = chunk.error;
                        return false;
                    }
                    source.next_read_offset += chunk.bytes.size();
                    read_bytes += chunk.bytes.size();
                    source.eof = chunk.eof;
                    // 块边界未满一个长周时，将短尾拼到新块前面；随后交出旧缓冲继续预读。
                    if (!source.buffer.isEmpty()) {
                        chunk.bytes.prepend(source.buffer);
                    }
                    QByteArray spare_buffer = std::move(source.buffer);
                    source.buffer = std::move(chunk.bytes);
                    if (!source.eof && !stop_requested_.loadAcquire()) {
                        schedule_read(source, std::move(spare_buffer));
                    }
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
            // 解算同步完成后才移动/补充 buffer；帧视图不离开当前循环，无需复制原始字节。
            frame.frame_data = QByteArray::fromRawData(data, int(frame_size));
            source.position += int(frame_size);
            ++frames;
            double recorded_time = 0;
            if (!source.decoder.decodeFrameTime(frame, &recorded_time, &error)) {
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
            const qint64 decode_begin = elapsed.nsecsElapsed();
            const bool decoded = source.decoder.decode(frame, recorded_time, &source.pending, &error);
            decode_ns += elapsed.nsecsElapsed() - decode_begin;
            if (!decoded) {
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
        if (sources.size() == 1) {
            // 单流的长周结果已有序，直接整块写入，避免逐值归并和二次复制。
            LegacySource &source = *sources.front();
            if (!advance(source)) {
                failure();
                return;
            }
            if (source.pending.isEmpty()) {
                break;
            }
            if (!write_block(source.pending)) {
                failure();
                return;
            }
            value_count += quint64(source.pending.size());
            source.value_index = source.pending.size();
            continue;
        }
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
            if (!write_block(batch)) {
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
    if ((!batch.isEmpty() && !write_block(batch)) ||
        !writer.finishTask(task_id, &output_path, &error)) {
        failure();
        return;
    }
    log(LogLevel::Info, QStringLiteral("处理耗时 %1 ms：读取等待 %2 ms，参数解算 %3 ms，结果写入 %4 ms；%5 长周，%6 个值")
        .arg(elapsed.elapsed()).arg(read_ns / 1000000).arg(decode_ns / 1000000).arg(write_ns / 1000000)
        .arg(frames).arg(value_count));
    emit progressChanged(task_id, total_bytes, total_bytes);
    emit finished(task_id, output_path, frames, value_count);
}
