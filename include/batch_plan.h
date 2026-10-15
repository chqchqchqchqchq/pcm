#pragma once

#include "traditional_processing_types.h"
#include <QJsonObject>
#include <QString>
#include <QUuid>

/** @brief 可独立编辑和持久化的任务快照，不包含本次运行状态。 */
struct BatchTaskItem
{
    QString id = QUuid::createUuid().toString(QUuid::WithoutBraces);
    QString name;
    QString source_description;
    QString config_fingerprint;
    TraditionalProcessingOptions options;
    int time_mode = 0;
    QString time_range_file_path;
    bool time_ranges_resolved = true;

    /** @brief 为快照设置独立任务标识，并重建通用五元组。 */
    TraditionalProcessingOptions processingOptions() const;
    /** @brief 返回完整时间段摘要。 */
    QString timeDescription() const;
};

/** @brief 一个有序多任务方案，原始数据及配置仍使用外部文件引用。 */
struct BatchPlan
{
    QString name = QStringLiteral("新建多任务方案");
    QString output_root;
    QList<BatchTaskItem> tasks;
};

/** @brief 任务运行状态与文档数据分离，打开文档不会恢复执行。 */
enum class BatchTaskState { Ready, NeedsAttention, Waiting, Running, Stopping, Succeeded, Failed, Stopped, NotRun };

/** @brief 本次运行的可视状态及已提交结果。 */
struct BatchTaskRuntime
{
    BatchTaskState state = BatchTaskState::Ready;
    int progress = 0;
    QString detail;
    QStringList output_paths;
};

/** @brief 统一中文状态文案。 */
QString batchTaskStateText(BatchTaskState state);

/** @brief 带版本校验及原子写入的 UTF-8 .txt 方案文件。 */
class BatchPlanFile
{
public:
    /** @brief 完整解析成功后才替换输出对象；允许保存尚未修正的任务。 */
    static bool load(const QString &path, BatchPlan *plan, QString *error = nullptr);
    /** @brief 使用 QSaveFile 提交，失败时保留原文件。 */
    static bool save(const QString &path, const BatchPlan &plan, QString *error = nullptr);
};

Q_DECLARE_METATYPE(BatchTaskState)
Q_DECLARE_METATYPE(BatchTaskRuntime)
