#include "raw_file_reader.h"

#include <QDir>
#include <QFile>
#include <QFileInfo>

#include <algorithm>
#include <cstring>
#include <limits>
#include <utility>

struct RawFileReader::MappedSegment
{
    qint64 offset = 0;
    qint64 size = 0;
    uchar *data = nullptr;
    bool mapped = false;
    QByteArray fallback_data;
};

struct RawFileReader::MatchResult
{
    enum State
    {
        NoMatch,
        Partial,
        Complete
    };

    State state = NoMatch;
    const StreamDefinition *stream = nullptr;
};

QString rawFilePathForStream(const QString &stream1_file_path, int stream_index)
{
    const QFileInfo file_info(stream1_file_path);
    if (stream_index <= 1) {
        return file_info.absoluteFilePath();
    }

    const QString suffix = file_info.suffix();
    const QString base_name = suffix.isEmpty() ? file_info.fileName() : file_info.completeBaseName();
    const QString derived_name = suffix.isEmpty()
        ? QStringLiteral("%1_%2").arg(base_name).arg(stream_index)
        : QStringLiteral("%1_%2.%3").arg(base_name).arg(stream_index).arg(suffix);

    return QDir(file_info.absolutePath()).filePath(derived_name);
}

RawFileReader::RawFileReader(QObject *parent)
    : QObject(parent)
{
}

RawFileReader::RawFileReader(const RawReadTask &task,
                             const RawFileReaderOptions &options,
                             QObject *parent)
    : QObject(parent)
    , task_(task)
    , options_(options)
{
}

void RawFileReader::setReadTask(const RawReadTask &task)
{
    task_ = task;
}

void RawFileReader::setOptions(const RawFileReaderOptions &options)
{
    options_ = options;
}

