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
#include <QStringList>

class ProcessingConfigCache;

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
    /** @brief 整个单任务请求的最终结果，所有时间段共用一个终态。 */
    enum class ProcessingOutcome { Succeeded, Failed, Stopped };
    Q_ENUM(ProcessingOutcome)
    /** @brief 包括清理阶段在内的忙状态，防止终态通知前重复启动。 */
    bool isBusy() const;
    /** @brief 仅在空闲时注入本轮共享配置缓存。 */
    void setConfigCache(const QSharedPointer<ProcessingConfigCache> &cache);
    /**
     * @brief Creates the controller and wires TaskScheduler lifecycle signals.
     */
    explicit TraditionalProcessingController(QObject *parent = nullptr);

    /**
     * @brief Stops any active pipeline before member backends are destroyed.
     */
    ~TraditionalProcessingController() override;

    /**
     * @brief Performs the same configuration, stream, and parameter-selection checks used at startup.
     */
    bool validateOptions(const TraditionalProcessingOptions &options,
                         QString *error_message = nullptr) const;

    /** @brief 无共享状态的配置导入，供处理任务和后台时间扫描共用。 */
    static bool loadConfig(const QString &file_path, Config *config, QString *error_message);
    /** @brief 将配置转为相同的原始帧同步规则，可在后台扫描线程调用。 */
    static bool buildStreamDefinitions(const Config &config, QList<StreamDefinition> *streams,
                                       QString *error_message);

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
    /** @brief 全部时间段和线程资源释放后恰好一次发出，允许安全派发下一条任务。 */
    void processingSettled(const QString &request_id, ProcessingOutcome outcome,
                           const QStringList &output_paths, const QString &reason);
    /** @brief 请求开始至资源彻底释放之间保持忙状态。 */
    void busyChanged(bool busy);
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
     * @brief Emitted after one range in a multi-range request commits its independent result file.
     */
    void rangeFinished(const QString &request_id,
                       int range_index,
                       int range_count,
                       const QString &output_path);

    /**
     * @brief Emitted after every requested range finishes successfully.
     */
    void processingFinished(const QString &request_id, const QStringList &output_paths);

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
    /** @brief 捕获已有终态信号，异步等待线程清理完成。 */
    void queueSettlement(ProcessingOutcome outcome, const QString &reason = QString());
    /** @brief 只在处理器和读取调度器完全空闲后发布终结通知。 */
    void trySettle();
    /** @brief 优先使用本轮不可变配置缓存。 */
    bool loadProcessingConfig(const QString &path, Config *config, QString *error) const;

    QSharedPointer<ProcessingConfigCache> config_cache_;
    bool request_in_flight_ = false;
    bool settlement_pending_ = false;
    bool rejecting_request_ = false;
    ProcessingOutcome settlement_outcome_ = ProcessingOutcome::Succeeded;
    QString settlement_request_id_;
    QString settlement_reason_;
    QStringList settlement_output_paths_;

    void startSingleProcessing(const TraditionalProcessingOptions &options);
    void startNextRange();
    void completeCurrentRange(const QString &task_id, const QString &output_path);
    void resetBatch();
    QString publicTaskId(const QString &task_id) const;
    int aggregateProgress(int local_progress_percent) const;
    QString rangeDetail(const QString &detail) const;
    /**
     * @brief Adds range position and already committed output paths to a failure message.
     */
    QString failureDetail(const QString &detail) const;

    bool applyParameterSelection(const TraditionalProcessingOptions &options,
                                 const Config &config,
                                 TaskDefinition *definition,
                                 QHash<QString, QSet<QString>> *selected_parameter_ids_by_stream,
                                 QString *error_message) const;
    void handleTaskQueuesReady(const TaskDefinition &definition,
                               const QList<StreamDefinition> &streams,
                               const StreamFrameQueues &frame_queues);
    void handleParameterReady(const ParameterValue &value);
    void handleParameterBatchReady(const QList<ParameterValue> &values);
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
    void handleFastTaskStarted(const QString &task_id);
    void handleFastTaskProgress(const QString &task_id, qint64 processed_bytes, qint64 total_bytes);
    void handleFastTaskFailed(const QString &task_id, const QString &reason);
    void handleFastTaskStopped(const QString &task_id);
    void handleFastTaskFinished(const QString &task_id,
                                const QString &output_path,
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
    TraditionalProcessingOptions batch_options_;
    QList<TaskTimeRange> batch_ranges_;
    QString batch_request_id_;
    QStringList completed_output_paths_;
    int current_range_index_ = -1;
    bool batch_active_ = false;
    bool batch_stop_requested_ = false;
};
