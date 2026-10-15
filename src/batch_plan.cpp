#include "batch_plan.h"
#include "processing_options_support.h"
#include "raw_time_range_scanner.h"

#include <QFile>
#include <QJsonArray>
#include <QJsonDocument>
#include <QSaveFile>
#include <QSet>
#include <cmath>

namespace {
/** @brief 附加方案文件错误。 */
bool fail(QString *error, const QString &message)
{
    if (error) { *error = message; }
    return false;
}
/** @brief JSON 中必须存在的文本字段。 */
bool stringsPresent(const QJsonObject &object, const QStringList &keys)
{
    for (const auto &key : keys) { if (!object.value(key).isString()) { return false; } }
    return true;
}
/** @brief 将字段规范为跨平台绝对路径。 */
QString pathValue(const QJsonObject &object, const QString &key)
{
    return ProcessingOptionsSupport::absolutePath(object.value(key).toString());
}
}

TraditionalProcessingOptions BatchTaskItem::processingOptions() const
{
    auto result = options;
    result.task_definition.task_id = id;
    ProcessingOptionsSupport::normalize(&result);
    return result;
}

QString BatchTaskItem::timeDescription() const
{
    if (time_mode == 0) { return QStringLiteral("全程"); }
    if (!time_ranges_resolved || options.time_ranges.isEmpty()) { return QStringLiteral("时间段待补充"); }
    QStringList parts;
    for (const auto &range : options.time_ranges) {
        parts << QStringLiteral("%1—%2").arg(RawTimeRangeScanner::displayTime(range.start_seconds),
                                             RawTimeRangeScanner::displayTime(range.end_seconds));
    }
    return parts.join(QStringLiteral("；"));
}

QString batchTaskStateText(BatchTaskState state)
{
    switch (state) {
    case BatchTaskState::Ready: return QStringLiteral("就绪");
    case BatchTaskState::NeedsAttention: return QStringLiteral("待修正");
    case BatchTaskState::Waiting: return QStringLiteral("等待执行");
    case BatchTaskState::Running: return QStringLiteral("正在处理");
    case BatchTaskState::Stopping: return QStringLiteral("正在中断");
    case BatchTaskState::Succeeded: return QStringLiteral("已完成");
    case BatchTaskState::Failed: return QStringLiteral("失败");
    case BatchTaskState::Stopped: return QStringLiteral("已中断");
    case BatchTaskState::NotRun: return QStringLiteral("未执行");
    }
    return {};
}

bool BatchPlanFile::save(const QString &path, const BatchPlan &plan, QString *error)
{
    QJsonArray tasks;
    QSet<QString> ids;
    for (const auto &item : plan.tasks) {
        if (item.id.isEmpty() || ids.contains(item.id)) {
            return fail(error, QStringLiteral("任务标识为空或重复，无法保存。"));
        }
        ids.insert(item.id);
        const auto options = item.processingOptions();
        if (!std::isfinite(options.task_definition.user_sample_rate_hz)) {
            return fail(error, QStringLiteral("采样率不是有效数值。"));
        }
        QJsonArray ranges;
        for (const auto &range : options.time_ranges) {
            if (!std::isfinite(range.start_seconds) || !std::isfinite(range.end_seconds)) {
                return fail(error, QStringLiteral("时间段包含无效数值。"));
            }
            ranges.append(QJsonObject{{"startSeconds", range.start_seconds}, {"endSeconds", range.end_seconds}});
        }
        tasks.append(QJsonObject{
            {"id", item.id}, {"name", item.name}, {"sourceDescription", item.source_description},
            {"configFingerprint", item.config_fingerprint},
            {"configPath", options.task_definition.config_file_path},
            {"rawPath", options.task_definition.raw_file_path},
            {"parameterGroupPath", options.parameter_group_file_path},
            {"parameterNames", QJsonArray::fromStringList(options.parameter_names)},
            {"sampleRateHz", options.task_definition.user_sample_rate_hz},
            {"timeMode", item.time_mode}, {"timeRanges", ranges},
            {"timeRangeFilePath", ProcessingOptionsSupport::absolutePath(item.time_range_file_path)},
            {"timeRangesResolved", item.time_ranges_resolved}, {"includeRawValues", options.include_raw_values}});
    }
    const QJsonObject root{{"format", "pcm-batch"}, {"version", 1}, {"name", plan.name},
                          {"outputRoot", ProcessingOptionsSupport::absolutePath(plan.output_root)}, {"tasks", tasks}};
    QSaveFile file(path);
    const auto data = QJsonDocument(root).toJson(QJsonDocument::Indented);
    if (!file.open(QIODevice::WriteOnly) || file.write(data) != data.size() || !file.commit()) {
        return fail(error, QStringLiteral("保存方案失败：%1").arg(file.errorString()));
    }
    return true;
}

