#include "processing_history_store.h"
#include "time_range_parser.h"
#include <QFileInfo>
#include <QSettings>
#include <algorithm>

BatchTaskItem ProcessingHistoryEntry::toBatchTask() const
{
    BatchTaskItem item;
    item.name = QFileInfo(raw_file_path).completeBaseName();
    if (item.name.isEmpty()) { item.name = QStringLiteral("历史任务"); }
    item.source_description = QStringLiteral("最近单次处理记录 · %1").arg(started_at.toString("yyyy-MM-dd HH:mm:ss"));
    item.config_fingerprint = config_fingerprint;
    item.options.task_definition.config_file_path = config_path;
    item.options.task_definition.raw_file_path = raw_file_path;
    item.options.task_definition.user_sample_rate_hz = sample_rate_hz;
    item.options.parameter_names = parameter_names;
    item.options.parameter_group_file_path = parameter_group_path;
    item.options.include_raw_values = include_raw_values;
    item.time_mode = time_mode;
    item.time_range_file_path = time_range_file_path;
    if (time_mode == 1) {
        item.options.time_ranges = {{QTime(0, 0).msecsTo(start_time) / 1000.0,
                                     QTime(0, 0).msecsTo(end_time) / 1000.0}};
    } else if (time_mode == 2) {
        if (has_time_ranges_snapshot) {
            item.options.time_ranges = time_ranges;
            item.time_ranges_resolved = !time_ranges.isEmpty();
        } else {
            item.time_ranges_resolved = TimeRangeParser::parseFile(time_range_file_path, &item.options.time_ranges);
        }
    }
    item.options = item.processingOptions();
    return item;
}

QList<ProcessingHistoryEntry> ProcessingHistoryStore::load()
{
    QList<ProcessingHistoryEntry> entries;
    QSettings settings;
    settings.beginGroup(QStringLiteral("processingHistory"));
    if (settings.value("version", 0).toInt() != 1) { return entries; }
    const int count = settings.beginReadArray(QStringLiteral("records"));
    for (int index = 0; index < count; ++index) {
        settings.setArrayIndex(index);
        ProcessingHistoryEntry entry;
        entry.task_id = settings.value("taskId").toString();
        entry.started_at = QDateTime::fromString(settings.value("startedAt").toString(), Qt::ISODateWithMs);
        entry.status = settings.value("status", QStringLiteral("未知")).toString();
        entry.config_path = settings.value("configPath").toString();
        entry.config_fingerprint = settings.value("configFingerprint").toString();
        entry.parameter_group_from_file = settings.value("parameterGroupFromFile", false).toBool();
        entry.parameter_group_path = settings.value("parameterGroupPath").toString();
        entry.parameter_group_fingerprint = settings.value("parameterGroupFingerprint").toString();
        entry.parameter_names = settings.value("parameterNames").toStringList();
        entry.sample_rate_hz = settings.value("sampleRateHz", 1.0).toDouble();
        entry.time_mode = settings.value("timeMode", 0).toInt();
        entry.start_time = settings.value("startTime", QTime(0, 0)).toTime();
        entry.end_time = settings.value("endTime", QTime(23, 59, 59)).toTime();
        entry.raw_file_path = settings.value("rawFilePath").toString();
        entry.output_paths = settings.value("outputPaths").toStringList();
        entry.time_range_file_path = settings.value("timeRangeFilePath").toString();
        entry.include_raw_values = settings.value("includeRawValues", false).toBool();
        entry.has_time_ranges_snapshot = settings.value("hasTimeRangesSnapshot", false).toBool();
        for (const auto &value : settings.value("timeRanges").toList()) {
            const auto range = value.toMap();
            entry.time_ranges.append({range.value("start").toDouble(), range.value("end").toDouble()});
        }
        if (!entry.task_id.isEmpty() && entry.started_at.isValid() && !entry.config_path.isEmpty()) {
            entries.append(entry);
        }
    }
    settings.endArray();
    settings.endGroup();
    std::sort(entries.begin(), entries.end(), [](const ProcessingHistoryEntry &a, const ProcessingHistoryEntry &b) {
        return a.started_at > b.started_at;
    });
    while (entries.size() > 20) { entries.removeLast(); }
    return entries;
}

bool ProcessingHistoryStore::save(const QList<ProcessingHistoryEntry> &entries)
{
    QSettings settings;
    settings.beginGroup(QStringLiteral("processingHistory"));
    settings.remove(QString());
    settings.setValue("version", 1);
    settings.beginWriteArray(QStringLiteral("records"), qMin(entries.size(), 20));
    for (int index = 0; index < qMin(entries.size(), 20); ++index) {
        settings.setArrayIndex(index);
        const auto &entry = entries.at(index);
        const QVariantMap values{
            {"taskId", entry.task_id}, {"startedAt", entry.started_at.toString(Qt::ISODateWithMs)},
            {"status", entry.status}, {"configPath", entry.config_path},
            {"configFingerprint", entry.config_fingerprint},
            {"parameterGroupFromFile", entry.parameter_group_from_file}, {"parameterGroupPath",
                entry.parameter_group_path},
            {"parameterGroupFingerprint", entry.parameter_group_fingerprint}, {"parameterNames", entry.parameter_names},
            {"sampleRateHz", entry.sample_rate_hz}, {"timeMode", entry.time_mode},
            {"startTime", entry.start_time}, {"endTime", entry.end_time}, {"rawFilePath", entry.raw_file_path},
            {"outputPaths", entry.output_paths}, {"timeRangeFilePath", entry.time_range_file_path},
            {"includeRawValues", entry.include_raw_values}, {"hasTimeRangesSnapshot", entry.has_time_ranges_snapshot}};
        for (auto it = values.begin(); it != values.end(); ++it) { settings.setValue(it.key(), it.value()); }
        QVariantList ranges;
        for (const auto &range : entry.time_ranges) {
            ranges.append(QVariantMap{{"start", range.start_seconds}, {"end", range.end_seconds}});
        }
        settings.setValue("timeRanges", ranges);
    }
    settings.endArray();
    settings.endGroup();
    settings.sync();
    return settings.status() == QSettings::NoError;
}