void RawFileReader::readFrames()
{
    stop_requested_.storeRelease(0);
    buffer_tail_.clear();
    stream_frame_counts_.clear();
    produced_frame_count_ = 0;

    QString error_message;
    if (!validateTask(&error_message) || !computeTailSize(&error_message)) {
        log(LogLevel::Error, error_message);
        closeQueues();
        emit failed(task_.task_id, error_message);
        emit finished(task_.task_id, produced_frame_count_);
        return;
    }

    emit started(task_.task_id);

    struct SourceGroup
    {
        QString file_path;
        QList<StreamDefinition> streams;
        qint64 file_size = 0;
        qint64 read_start = 0;
        qint64 read_end = 0;
        qint64 total_bytes = 0;
    };

    QList<SourceGroup> source_groups;
    QHash<QString, int> source_index_by_path;
    for (const StreamDefinition &stream : task_.streams) {
        const QString configured_path = task_.raw_file_paths_by_stream.value(stream.stream_id).trimmed();
        const QString file_path = configured_path.isEmpty() ? task_.raw_file_path : configured_path;
        const QFileInfo file_info(file_path);
        const QString normalized_path = file_info.absoluteFilePath();

        int source_index = source_index_by_path.value(normalized_path, -1);
        if (source_index < 0) {
            SourceGroup source_group;
            source_group.file_path = normalized_path;
            source_index = source_groups.size();
            source_index_by_path.insert(normalized_path, source_index);
            source_groups.append(source_group);
        }

        source_groups[source_index].streams.append(stream);
    }

    qint64 total_bytes = 0;
    for (SourceGroup &source_group : source_groups) {
        QFile file(source_group.file_path);
        if (!file.open(QIODevice::ReadOnly)) {
            error_message = QStringLiteral("Failed to open raw PCM file '%1': %2")
                                .arg(source_group.file_path, file.errorString());
            log(LogLevel::Error, error_message);
            closeQueues();
            emit failed(task_.task_id, error_message);
            emit finished(task_.task_id, produced_frame_count_);
            return;
        }

        source_group.file_size = file.size();
        source_group.read_start = task_.range.start_byte_offset;
        source_group.read_end = task_.range.end_byte_offset < 0
            ? source_group.file_size
            : std::min(task_.range.end_byte_offset, source_group.file_size);

        if (source_group.read_start > source_group.read_end) {
            error_message = QStringLiteral("Invalid read range [%1, %2) for file '%3' size %4")
                                .arg(source_group.read_start)
                                .arg(source_group.read_end)
                                .arg(source_group.file_path)
                                .arg(source_group.file_size);
            log(LogLevel::Error, error_message);
            closeQueues();
            emit failed(task_.task_id, error_message);
            emit finished(task_.task_id, produced_frame_count_);
            return;
        }

        source_group.total_bytes = source_group.read_end - source_group.read_start;
        total_bytes += source_group.total_bytes;
    }

    log(LogLevel::Info,
        QStringLiteral("Raw file reader started, sources=%1, total_bytes=%2, segment_size=%3, premap=%4")
            .arg(source_groups.size())
            .arg(total_bytes)
            .arg(options_.segment_size_bytes)
            .arg(options_.enable_premap ? QStringLiteral("on") : QStringLiteral("off")));

    if (total_bytes == 0) {
        closeQueues();
        emit progressChanged(task_.task_id, 0, 0);
        emit finished(task_.task_id, produced_frame_count_);
        return;
    }

    qint64 processed_bytes = 0;
    bool failed_read = false;
    for (const SourceGroup &source_group : source_groups) {
        if (stop_requested_.loadAcquire()) {
            break;
        }

        if (!readSourceFile(source_group.file_path,
                            source_group.streams,
                            processed_bytes,
                            total_bytes,
                            &processed_bytes,
                            &error_message)) {
            failed_read = true;
            break;
        }
    }

    closeQueues();

    if (failed_read) {
        log(LogLevel::Error, error_message);
        emit failed(task_.task_id, error_message);
    } else if (stop_requested_.loadAcquire()) {
        log(LogLevel::Warning, QStringLiteral("Raw file reader stopped by request"));
    } else {
        log(LogLevel::Info,
            QStringLiteral("Raw file reader finished, produced_frames=%1").arg(produced_frame_count_));
    }

    emit finished(task_.task_id, produced_frame_count_);
}

void RawFileReader::requestStop()
{
    stop_requested_.storeRelease(1);
    closeQueues();
}

