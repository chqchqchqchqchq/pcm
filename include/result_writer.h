#pragma once

#include "config_types.h"
#include "logger.h"

#include <QHash>
#include <QList>
#include <QMutex>
#include <QObject>
#include <QString>

/**
 * @brief Controls task-result text output, timestamp formatting, and missing-value rendering.
 */
struct ResultWriterOptions
{
    QString file_name_suffix = QStringLiteral("_results.txt");
    QString output_file_path;
    int timestamp_precision = 9;
    QString missing_value_text;
};

Q_DECLARE_METATYPE(ResultWriterOptions)

/**
 * @brief Writes one independent, time-ordered result file for each task.
 *
 * Frame processors may append ParameterValue objects from multiple worker threads in any order. ResultWriter keeps the
 * values isolated by task_id, performs a stable timestamp reorder when the task is finished, then merges values from
 * all streams into rows keyed by timestamp. Each output row starts with timestamp_seconds, followed by deterministic
 * stream-qualified parameter columns.
 */
class ResultWriter : public QObject
{
    Q_OBJECT

public:
    explicit ResultWriter(QObject *parent = nullptr);

    /**
     * @brief Replaces writer options. timestamp_precision is clamped to a safe range for double formatting.
     */
    void setOptions(const ResultWriterOptions &options);

    /**
     * @brief Returns the current writer options.
     */
    ResultWriterOptions options() const;

    /**
     * @brief Registers a task-local result buffer and resolves its independent output file path.
     */
    bool startTask(const TaskDefinition &definition, QString *error_message = nullptr);

    /**
     * @brief Appends one decoded parameter value to its owning task buffer.
     */
    bool appendValue(const ParameterValue &value, QString *error_message = nullptr);

    /**
     * @brief Appends a batch of decoded parameter values, preserving batch order for duplicate conflict resolution.
     */
    bool appendValues(const QList<ParameterValue> &values, QString *error_message = nullptr);

    /**
     * @brief Convenience helper that writes a full task result in one call.
     */
    bool writeTaskResult(const TaskDefinition &definition,
                         const QList<ParameterValue> &values,
                         QString *output_path = nullptr,
                         QString *error_message = nullptr);

    /**
     * @brief Sorts, merges, writes, and releases the task-local result buffer.
     */
    bool finishTask(const QString &task_id,
                    QString *output_path = nullptr,
                    QString *error_message = nullptr);

    /**
     * @brief Drops a task-local result buffer without writing a file.
     */
    bool abortTask(const QString &task_id);

    /**
     * @brief Returns true while a task is registered and waiting for finishTask() or abortTask().
     */
    bool hasTask(const QString &task_id) const;

    /**
     * @brief Returns the output file path resolved by startTask(), or an empty string for unknown tasks.
     */
    QString outputPathForTask(const QString &task_id) const;

signals:
    /**
     * @brief Emitted after startTask() reserves the per-task output path.
     */
    void taskOutputStarted(const QString &task_id, const QString &output_path);

    /**
     * @brief Emitted after finishTask() successfully commits the merged file.
     */
    void taskOutputFinished(const QString &task_id, const QString &output_path, quint64 row_count);

    /**
     * @brief Emitted when validation or file output fails.
     */
    void taskOutputFailed(const QString &task_id, const QString &reason);

private:
    struct ColumnDefinition
    {
        int stream_index = 0;
        QString parameter_id;
        QString parameter_name;
        QString unit;
        QString key;
    };

    struct PendingTask
    {
        TaskDefinition definition;
        QString output_path;
        QList<ParameterValue> values;
    };

    QList<ColumnDefinition> buildColumns(const TaskDefinition &definition,
                                         const QList<ParameterValue> &sorted_values) const;
    bool writeMergedFile(const PendingTask &task,
                         const ResultWriterOptions &options,
                         quint64 *row_count,
                         QString *error_message) const;
    void log(LogLevel level, const QString &task_id, const QString &parameter_name, const QString &message) const;

    static QString formatTimestamp(double timestamp_seconds, const ResultWriterOptions &options);
    static QString makeColumnKey(int stream_index, const QString &parameter_id, const QString &parameter_name);
    static QString makeColumnKey(const ParameterValue &value);
    static QString makeColumnTitle(const ColumnDefinition &column);
    static QString makeOutputPath(const TaskDefinition &definition, const ResultWriterOptions &options);
    static QString safeFileName(const QString &file_name);
    static QString sanitizeCell(QString text);
    static QString variantToText(const QVariant &value);
    static void setError(QString *error_message, const QString &message);

    mutable QMutex mutex_;
    ResultWriterOptions options_;
    QHash<QString, PendingTask> pending_tasks_;
};
