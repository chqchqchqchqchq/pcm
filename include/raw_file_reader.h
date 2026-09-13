#pragma once

#include "config_types.h"
#include "frame_task.h"
#include "logger.h"

#include <QAtomicInteger>
#include <QHash>
#include <QList>
#include <QObject>
#include <QString>

class QFile;

/**
 * @brief Describes one stream sync word and fixed PCM frame length.
 *
 * ConfigParser is expected to generate these definitions from the configuration file. sync_word must be unique and not
 * a prefix of another stream sync word, otherwise demultiplexing would be ambiguous.
 */
struct StreamDefinition
{
    int stream_index = 1;
    QString stream_id;
    QByteArray sync_word;
    qsizetype frame_length_bytes = 0;
    quint64 samples_per_frame = 1;
};

/**
 * @brief Byte range selected from the raw PCM file for the current task.
 *
 * A negative end offset means EOF. Time-range to byte-range conversion belongs to the task/config layer because it
 * depends on PCM packing rules that are not known to the raw reader.
 */
struct RawReadRange
{
    qint64 start_byte_offset = 0;
    qint64 end_byte_offset = -1;
};

/**
 * @brief Task-local input and output channels for RawFileReader.
 */
struct RawReadTask
{
    QString task_id;
    QString raw_file_path;
    QHash<QString, QString> raw_file_paths_by_stream;
    double sample_rate_hz = 0.0;
    TaskTimeRange time_range;
    RawReadRange range;
    QList<StreamDefinition> streams;
    QHash<QString, FrameTaskQueuePtr> frame_queues;
};

/**
 * @brief Resolves the raw file path for one stream from the user-selected stream-1 file.
 *
 * Stream 1 keeps the original path. Stream N appends "_N" before the extension, for example
 * data.pcm -> data_2.pcm. Files without an extension become data_2.
 */
QString rawFilePathForStream(const QString &stream1_file_path, int stream_index);

/**
 * @brief Controls sliding-window mapping and tail handling.
 */
struct RawFileReaderOptions
{
    enum class ProcessingMode
    {
        Auto,
        FastFixed,
        ScanOnly
    };

    qint64 segment_size_bytes = 64 * 1024 * 1024;
    qint64 block_size_bytes = 8 * 1024 * 1024;
    qint64 resync_window_bytes = 1024 * 1024;
    ProcessingMode processing_mode = ProcessingMode::Auto;
    bool enable_premap = true;
    int premap_segment_count = 1;
    qsizetype max_tail_bytes = 0;
};

Q_DECLARE_METATYPE(RawFileReaderOptions::ProcessingMode)

/**
 * @brief Per-active-task raw PCM reader and demultiplexer.
 *
 * One RawFileReader instance should be created for each active task and moved to its own QThread by TaskScheduler.
 * It is the single producer in the task's producer/consumer pipeline: it maps the raw file in sliding windows, keeps a
 * buffer_tail for frames/sync words split by segment boundaries, detects stream sync words, and pushes FrameTask values
 * into the corresponding stream queue.
 */
class RawFileReader : public QObject
{
    Q_OBJECT

public:
    explicit RawFileReader(QObject *parent = nullptr);
    RawFileReader(const RawReadTask &task, const RawFileReaderOptions &options, QObject *parent = nullptr);

    /**
     * @brief Replaces the task definition before readFrames() starts.
     */
    void setReadTask(const RawReadTask &task);

    /**
     * @brief Replaces sliding-window options before readFrames() starts.
     */
    void setOptions(const RawFileReaderOptions &options);

public slots:
    /**
     * @brief Runs the blocking read/demultiplex loop in the current thread.
     */
    void readFrames();

    /**
     * @brief Requests cooperative cancellation.
     */
    void requestStop();

signals:
    /**
     * @brief Emitted when the reader starts scanning a task file.
     */
    void started(const QString &task_id);

    /**
     * @brief Emitted after each mapped segment is processed.
     */
    void progressChanged(const QString &task_id, qint64 processed_bytes, qint64 total_bytes);

    /**
     * @brief Emitted when reading completes or is cancelled after queue close.
     */
    void finished(const QString &task_id, quint64 produced_frame_count);

    /**
     * @brief Emitted on validation, I/O, mapping, or queue errors.
     */
    void failed(const QString &task_id, const QString &reason);

private:
    struct MappedSegment;
    struct MatchResult;

    bool validateTask(QString *error_message) const;
    bool computeTailSize(QString *error_message);
    bool readSourceFile(const QString &file_path,
                        const QList<StreamDefinition> &streams,
                        qint64 progress_base_bytes,
                        qint64 progress_total_bytes,
                        qint64 *processed_bytes,
                        QString *error_message);
    bool mapSegment(QFile *file, qint64 offset, qint64 size, MappedSegment *segment);
    void releaseSegment(QFile *file, MappedSegment *segment);
    bool processSegment(const MappedSegment &segment,
                        const QList<StreamDefinition> &streams,
                        const QString &source_file_path,
                        bool is_final_segment);
    MatchResult matchAt(const char *data, qsizetype available, const QList<StreamDefinition> &streams) const;
    bool enqueueFrame(const StreamDefinition &stream,
                      const QString &source_file_path,
                      const char *frame_data,
                      qsizetype frame_length,
                      qint64 file_offset);
    /**
     * @brief Returns true once every stream in the current source has reached the exclusive end time.
     */
    bool sourceRangeComplete(const QList<StreamDefinition> &streams) const;
    void updateTail(const QByteArray &scan_data, qsizetype tail_start, bool is_final_segment);
    void closeQueues();
    void log(LogLevel level, const QString &message) const;

    RawReadTask task_;
    RawFileReaderOptions options_;
    QByteArray buffer_tail_;
    qsizetype tail_size_bytes_ = 0;
    QHash<QString, quint64> stream_frame_counts_;
    quint64 produced_frame_count_ = 0;
    QAtomicInteger<int> stop_requested_ = 0;
};