bool RawFileReader::validateTask(QString *error_message) const
{
    if (task_.task_id.trimmed().isEmpty()) {
        *error_message = QStringLiteral("RawReadTask.task_id is empty");
        return false;
    }

    if (task_.raw_file_path.trimmed().isEmpty()) {
        *error_message = QStringLiteral("RawReadTask.raw_file_path is empty");
        return false;
    }

    if (task_.range.start_byte_offset < 0) {
        *error_message = QStringLiteral("Read range start offset must be >= 0");
        return false;
    }

    if (task_.range.end_byte_offset >= 0 && task_.range.end_byte_offset < task_.range.start_byte_offset) {
        *error_message = QStringLiteral("Read range end offset must be >= start offset");
        return false;
    }

    if (task_.streams.isEmpty()) {
        *error_message = QStringLiteral("RawReadTask.streams is empty");
        return false;
    }

    if (options_.segment_size_bytes <= 0) {
        *error_message = QStringLiteral("RawFileReaderOptions.segment_size_bytes must be > 0");
        return false;
    }

    if (options_.segment_size_bytes > std::numeric_limits<int>::max()) {
        *error_message = QStringLiteral("segment_size_bytes must be <= %1 for bounded QByteArray assembly")
                             .arg(std::numeric_limits<int>::max());
        return false;
    }

    if (options_.premap_segment_count < 0) {
        *error_message = QStringLiteral("premap_segment_count must be >= 0");
        return false;
    }

    QHash<QString, QList<StreamDefinition>> streams_by_source_path;
    for (int i = 0; i < task_.streams.size(); ++i) {
        const StreamDefinition &stream = task_.streams.at(i);
        if (stream.stream_index <= 0) {
            *error_message = QStringLiteral("Stream '%1' has invalid stream_index %2")
                                 .arg(stream.stream_id)
                                 .arg(stream.stream_index);
            return false;
        }

        if (stream.stream_id.trimmed().isEmpty()) {
            *error_message = QStringLiteral("Stream at index %1 has an empty stream_id").arg(i);
            return false;
        }

        if (stream.sync_word.isEmpty()) {
            *error_message = QStringLiteral("Stream '%1' has an empty sync_word").arg(stream.stream_id);
            return false;
        }

        if (stream.frame_length_bytes < stream.sync_word.size()) {
            *error_message = QStringLiteral("Stream '%1' frame_length_bytes is shorter than sync_word")
                                 .arg(stream.stream_id);
            return false;
        }

        if (!task_.frame_queues.contains(stream.stream_id) || task_.frame_queues.value(stream.stream_id).isNull()) {
            *error_message = QStringLiteral("Stream '%1' has no output FrameTask queue").arg(stream.stream_id);
            return false;
        }

        const QString configured_path = task_.raw_file_paths_by_stream.value(stream.stream_id).trimmed();
        const QString file_path = configured_path.isEmpty() ? task_.raw_file_path : configured_path;
        streams_by_source_path[file_path].append(stream);

        for (int j = i + 1; j < task_.streams.size(); ++j) {
            const StreamDefinition &other = task_.streams.at(j);
            if (stream.stream_id == other.stream_id) {
                *error_message = QStringLiteral("Duplicate stream_id '%1'").arg(stream.stream_id);
                return false;
            }
        }
    }

    for (auto iterator = streams_by_source_path.constBegin();
         iterator != streams_by_source_path.constEnd();
         ++iterator) {
        const QList<StreamDefinition> source_streams = iterator.value();
        for (int i = 0; i < source_streams.size(); ++i) {
            const StreamDefinition &stream = source_streams.at(i);
            for (int j = i + 1; j < source_streams.size(); ++j) {
                const StreamDefinition &other = source_streams.at(j);
                const bool stream_is_prefix = other.sync_word.startsWith(stream.sync_word);
                const bool other_is_prefix = stream.sync_word.startsWith(other.sync_word);
                if (stream_is_prefix || other_is_prefix) {
                    *error_message = QStringLiteral("Ambiguous sync words between streams '%1' and '%2' in '%3'")
                                         .arg(stream.stream_id, other.stream_id, iterator.key());
                    return false;
                }
            }
        }
    }

    return true;
}

bool RawFileReader::computeTailSize(QString *error_message)
{
    qsizetype required_tail = 0;
    for (const StreamDefinition &stream : task_.streams) {
        required_tail = std::max(required_tail, stream.frame_length_bytes - 1);
        required_tail = std::max(required_tail, static_cast<qsizetype>(stream.sync_word.size() - 1));
    }

    if (options_.max_tail_bytes > 0 && options_.max_tail_bytes < required_tail) {
        *error_message = QStringLiteral("max_tail_bytes=%1 is too small; required at least %2")
                             .arg(options_.max_tail_bytes)
                             .arg(required_tail);
        return false;
    }

    tail_size_bytes_ = options_.max_tail_bytes > 0 ? options_.max_tail_bytes : required_tail;

    const qint64 max_scan_size = static_cast<qint64>(tail_size_bytes_) + options_.segment_size_bytes;
    if (max_scan_size > std::numeric_limits<int>::max()) {
        *error_message = QStringLiteral("segment_size_bytes + buffer_tail exceeds QByteArray safety limit");
        return false;
    }

    return true;
}

