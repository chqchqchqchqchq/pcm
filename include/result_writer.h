#pragma once

#include "config_types.h"
#include "logger.h"

#include <QHash>
#include <QList>
#include <QMutex>
#include <QObject>
#include <QString>
#include <QStringList>
#include <QVector>

class QSaveFile;
class QTextStream;

/**
 * @brief Controls task-result text output, timestamp formatting, and missing-value rendering.
 */
struct ResultWriterOptions
{
    QString file_name_suffix = QStringLiteral("_results.txt");
    QString output_file_path;
    int timestamp_precision = 9;
    /** @brief 传统处理使用旧版 TIME 列，格式为 HH:mm:ss:zzz，毫秒以下截断。 */
    bool legacy_time_format = false;
    QString missing_value_text;
    bool include_raw_values = false;
};

Q_DECLARE_METATYPE(ResultWriterOptions)

/**
 * @brief Writes one independent, time-ordered result file for each task.
 *
 * Frame processors may append ParameterValue objects from multiple worker threads in any order. ResultWriter keeps the
 * values isolated by task_id, performs a stable timestamp reorder when the task is finished, then merges values from
 * all streams into rows keyed by precise timestamp. The first column is TIME in legacy mode or timestamp_seconds
 * otherwise, followed by parameter names; stream ids and units remain internal routing metadata.
 */
class ResultWriter : public QObject
{
    Q_OBJECT

public:
    explicit ResultWriter(QObject *parent = nullptr);
    /** @brief 异常退出时取消所有尚未提交的流式输出并释放资源。 */
    ~ResultWriter() override;

    /**
     * @brief 设置后续任务的选项；已启动任务保留独立快照。时间精度限制在安全范围内。
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
     * @brief Starts a task in streaming mode using layout metadata to write the header immediately.
     */
    bool startStreamingTask(const TaskDefinition &definition,
                            const QList<StreamLayoutTable> &layout_tables,
                            QString *error_message = nullptr);

    /**
     * @brief Appends one timestamp-ordered decoded block to a streaming task.
     */
    bool appendDecodedBlock(const QString &task_id,
                            const QList<ParameterValue> &values,
                            QString *error_message = nullptr);

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
    /** @brief 复用行内存；整数直接存入定长缓冲，其他类型保持 Qt 的转换规则。 */
    struct StreamingCell
    {
        char number[32];
        int number_size = -1;
        QByteArray text;
        bool present = false;
    };

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
        ResultWriterOptions writer_options;
        QString output_path;
        QList<ParameterValue> values;
        bool streaming = false;
        QSaveFile *stream_file = nullptr;
        QList<ColumnDefinition> streaming_columns;
        QHash<int, QHash<QString, int>> streaming_column_index_by_stream;
        double last_timestamp_seconds = 0;
        QString current_timestamp_key;
        QString current_timestamp_text;
        QVector<StreamingCell> current_row_cells;
        QByteArray missing_cell;
        QByteArray output_buffer;
        QString stream_error;
        int next_column_hint = 0;
        quint64 streamed_row_count = 0;
    };

    QList<ColumnDefinition> buildColumns(const TaskDefinition &definition,
                                         const QList<ParameterValue> &sorted_values) const;
    QList<ColumnDefinition> buildColumnsFromLayouts(const TaskDefinition &definition,
                                                    const QList<StreamLayoutTable> &layout_tables) const;
    bool writeMergedFile(const PendingTask &task,
                         const ResultWriterOptions &options,
                         quint64 *row_count,
                         QString *error_message) const;
    bool initializeStreamingOutput(PendingTask *task,
                                   const ResultWriterOptions &options,
                                   QString *error_message) const;
    bool appendStreamingValues(PendingTask *task,
                               const QList<ParameterValue> &values,
                               const ResultWriterOptions &options,
                               QString *error_message) const;
    bool finishStreamingOutput(PendingTask *task,
                               quint64 *row_count,
                               QString *error_message) const;
    static void resetStreamingRow(PendingTask *task, const ResultWriterOptions &options);
    /** @brief 向任务私有缓冲拼接完整行，达到阈值时写入临时文件，不提交目标文件。 */
    static bool flushStreamingRow(PendingTask *task, QString *error_message);
    /** @brief 写入并检查短写；错误保持到任务结束，禁止提交不完整的结果。 */
    static bool flushStreamingBuffer(PendingTask *task, QString *error_message);
    /** @brief 整数免堆分配；浮点仍使用 Qt 的 17 位有效数字转换。 */
    static void formatStreamingCell(StreamingCell *cell, const QVariant &value);
    static void destroyStreamingOutput(PendingTask *task);
    void log(LogLevel level, const QString &task_id, const QString &parameter_name, const QString &message) const;

    /** @brief 精确分行键不受可见时间格式影响，避免毫秒内多个采样点被覆盖。 */
    static QString timestampKey(double timestamp_seconds, const ResultWriterOptions &options);
    /** @brief 仅格式化显示文本，旧版格式按当天时分秒和毫秒输出。 */
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
