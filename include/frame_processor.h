#pragma once

#include "config_types.h"
#include "frame_task.h"
#include "logger.h"
#include "thread_safe_queue.h"

#include <QAtomicInteger>
#include <QFuture>
#include <QHash>
#include <QList>
#include <QMetaType>
#include <QMutex>
#include <QObject>
#include <QSet>
#include <QSharedPointer>
#include <QString>
#include <QVariant>

/**
 * @brief Thread-safe queue carrying calibrated values from one task stream to ResultWriter.
 */
using ParameterValueQueue = ThreadSafeQueue<ParameterValue>;
using ParameterValueQueuePtr = QSharedPointer<ParameterValueQueue>;
using StreamParameterValueQueues = QHash<QString, ParameterValueQueuePtr>;

/**
 * @brief Runtime options for asynchronous frame consumption and value delivery.
 */
struct FrameProcessorOptions
{
    int consumer_count_per_stream = 1;
    int wait_timeout_ms = 50;
    int result_queue_max_size = 0;
    int value_batch_size = 512;
    int progress_interval_frames = 1024;
    bool enable_result_queues = true;
    bool emit_individual_values = true;
};

/**
 * @brief Task-local processing context that keeps stream inputs and outputs isolated.
 */
struct FrameProcessorTask
{
    QString task_id;
    QList<StreamLayoutTable> layout_tables;
    StreamFrameQueues frame_queues;
    StreamParameterValueQueues result_queues;
    QHash<QString, QSet<QString>> selected_parameter_ids_by_stream;
};

/**
 * @brief Consumes FrameTask queues asynchronously and emits calibrated ParameterValue records.
 *
 * A FrameProcessor instance owns one task context. Every stream has its own input FrameTaskQueue and output
 * ParameterValueQueue, so different tasks and streams never share mutable data. Raw bytes inside each byte-aligned PCM
 * word are interpreted as little-endian; source_bit still numbers logical bits MSB-first within that word. Active
 * ParameterWordSegment entries are assembled in array order from high bits to low bits; object_bit is retained only as
 * compatibility metadata for existing configuration files.
 */
class FrameProcessor : public QObject
{
    Q_OBJECT

public:
    explicit FrameProcessor(QObject *parent = nullptr);
    FrameProcessor(const FrameProcessorTask &task,
                   const FrameProcessorOptions &options,
                   QObject *parent = nullptr);
    ~FrameProcessor() override;

    /**
     * @brief Replaces the task context before start() is called.
     */
    void setProcessingTask(const FrameProcessorTask &task);

    /**
     * @brief Replaces runtime options before start() is called.
     */
    void setOptions(const FrameProcessorOptions &options);

    /**
     * @brief Returns the current per-stream result queues.
     */
    StreamParameterValueQueues resultQueues() const;

    /**
     * @brief Creates one ParameterValue output queue for every layout table stream.
     */
    static StreamParameterValueQueues createResultQueues(const QList<StreamLayoutTable> &layout_tables,
                                                         int max_queue_size = 0);

    /**
     * @brief Extracts one raw assembled integer from a frame according to the layout entry.
     */
    static bool extractRawBits(const QByteArray &frame_data,
                               const StreamLayoutTable &layout_table,
                               const StreamLayoutEntry &entry,
                               quint64 *raw_bits,
                               int *bit_count,
                               QString *error_message = nullptr);

    /**
     * @brief Converts assembled raw bits to a QVariant using code type, sign bit, inverse flag, and calibration.
     */
    static bool convertRawValue(quint64 raw_bits,
                                int bit_count,
                                const StreamLayoutEntry &entry,
                                QVariant *value,
                                QString *error_message = nullptr);

    /**
     * @brief Processes one frame into zero or more calibrated ParameterValue records.
     */
    static bool processFrame(const FrameTask &frame_task,
                             const StreamLayoutTable &layout_table,
                             QList<ParameterValue> *values,
                             QString *error_message = nullptr,
                             const QSet<QString> &selected_parameter_ids = QSet<QString>());

    /**
     * @brief Processes one frame from a caller-owned memory block without copying frame bytes.
     */
    static bool processFrameData(const char *frame_data,
                                 qsizetype frame_size,
                                 const QString &task_id,
                                 double timestamp_seconds,
                                 const StreamLayoutTable &layout_table,
                                 QList<ParameterValue> *values,
                                 QString *error_message = nullptr,
                                 const QSet<QString> &selected_parameter_ids = QSet<QString>());

    /**
     * @brief Blocks until all asynchronous consumers launched by start() have exited.
     */
    void waitForFinished();

public slots:
    /**
     * @brief Starts one or more asynchronous consumers for every stream queue.
     */
    void start();