bool RawFileReader::readSourceFile(const QString &file_path,
                                   const QList<StreamDefinition> &streams,
                                   qint64 progress_base_bytes,
                                   qint64 progress_total_bytes,
                                   qint64 *processed_bytes,
                                   QString *error_message)
{
    if (streams.isEmpty()) {
        return true;
    }

    QFile file(file_path);
    if (!file.open(QIODevice::ReadOnly)) {
        *error_message = QStringLiteral("Failed to open raw PCM file '%1': %2")
                             .arg(file_path, file.errorString());
        return false;
    }

    const qint64 file_size = file.size();
    const qint64 read_start = task_.range.start_byte_offset;
    const qint64 read_end = task_.range.end_byte_offset < 0
        ? file_size
        : std::min(task_.range.end_byte_offset, file_size);

    if (read_start > read_end) {
        *error_message = QStringLiteral("Invalid read range [%1, %2) for file '%3' size %4")
                             .arg(read_start)
                             .arg(read_end)
                             .arg(file_path)
                             .arg(file_size);
        return false;
    }

    const qint64 source_total_bytes = read_end - read_start;
    if (source_total_bytes == 0) {
        if (processed_bytes != nullptr) {
            *processed_bytes = progress_base_bytes;
        }
        emit progressChanged(task_.task_id, progress_base_bytes, progress_total_bytes);
        return true;
    }

    log(LogLevel::Info,
        QStringLiteral("Reading source '%1', streams=%2, range=[%3, %4)")
            .arg(file_path)
            .arg(streams.size())
            .arg(read_start)
            .arg(read_end));

    buffer_tail_.clear();

    MappedSegment current_segment;
    MappedSegment next_segment;
    bool has_next_segment = false;
    bool failed_read = false;

    auto map_at = [&](qint64 offset, MappedSegment *segment) -> bool {
        const qint64 size = std::min(options_.segment_size_bytes, read_end - offset);
        return mapSegment(&file, offset, size, segment);
    };

    if (!map_at(read_start, &current_segment)) {
        *error_message = QStringLiteral("Failed to map or read first segment at offset %1 in '%2'")
                             .arg(read_start)
                             .arg(file_path);
        failed_read = true;
    }

    qint64 next_offset = current_segment.offset + current_segment.size;
    const bool use_premap = options_.enable_premap && options_.premap_segment_count > 0;
    if (!failed_read && use_premap && next_offset < read_end) {
        has_next_segment = map_at(next_offset, &next_segment);
        if (!has_next_segment) {
            *error_message = QStringLiteral("Failed to pre-map segment at offset %1 in '%2'")
                                 .arg(next_offset)
                                 .arg(file_path);
            failed_read = true;
        }
    }

    while (!failed_read && !stop_requested_.loadAcquire()) {
        const bool is_final_segment = current_segment.offset + current_segment.size >= read_end;
        if (!processSegment(current_segment, streams, file_path, is_final_segment)) {
            if (!stop_requested_.loadAcquire()) {
                *error_message = QStringLiteral("Failed while demultiplexing segment at offset %1 in '%2'")
                                     .arg(current_segment.offset)
                                     .arg(file_path);
                failed_read = true;
            }
            break;
        }

        const qint64 source_processed =
            std::min(current_segment.offset + current_segment.size, read_end) - read_start;
        releaseSegment(&file, &current_segment);
        emit progressChanged(task_.task_id, progress_base_bytes + source_processed, progress_total_bytes);

        if (is_final_segment || sourceRangeComplete(streams)) {
            break;
        }

        if (has_next_segment) {
            std::swap(current_segment, next_segment);
            has_next_segment = false;
        } else if (!map_at(next_offset, &current_segment)) {
            *error_message = QStringLiteral("Failed to map or read segment at offset %1 in '%2'")
                                 .arg(next_offset)
                                 .arg(file_path);
            failed_read = true;
            break;
        }

        next_offset = current_segment.offset + current_segment.size;
        if (use_premap && next_offset < read_end) {
            has_next_segment = map_at(next_offset, &next_segment);
            if (!has_next_segment) {
                *error_message = QStringLiteral("Failed to pre-map segment at offset %1 in '%2'")
                                     .arg(next_offset)
                                     .arg(file_path);
                failed_read = true;
                break;
            }
        }
    }

    releaseSegment(&file, &current_segment);
    releaseSegment(&file, &next_segment);
    buffer_tail_.clear();

    if (processed_bytes != nullptr) {
        *processed_bytes = progress_base_bytes + source_total_bytes;
    }

    return !failed_read || stop_requested_.loadAcquire();
}