bool BatchPlanFile::load(const QString &path, BatchPlan *plan, QString *error)
{
    QFile file(path);
    if (!plan || !file.open(QIODevice::ReadOnly)) { return fail(error, QStringLiteral("无法读取方案文件。")); }
    if (file.size() > 16 * 1024 * 1024) { return fail(error, QStringLiteral("方案文件超过 16 MB 限制。")); }
    QJsonParseError parse_error;
    const auto document = QJsonDocument::fromJson(file.readAll(), &parse_error);
    if (file.error() != QFileDevice::NoError || parse_error.error != QJsonParseError::NoError || !document.isObject()) {
        return fail(error, QStringLiteral("方案文件损坏：%1").arg(parse_error.errorString()));
    }
    const auto root = document.object();
    if (root.value("format").toString() != QStringLiteral("pcm-batch") || root.value("version").toDouble() != 1) {
        return fail(error, QStringLiteral("不支持的方案格式或版本。"));
    }
    if (!stringsPresent(root, {"name", "outputRoot"}) || !root.value("tasks").isArray()) {
        return fail(error, QStringLiteral("方案缺少必要字段。"));
    }
    BatchPlan parsed;
    parsed.name = root.value("name").toString();
    parsed.output_root = pathValue(root, "outputRoot");
    QSet<QString> ids;
    for (const auto &value : root.value("tasks").toArray()) {
        const auto object = value.toObject();
        if (!value.isObject() || !stringsPresent(object, {"id", "name", "sourceDescription", "configPath",
                "rawPath", "parameterGroupPath", "timeRangeFilePath"}) ||
            !object.value("sampleRateHz").isDouble() || !object.value("timeMode").isDouble() ||
            !object.value("timeRangesResolved").isBool() || !object.value("includeRawValues").isBool() ||
            !object.value("parameterNames").isArray() || !object.value("timeRanges").isArray()) {
            return fail(error, QStringLiteral("任务字段缺失或类型不正确。"));
        }
        BatchTaskItem item;
        item.id = object.value("id").toString();
        if (item.id.isEmpty() || ids.contains(item.id)) { return fail(error, QStringLiteral("任务标识为空或重复。")); }
        ids.insert(item.id);
        item.name = object.value("name").toString();
        item.source_description = object.value("sourceDescription").toString();
        item.config_fingerprint = object.value("configFingerprint").toString();
        auto &options = item.options;
        options.task_definition.config_file_path = pathValue(object, "configPath");
        options.task_definition.raw_file_path = pathValue(object, "rawPath");
        options.parameter_group_file_path = pathValue(object, "parameterGroupPath");
        options.task_definition.user_sample_rate_hz = object.value("sampleRateHz").toDouble();
        for (const auto &name : object.value("parameterNames").toArray()) {
            if (!name.isString()) { return fail(error, QStringLiteral("参数名称必须是文本。")); }
            options.parameter_names.append(name.toString());
        }
        const double mode = object.value("timeMode").toDouble();
        if (mode < 0 || mode > 2 || std::floor(mode) != mode) {
            return fail(error, QStringLiteral("时间模式无效。"));
        }
        item.time_mode = int(mode);
        item.time_ranges_resolved = object.value("timeRangesResolved").toBool();
        item.time_range_file_path = pathValue(object, "timeRangeFilePath");
        options.include_raw_values = object.value("includeRawValues").toBool();
        for (const auto &range : object.value("timeRanges").toArray()) {
            const auto obj = range.toObject();
            if (!range.isObject() || !obj.value("startSeconds").isDouble() || !obj.value("endSeconds").isDouble()) {
                return fail(error, QStringLiteral("时间段字段不正确。"));
            }
            options.time_ranges.append({obj.value("startSeconds").toDouble(), obj.value("endSeconds").toDouble()});
        }
        item.options = item.processingOptions();
        parsed.tasks.append(item);
    }
    *plan = parsed;
    return true;
}
