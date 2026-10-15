#pragma once

#include "batch_plan.h"
#include "processing_config_cache.h"
#include "traditional_processing_controller.h"
#include <QHash>
#include <QFile>

/** @brief 每次只驱动一个完整单任务请求；失败继续，中断停止派发，资源按任务隔离。 */
class BatchProcessingController : public QObject
{
    Q_OBJECT
public:
    /** @brief 创建独立处理控制器并连接安全终结通知。 */
    explicit BatchProcessingController(QObject *parent = nullptr);
    /** @brief 启动前收集所有错误，以任务标识定位；不创建结果文件。 */
    QHash<QString, QString> validatePlan(const BatchPlan &plan);
    /** @brief 校验全部任务后启动；失败时返回完整错误，列表保持可修正。 */
    bool start(const BatchPlan &plan, QString *error = nullptr);
    /** @brief 当前批次是否仍占用处理资源。 */
    bool isRunning() const;
    /** @brief 请求中断当前任务，后续任务不再派发。 */
    void requestStop();
    /** @brief 本次运行的独立结果目录。 */
    QString outputDirectory() const;
    /** @brief 单条任务快照的完整检查，供界面与启动预检共用。 */
    QString validateTask(const BatchTaskItem &item);

signals:
    /** @brief 运行标记包含启动校验之后的全部执行及清理阶段。 */
    void runningChanged(bool running);
    /** @brief 以任务标识路由行状态、进度及输出路径。 */
    void taskChanged(const QString &id, const BatchTaskRuntime &runtime);
    /** @brief 任务等权总进度及成功/失败计数。 */
    void progressChanged(int percent, int succeeded, int failed, int total);
    /** @brief 全部执行完毕或中断完成时的汇总。 */
    void finished(int succeeded, int failed, bool interrupted, const QString &directory);
    /** @brief 当前批次的用户可见日志。 */
    void message(const QString &text);

private:
    /** @brief 事件循环中派发下一条任务，避免终结回调重入。 */
    void startNext();
    /** @brief 从运行快照汇总进度。 */
    void updateProgress();
    /** @brief 释放批次缓存并发送结束汇总。 */
    void finish();
    /** @brief 向本次结果目录写入少量生命周期日志。 */
    void writeRunLog(const QString &text);
    /** @brief 保存本轮结果汇总，重开方案仍只恢复任务设置。 */
    void writeSummary();
    QFile run_log_;
    TraditionalProcessingController controller_;
    QSharedPointer<ProcessingConfigCache> cache_;
    BatchPlan plan_;
    QHash<QString, BatchTaskRuntime> runtime_;
    int current_index_ = -1;
    int succeeded_ = 0;
    int failed_ = 0;
    bool running_ = false;
    bool stop_requested_ = false;
    QString output_directory_;
};
