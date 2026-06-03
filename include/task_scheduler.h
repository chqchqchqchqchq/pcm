#pragma once

#include "config_types.h"
#include "logger.h"
#include "raw_file_reader.h"

#include <QHash>
#include <QMutex>
#include <QObject>
#include <QQueue>

#include <functional>

/**
 * @brief Coordinates task-level concurrency and owns the raw-reading pipeline for active tasks.
 *
 * TaskScheduler accepts user-facing TaskDefinition values, limits the number of simultaneously active tasks, creates
 * one RawFileReader/QThread pair per active task, and allocates independent FrameTask queues for every stream. The
 * scheduler intentionally exposes taskQueuesReady() as the hand-off point for future FrameProcessor and ResultWriter
 * components so frame consumption and output routing can be attached without changing RawFileReader.
 */
class TaskScheduler : public QObject
{
    Q_OBJECT

public:
    /**
     * @brief Runtime state emitted for GUI task lists and tests.
     */
    enum class TaskState
    {
        Queued,
        Running,
        Stopping,
        Finished,
        Failed,
        Stopped
    };
    Q_ENUM(TaskState)

    using StreamDefinitionProvider =
        std::function<bool(const TaskDefinition &definition, QList<StreamDefinition> *streams, QString *error_message)>;
    using ReadRangeResolver =
        std::function<bool(const TaskDefinition &definition, RawReadRange *range, QString *error_message)>;

    /**
     * @brief Creates a scheduler with one active task by default.
     */
    explicit TaskScheduler(QObject *parent = nullptr);

    /**
     * @brief Requests all active readers to stop and waits for their threads to exit.
     */
    ~TaskScheduler() override;

    /**
     * @brief Sets the task-level concurrency limit. Values below 1 are clamped to 1.
     */
    void setMaxActiveTasks(int max_active_tasks);

    /**
     * @brief Returns the configured task-level concurrency limit.
     */
    int maxActiveTasks() const;

    /**
     * @brief Returns the number of reader pipelines currently counted against the active-task limit.
     */
    int activeTaskCount() const;

    /**
     * @brief Returns the number of queued definitions waiting for an active slot.
     */
    int pendingTaskCount() const;

    /**
     * @brief Sets the per-stream FrameTask queue capacity. A value <= 0 means unbounded queues.
     */
    void setFrameQueueMaxSize(int max_queue_size);

    /**
     * @brief Replaces RawFileReader options used for subsequently started tasks.
     */
    void setReaderOptions(const RawFileReaderOptions &options);

    /**
     * @brief Registers stream definitions for a configuration file path.
     *
     * This is the minimal bridge until ConfigManager can provide stream sync words and frame lengths directly.
     */
    void registerConfigStreams(const QString &config_file_path, const QList<StreamDefinition> &streams);

    /**
     * @brief Removes registered stream definitions for one configuration file path.
     */
    void unregisterConfigStreams(const QString &config_file_path);

    /**
     * @brief Installs a callback used to resolve TaskDefinition::config_file_path into stream definitions.
     */
    void setStreamDefinitionProvider(const StreamDefinitionProvider &provider);

    /**
     * @brief Installs a callback used to convert a task time range into a raw byte range.
     *
     * Without a resolver, the scheduler reads the whole file because byte mapping depends on PCM packing rules.
     */
    void setReadRangeResolver(const ReadRangeResolver &resolver);

    /**
     * @brief Enqueues a task and starts it immediately when an active slot is available.
     * @return false only for local submission errors such as empty ids or duplicate queued/running ids.
     */
    bool submitTask(const TaskDefinition &definition, QString *error_message = nullptr);

    /**
     * @brief Requests cooperative stop for an active task, or removes a pending task before it starts.
     */
    bool requestStop(const QString &task_id);

    /**
     * @brief Requests cooperative stop for all active readers and drops all pending tasks as stopped.
     */
    void requestStopAll();

    /**
     * @brief Returns whether the id is currently pending or still owned by an active reader thread.
     */
    bool hasTask(const QString &task_id) const;

signals:
    /**
     * @brief Emitted when submitTask() accepts a definition.
     */
    void taskSubmitted(const QString &task_id);

    /**
     * @brief Emitted when a RawFileReader starts scanning the task file.
     */
    void taskStarted(const QString &task_id);

