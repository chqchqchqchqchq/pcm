#pragma once

#include "batch_plan.h"
#include <QDateTime>
#include <QTime>

/** @brief 最近一次单任务处理的兼容快照，保留旧 QSettings 字段。 */
struct ProcessingHistoryEntry
{
    QString task_id;
    QDateTime started_at;
    QString status;
    QString config_path;
    QString config_fingerprint;
    bool parameter_group_from_file = false;
    QString parameter_group_path;
    QString parameter_group_fingerprint;
    QStringList parameter_names;
    double sample_rate_hz = 1.0;
    int time_mode = 0;
    QTime start_time;
    QTime end_time;
    QString raw_file_path;
    QStringList output_paths;
    QString time_range_file_path;
    bool include_raw_values = false;
    QList<TaskTimeRange> time_ranges;
    bool has_time_ranges_snapshot = false;

    /** @brief 复制为新任务，旧多时间段记录按需读取辅助文件。 */
    BatchTaskItem toBatchTask() const;
};

/** @brief 两个页面共享最近二十条记录，不将多任务运行写入单任务历史。 */
class ProcessingHistoryStore
{
public:
    /** @brief 读取旧版本兼容记录并按启动时间降序排列。 */
    static QList<ProcessingHistoryEntry> load();
    /** @brief 保留原键名保存快照，返回 QSettings 同步结果。 */
    static bool save(const QList<ProcessingHistoryEntry> &entries);
};