bool RawFileReader::mapSegment(QFile *file, qint64 offset, qint64 size, MappedSegment *segment)
{
    segment->offset = offset;
    segment->size = size;
    segment->data = nullptr;
    segment->mapped = false;
    segment->fallback_data.clear();

    if (size <= 0) {
        return false;
    }

    uchar *mapped_data = file->map(offset, size);
    if (mapped_data != nullptr) {
        segment->data = mapped_data;
        segment->mapped = true;
        return true;
    }

    log(LogLevel::Warning,
        QStringLiteral("QFile::map failed at offset %1, size %2; falling back to QFile::read")
            .arg(offset)
            .arg(size));

    if (!file->seek(offset)) {
        return false;
    }

    segment->fallback_data = file->read(size);
    if (segment->fallback_data.size() != size) {
        return false;
    }

    segment->data = reinterpret_cast<uchar *>(segment->fallback_data.data());
    return true;
}

void RawFileReader::releaseSegment(QFile *file, MappedSegment *segment)
{
    if (segment->mapped && segment->data != nullptr) {
        file->unmap(segment->data);
    }

    segment->offset = 0;
    segment->size = 0;
    segment->data = nullptr;
    segment->mapped = false;
    segment->fallback_data.clear();
}

bool RawFileReader::processSegment(const MappedSegment &segment,
                                   const QList<StreamDefinition> &streams,
                                   const QString &source_file_path,
                                   bool is_final_segment)
{
    QByteArray scan_data;
    scan_data.reserve(buffer_tail_.size() + static_cast<int>(segment.size));
    scan_data.append(buffer_tail_);
    scan_data.append(reinterpret_cast<const char *>(segment.data), static_cast<int>(segment.size));

    const qint64 base_offset = segment.offset - buffer_tail_.size();
    qsizetype position = 0;
    qsizetype tail_start = scan_data.size();

    while (position < scan_data.size() && !stop_requested_.loadAcquire()) {
        const MatchResult match = matchAt(scan_data.constData() + position, scan_data.size() - position, streams);
        if (match.state == MatchResult::Complete) {
            if (!enqueueFrame(*match.stream,
                              source_file_path,
                              scan_data.constData() + position,
                              match.stream->frame_length_bytes,
                              base_offset + position)) {
                return false;
            }

            position += match.stream->frame_length_bytes;
            if (sourceRangeComplete(streams)) {
                buffer_tail_.clear();
                return true;
            }
            continue;
        }

        if (match.state == MatchResult::Partial) {
            tail_start = position;
            break;
        }

        ++position;
    }

    if (stop_requested_.loadAcquire()) {
        return true;
    }

    updateTail(scan_data, tail_start, is_final_segment);
    return true;
}

RawFileReader::MatchResult RawFileReader::matchAt(const char *data,
                                                  qsizetype available,
                                                  const QList<StreamDefinition> &streams) const
{
    MatchResult result;
    if (available <= 0) {
        return result;
    }

    bool has_partial_match = false;
    for (const StreamDefinition &stream : streams) {
        const qsizetype sync_length = stream.sync_word.size();
        const qsizetype compare_length = std::min(available, sync_length);

        if (std::memcmp(data, stream.sync_word.constData(), static_cast<size_t>(compare_length)) != 0) {
            continue;
        }

        if (available < sync_length || available < stream.frame_length_bytes) {
            has_partial_match = true;
            continue;
        }

        result.state = MatchResult::Complete;
        result.stream = &stream;
        return result;
    }

    if (has_partial_match) {
        result.state = MatchResult::Partial;
    }

    return result;
}

