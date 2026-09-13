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

class QFile;
class QSaveFile;

/**
 * @brief Runtime options for the fixed-frame fast path.
 */
struct FastFixedPcmProcessorOptions
{
    qint64 block_size_bytes = 256 * 1024 * 1024;
    qint64 resync_window_bytes = 1024 * 1024;
    int worker_count = 0;
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
 * @brief Reads per-stream fixed-frame PCM files, decodes them in large chunks, and writes TXT rows directly.
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
    struct StreamPlan;
    struct ExecutionPlan;
    struct StreamBlock;
    struct ChunkResult;
    struct FastOutputColumn;
    struct FastTxtBlock;
    struct CompiledSegmentPlan;
    struct CompiledParameterPlan;
    struct CompiledStreamDecodePlan;

    bool buildExecutionPlan(ExecutionPlan *plan, QString *error_message) const;
    static bool buildExecutionPlanForTask(const TaskDefinition &definition,
                                          const QList<StreamDefinition> &streams,
                                          const QList<StreamLayoutTable> &layout_tables,
                                          const QHash<QString, QSet<QString>> &selected_parameter_ids_by_stream,
                                          qint64 resync_window_bytes,
                                          ExecutionPlan *plan,
                                          QString *error_message);
    ChunkResult processStreamFrameRange(const QByteArray &block_data,
                                        const StreamPlan &stream_plan,
                                        quint64 first_frame,
                                        quint64 frame_offset_in_block,
                                        quint64 frame_count) const;
    bool processAndWriteBlocks(const QList<StreamBlock> &stream_blocks,
                              ResultWriter *writer,
                              quint64 *processed_frame_count,
                              quint64 *produced_value_count,
                              QString *error_message) const;
    bool processAndWriteTxtBlocks(const QList<StreamBlock> &stream_blocks,
                                  QSaveFile *output_file,
                                  const QList<FastOutputColumn> &columns,
                                  quint64 *processed_frame_count,
                                  quint64 *produced_value_count,
                                  qint64 *written_text_bytes,
                                  QString *error_message) const;
    FastTxtBlock processSynchronousTxtFrameRange(const QList<StreamBlock> &stream_blocks,
                                                 const QList<FastOutputColumn> &columns,
                                                 quint64 first_frame_offset,
                                                 quint64 frame_count) const;
    bool processMergedTxtBlocks(const QList<StreamBlock> &stream_blocks,
                                QSaveFile *output_file,
                                const QList<FastOutputColumn> &columns,
                                quint64 *processed_frame_count,
                                quint64 *produced_value_count,
                                qint64 *written_text_bytes,
                                QString *error_message) const;
    void log(LogLevel level, const QString &message) const;

    static qint64 syncUnitByteCount(const StreamLayoutTable &layout_table);
    static bool prepareOutputColumns(ExecutionPlan *plan,
                                     const TaskDefinition &definition,
                                     QList<FastOutputColumn> *columns,
                                     QString *error_message);
    static bool compileDecodePlan(const StreamLayoutTable &layout_table,
                                  const QSet<QString> &selected_parameter_ids,
                                  CompiledStreamDecodePlan *decode_plan,
                                  QString *error_message);
    static bool decodeFrameWithCompiledPlan(const char *frame_data,
                                            qsizetype frame_size,
                                            const QString &task_id,
                                            double timestamp_seconds,
                                            const CompiledStreamDecodePlan &decode_plan,
                                            QList<ParameterValue> *values,
                                            QString *error_message);
    static bool readCompiledSegment(const char *frame_data,
                                    qsizetype frame_size,
                                    const CompiledSegmentPlan &segment,
                                    quint64 *value,
                                    QString *error_message);
    static bool decodeFrameToTextRow(const char *frame_data,
                                     qsizetype frame_size,
                                     const CompiledStreamDecodePlan &decode_plan,
                                     bool include_raw_values,
                                     QList<QByteArray> *row_values,
                                     quint64 *produced_value_count,
                                     QString *error_message);
    static bool convertCompiledValue(quint64 raw_bits,
                                     int bit_count,
                                     const CompiledParameterPlan &parameter,
                                     QVariant *value,
                                     QString *error_message);
    static bool convertCompiledValueToText(quint64 raw_bits,
                                           int bit_count,
                                           const CompiledParameterPlan &parameter,
                                           QByteArray *value,
                                           QString *error_message);
    static bool openFastTextOutput(const TaskDefinition &definition,
                                   const ResultWriterOptions &options,
                                   const QList<FastOutputColumn> &columns,
                                   QSaveFile *output_file,
                                   QString *output_path,
                                   QString *error_message);
    static QByteArray formatTimestamp(double timestamp_seconds, const ResultWriterOptions &options);
    static bool blockHasFrameAt(const char *data, qsizetype available, const StreamPlan &stream_plan);
    static bool findFirstFrameOffset(QFile *file,
                                     const StreamPlan &stream_plan,
                                     qint64 resync_window_bytes,
                                     qint64 *offset,
                                     QString *error_message);
    static void setError(QString *error_message, const QString &message);

    FastFixedPcmTask task_;
    FastFixedPcmProcessorOptions options_;
    QAtomicInteger<int> stop_requested_ = 0;
};

Q_DECLARE_METATYPE(FastFixedPcmProcessorOptions)
Q_DECLARE_METATYPE(FastFixedPcmTask)