    /**
     * @brief Emitted before a reader thread starts so FrameProcessor/ResultWriter can attach to queues.
     */
    void taskQueuesReady(const TaskDefinition &definition,
                         const QList<StreamDefinition> &streams,
                         const StreamFrameQueues &frame_queues);

    /**
     * @brief Forwards per-task raw-reader progress.
     */
    void taskProgressChanged(const QString &task_id, qint64 processed_bytes, qint64 total_bytes);

    /**
     * @brief Emitted when task preparation or raw reading fails.
     */
    void taskFailed(const QString &task_id, const QString &reason);

    /**
     * @brief Emitted after a pending task is dropped or an active task exits due to requestStop().
     */
    void taskStopped(const QString &task_id);

    /**
     * @brief Emitted once a task's reader has closed all stream queues.
     */
    void taskFinished(const QString &task_id, quint64 produced_frame_count);

    /**
     * @brief Emitted when the scheduler has no pending or active task resources left.
     */
    void allTasksFinished();

    /**
     * @brief Emitted when a task changes state.
     */
    void stateChanged(const QString &task_id, TaskState state);

    /**
     * @brief Emitted whenever the counted active reader count changes.
     */
    void activeTaskCountChanged(int active_task_count);

private:
    /**
     * @brief Owns one active raw reader, its thread, and task-local queues until the thread exits.
     */
    struct ActiveTask;

    /**
     * @brief Performs cheap submit-time validation that does not touch files or parse configs.
     */
    bool validateTaskDefinition(const TaskDefinition &definition, QString *error_message) const;

    /**
     * @brief Resolves the task config path into RawFileReader stream definitions.
     */
    bool resolveStreamDefinitions(const TaskDefinition &definition,
                                  QList<StreamDefinition> *streams,
                                  QString *error_message) const;

    /**
     * @brief Resolves TaskDefinition::time_range into RawReadRange.
     */
    bool resolveReadRange(const TaskDefinition &definition, RawReadRange *range, QString *error_message) const;

    /**
     * @brief Builds the RawReadTask and allocates one independent queue per stream.
     */
    bool buildRawReadTask(const TaskDefinition &definition,
                          const QList<StreamDefinition> &streams,
                          const RawReadRange &range,
                          RawReadTask *raw_task,
                          StreamFrameQueues *frame_queues,
                          QString *error_message) const;

    /**
     * @brief Creates and starts a reader thread for one queued task definition.
     */
    bool startTask(const TaskDefinition &definition, QString *error_message);

    /**
     * @brief Starts as many pending tasks as the active-task limit allows.
     */
    void startNextTasks();

    /**
     * @brief Records reader failure and forwards it as a scheduler-level signal.
     */
    void handleReaderFailed(const QString &task_id, const QString &reason);

    /**
     * @brief Handles the reader terminal signal after queues are closed.
     */
    void handleReaderFinished(const QString &task_id, quint64 produced_frame_count);

    /**
     * @brief Releases scheduler bookkeeping once the QThread has fully exited.
     */
    void handleThreadFinished(const QString &task_id, ActiveTask *active_task);

    /**
     * @brief Emits allTasksFinished() exactly once for each non-empty batch.
     */
    void emitAllTasksFinishedIfIdle();

    /**
     * @brief Writes a structured scheduler log record.
     */
    void log(LogLevel level, const QString &task_id, const QString &message) const;

    /**
     * @brief Normalizes config paths for stable registration and lookup.
     */
    static QString normalizedConfigPath(const QString &path);

    mutable QMutex mutex_;
    int max_active_tasks_ = 1;
    int active_reader_count_ = 0;
    int frame_queue_max_size_ = 0;
    bool idle_emitted_ = true;
    RawFileReaderOptions reader_options_;
    QQueue<TaskDefinition> pending_tasks_;
    QHash<QString, ActiveTask *> active_tasks_;
    QHash<QString, QList<StreamDefinition>> config_streams_;
    StreamDefinitionProvider stream_definition_provider_;
    ReadRangeResolver read_range_resolver_;
};

Q_DECLARE_METATYPE(TaskScheduler::TaskState)
Q_DECLARE_METATYPE(StreamDefinition)
Q_DECLARE_METATYPE(QList<StreamDefinition>)
Q_DECLARE_METATYPE(RawReadRange)