    /**
     * @brief Requests cooperative cancellation and closes result queues to wake blocked writers.
     */
    void requestStop();

signals:
    /**
     * @brief Emitted once worker submission succeeds.
     */
    void started(const QString &task_id);

    /**
     * @brief Emitted when a stream consumer begins reading one stream queue.
     */
    void streamStarted(const QString &task_id, int stream_index, const QString &stream_id);

    /**
     * @brief Emitted after a calibrated parameter value is accepted by its stream result queue.
     */
    void parameterReady(const ParameterValue &value);

    /**
     * @brief Emitted with decoded values in batches to reduce cross-thread delivery overhead.
     */
    void parameterBatchReady(const QList<ParameterValue> &values);

    /**
     * @brief Emitted periodically after frames are consumed from one stream queue.
     */
    void progressChanged(const QString &task_id,
                         int stream_index,
                         const QString &stream_id,
                         quint64 processed_frame_count,
                         quint64 produced_value_count);

    /**
     * @brief Emitted when a validation or per-frame decode error is detected.
     */
    void failed(const QString &task_id, const QString &reason);

    /**
     * @brief Emitted once all workers for one stream have stopped and its result queue is closed.
     */
    void streamFinished(const QString &task_id,
                        int stream_index,
                        const QString &stream_id,
                        quint64 processed_frame_count,
                        quint64 produced_value_count);

    /**
     * @brief Emitted once every stream consumer for the task has exited.
     */
    void finished(const QString &task_id, quint64 processed_frame_count, quint64 produced_value_count);

private:
    struct StreamWorkerContext;

    void ensureResultQueues();
    bool validateTask(QString *error_message) const;
    void closeResultQueues();
    void processStream(const StreamWorkerContext &context);
    void noteWorkerFinished(const StreamWorkerContext &context,
                            quint64 processed_frame_count,
                            quint64 produced_value_count);
    void reportFailure(const QString &task_id, const QString &reason);
    void log(LogLevel level, const QString &task_id, const QString &parameter_name, const QString &message) const;

    static bool extractRawBitsInternal(const QByteArray &frame_data,
                                       const StreamLayoutTable &layout_table,
                                       const StreamLayoutEntry &entry,
                                       quint64 *raw_bits,
                                       int *bit_count,
                                       QString *error_message);
    static bool extractRawBitsFromData(const char *frame_data,
                                       qsizetype frame_size,
                                       const StreamLayoutTable &layout_table,
                                       const StreamLayoutEntry &entry,
                                       quint64 *raw_bits,
                                       int *bit_count,
                                       QString *error_message);
    static bool readBitsMsbFirst(const QByteArray &frame_data,
                                 qint64 start_bit,
                                 int bit_length,
                                 quint64 *value,
                                 QString *error_message);
    static bool readBitsMsbFirst(const char *frame_data,
                                 qint64 available_bits,
                                 qint64 start_bit,
                                 int bit_length,
                                 quint64 *value,
                                 QString *error_message);
    static bool decodeSignedInteger(quint64 raw_bits,
                                    int bit_count,
                                    int sign_bit,
                                    qlonglong *value,
                                    QString *error_message);
    static bool decodeBcd(quint64 raw_bits, int bit_count, qlonglong *value, QString *error_message);
    static bool decodeFloatingPoint(quint64 raw_bits, int bit_count, double *value, QString *error_message);
    static bool applyCalibration(double raw_value,
                                 const CalibrationDefinition &calibration,
                                 double *engineering_value,
                                 bool *calibration_applied,
                                 QString *error_message);
    static bool normalizeLayoutIndex(int configured_index,
                                     int item_count,
                                     int *normalized_index,
                                     QString *error_message);

    mutable QMutex mutex_;
    FrameProcessorTask task_;
    FrameProcessorOptions options_;
    QList<QFuture<void>> futures_;
    QHash<QString, int> active_stream_workers_;
    QHash<QString, quint64> stream_processed_frames_;
    QHash<QString, quint64> stream_produced_values_;
    int active_worker_count_ = 0;
    quint64 total_processed_frames_ = 0;
    quint64 total_produced_values_ = 0;
    bool running_ = false;
    QAtomicInteger<int> stop_requested_ = 0;
};

Q_DECLARE_METATYPE(ParameterValueQueuePtr)
Q_DECLARE_METATYPE(StreamParameterValueQueues)
Q_DECLARE_METATYPE(FrameProcessorOptions)
Q_DECLARE_METATYPE(FrameProcessorTask)
