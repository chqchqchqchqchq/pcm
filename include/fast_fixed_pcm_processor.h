#pragma once

#include "config_types.h"
#include "logger.h"
#include "raw_file_reader.h"
#include "result_writer.h"

#include <QAtomicInteger>
#include <QByteArray>
#include <QHash>
#include <QList>
#include <QObject>
#include <QSet>
#include <QString>

/**
 * @brief Runtime options for the fixed-frame fast path.
 */
struct FastFixedPcmProcessorOptions
{
    qint64 block_size_bytes = 256 * 1024 * 1024;
    qint64 resync_window_bytes = 1024 * 1024;
};

/**
 * @brief Task context for fixed-frame, per-stream PCM processing.
 */
struct FastFixedPcmTask
{
    TaskDefinition definition;
    QList<StreamDefinition> streams;
    QList<StreamLayoutTable> layout_tables;
    QHash<QString, QSet<QString>> selected_parameter_ids_by_stream;
    ResultWriterOptions writer_options;
};

/**
 * @brief 按旧版长周和时间码读取各流数据，分块缓冲并按时间归并写入 TXT。
 */
class FastFixedPcmProcessor : public QObject
{
    Q_OBJECT

public:
    explicit FastFixedPcmProcessor(const FastFixedPcmTask &task,
                                   const FastFixedPcmProcessorOptions &options,
                                   QObject *parent = nullptr);

    /**
     * @brief Returns true when every selected stream file can align to its expected single stream sync word.
     */
    static bool canUseFastPath(const TaskDefinition &definition,
                               const QList<StreamDefinition> &streams,
                               const QList<StreamLayoutTable> &layout_tables,
                               const QHash<QString, QSet<QString>> &selected_parameter_ids_by_stream,
                               QString *reason = nullptr,
                               qint64 resync_window_bytes = 1024 * 1024);

public slots:
    /**
     * @brief Runs the blocking fast-path processing loop in the current thread.
     */
    void process();

    /**
     * @brief Requests cooperative cancellation.
     */
    void requestStop();

signals:
    void started(const QString &task_id);
    void progressChanged(const QString &task_id, qint64 processed_bytes, qint64 total_bytes);
    void failed(const QString &task_id, const QString &reason);
    void finished(const QString &task_id,
                  const QString &output_path,
                  quint64 processed_frame_count,
                  quint64 produced_value_count);
    void stopped(const QString &task_id);

private:
    /** @brief 旧版长周数据按实际时间码有界归并输出。 */
    void processLegacy();
    /** @brief 写入任务级结构化日志。 */
    void log(LogLevel level, const QString &message) const;
    /** @brief 设置可选错误输出。 */
    static void setError(QString *error_message, const QString &message);

    FastFixedPcmTask task_;
    FastFixedPcmProcessorOptions options_;
    QAtomicInteger<int> stop_requested_ = 0;
};

Q_DECLARE_METATYPE(FastFixedPcmProcessorOptions)
Q_DECLARE_METATYPE(FastFixedPcmTask)
