#pragma once

#include "config_types.h"
#include "frame_processor.h"
#include "result_writer.h"
#include "task_scheduler.h"
#include "traditional_processing_types.h"

#include <QHash>
#include <QObject>
#include <QSet>
#include <QString>

/**
 * @brief Connects the traditional single-task GUI request to the backend read/process/write pipeline.
 *
 * The controller owns one TaskScheduler and one ResultWriter, creates a task-local FrameProcessor when scheduler
 * queues are ready, forwards decoded ParameterValue records into ResultWriter, and emits GUI-friendly lifecycle
 * signals. It intentionally keeps the current traditional mode single-task so interruption and output ownership stay
 * deterministic.
 */
class TraditionalProcessingController : public QObject
{
    Q_OBJECT

public:
    /**
     * @brief Creates the controller and wires TaskScheduler lifecycle signals.
     */
    explicit TraditionalProcessingController(QObject *parent = nullptr);

    /**
     * @brief Stops any active pipeline before member backends are destroyed.
     */
    ~TraditionalProcessingController() override;

public slots:
    /**
     * @brief Parses the selected config and starts one traditional processing task.
     */
    void startProcessing(const TraditionalProcessingOptions &options);

    /**
     * @brief Requests cooperative stop for the current traditional task.
     */
    void requestStop();

signals:
    /**
     * @brief Emitted when the scheduler has started reading the raw file.
     */
    void taskStarted(const QString &task_id);

    /**
     * @brief Emitted as raw bytes and decoded frames progress through the chain.
     */
    void taskProgressChanged(const QString &task_id, int progress_percent, const QString &detail);

    /**
     * @brief Emitted when the task result file has been committed.
     */
    void taskFinished(const QString &task_id, const QString &output_path);

    /**
     * @brief Emitted once when the task cannot continue.
     */
    void taskFailed(const QString &task_id, const QString &reason);

    /**
     * @brief Emitted after a user stop request has drained active resources.
     */
    void taskStopped(const QString &task_id);

private:
    struct ActiveJob;

    bool loadConfig(const QString &file_path, Config *config, QString *error_message) const;
    bool buildStreamDefinitions(const Config &config,
                                QList<StreamDefinition> *streams,
                                QString *error_message) const;
    bool applyParameterSelection(const TraditionalProcessingOptions &options,
                                 const Config &config,
                                 TaskDefinition *definition,
                                 QHash<QString, QSet<QString>> *selected_parameter_ids_by_stream,
                                 QString *error_message) const;
    void handleTaskQueuesReady(const TaskDefinition &definition,
                               const QList<StreamDefinition> &streams,
                               const StreamFrameQueues &frame_queues);
    void handleParameterReady(const ParameterValue &value);
    void handleSchedulerTaskProgress(const QString &task_id, qint64 processed_bytes, qint64 total_bytes);
    void handleSchedulerTaskFailed(const QString &task_id, const QString &reason);
    void handleSchedulerTaskStopped(const QString &task_id);
    void handleSchedulerTaskFinished(const QString &task_id, quint64 produced_frame_count);
    void handleProcessorFailed(const QString &task_id, const QString &reason);
    void handleProcessorProgress(const QString &task_id,
                                 int stream_index,
                                 const QString &stream_id,
                                 quint64 processed_frame_count,
                                 quint64 produced_value_count);
    void handleProcessorFinished(const QString &task_id,
                                 quint64 processed_frame_count,
                                 quint64 produced_value_count);
    void failJob(const QString &task_id, const QString &reason, bool stop_scheduler = true);
    void finishJobIfReady(const QString &task_id);
    void cleanupJob(const QString &task_id);
    void log(LogLevel level, const QString &task_id, const QString &message) const;

    TaskScheduler scheduler_;
    ResultWriter writer_;
    QHash<QString, ActiveJob *> active_jobs_;
    QString current_task_id_;
};