bool RawFileReader::sourceRangeComplete(const QList<StreamDefinition> &streams) const
{
    if (!task_.time_range.hasEnd() || task_.sample_rate_hz <= 0.0 || streams.isEmpty()) {
        return false;
    }
    for (const StreamDefinition &stream : streams) {
        const quint64 next_frame_index = stream_frame_counts_.value(stream.stream_id, 0);
        const long double next_timestamp = static_cast<long double>(next_frame_index) *
            static_cast<long double>(stream.samples_per_frame) / task_.sample_rate_hz;
        if (next_timestamp < task_.time_range.end_seconds) {
            return false;
        }
    }
    return true;
}

bool RawFileReader::enqueueFrame(const StreamDefinition &stream,
                                 const QString &source_file_path,
                                 const char *frame_data,
                                 qsizetype frame_length,
                                 qint64 file_offset)
{
    FrameTask frame_task;
    const quint64 frame_index = stream_frame_counts_.value(stream.stream_id, 0);

    frame_task.task_id = task_.task_id;
    frame_task.stream_id = stream.stream_id;
    frame_task.source_file_path = source_file_path;
    frame_task.frame_index = frame_index;
    frame_task.file_offset = file_offset;
    frame_task.sample_index = frame_index * stream.samples_per_frame;
    frame_task.timestamp_seconds = task_.sample_rate_hz > 0.0
        ? static_cast<double>(frame_task.sample_index) / task_.sample_rate_hz
        : 0.0;

    stream_frame_counts_.insert(stream.stream_id, frame_index + 1);
    if (frame_task.timestamp_seconds < task_.time_range.start_seconds) {
        return true;
    }
    if (task_.time_range.hasEnd() && frame_task.timestamp_seconds >= task_.time_range.end_seconds) {
        return true;
    }

    frame_task.frame_data = QByteArray(frame_data, static_cast<int>(frame_length));

    const FrameTaskQueuePtr queue = task_.frame_queues.value(stream.stream_id);
    if (!queue || !queue->push(frame_task)) {
        if (!stop_requested_.loadAcquire()) {
            log(LogLevel::Error, QStringLiteral("Frame queue for stream '%1' rejected a frame").arg(stream.stream_id));
        }
        return false;
    }

    ++produced_frame_count_;
    return true;
}

void RawFileReader::updateTail(const QByteArray &scan_data, qsizetype tail_start, bool is_final_segment)
{
    if (is_final_segment) {
        if (tail_start < scan_data.size()) {
            log(LogLevel::Warning,
                QStringLiteral("Dropping incomplete trailing frame/tail of %1 bytes at EOF")
                    .arg(scan_data.size() - tail_start));
        }

        buffer_tail_.clear();
        return;
    }

    if (tail_start >= scan_data.size()) {
        buffer_tail_.clear();
        return;
    }

    qsizetype tail_length = scan_data.size() - tail_start;
    if (tail_length > tail_size_bytes_) {
        log(LogLevel::Warning,
            QStringLiteral("Truncating buffer_tail from %1 bytes to %2 bytes")
                .arg(tail_length)
                .arg(tail_size_bytes_));
        tail_start = scan_data.size() - tail_size_bytes_;
        tail_length = tail_size_bytes_;
    }

    buffer_tail_ = scan_data.mid(tail_start, tail_length);
}

void RawFileReader::closeQueues()
{
    for (const StreamDefinition &stream : task_.streams) {
        const FrameTaskQueuePtr queue = task_.frame_queues.value(stream.stream_id);
        if (queue) {
            queue->close();
        }
    }
}

void RawFileReader::log(LogLevel level, const QString &message) const
{
    Logger::instance().log(level, QStringLiteral("RawFileReader"), task_.task_id, QString(), message);
}
