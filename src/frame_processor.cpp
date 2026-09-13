#include "frame_processor.h"
#include "legacy_pcm_decoder.h"

#include "logger.h"

#include <QMutexLocker>
#include <QtConcurrent>

#include <algorithm>
#include <cstring>
#include <cmath>
#include <limits>

namespace {

void setError(QString *error_message, const QString &message)
{
    if (error_message != nullptr) {
        *error_message = message;
    }
}

quint64 bitMask(int bit_count)
{
    if (bit_count <= 0) {
        return 0;
    }
    if (bit_count >= 64) {
        return std::numeric_limits<quint64>::max();
    }

    return (quint64(1) << bit_count) - quint64(1);
}

QString parameterLabel(const StreamLayoutEntry &entry)
{
    if (!entry.parameter_id.trimmed().isEmpty() && !entry.parameter_name.trimmed().isEmpty()) {
        return QStringLiteral("%1/%2").arg(entry.parameter_id.trimmed(), entry.parameter_name.trimmed());
    }
    if (!entry.parameter_id.trimmed().isEmpty()) {
        return entry.parameter_id.trimmed();
    }
    if (!entry.parameter_name.trimmed().isEmpty()) {
        return entry.parameter_name.trimmed();
    }

    return QStringLiteral("#%1").arg(entry.serial_no);
}

bool hasEngineeringCalibration(CalibrationType type)
{
    return type == CalibrationType::Linear
        || type == CalibrationType::Polynomial
        || type == CalibrationType::Segmented
        || type == CalibrationType::Hyperbolic;
}

bool readBitsFromLittleEndianWordBytes(const char *frame_data,
                                       qsizetype frame_size,
                                       int word_length_bits,
                                       qint64 word_ordinal,
                                       int source_bit,
                                       int bit_length,
                                       quint64 *value,
                                       QString *error_message)
{
    if (value == nullptr) {
        setError(error_message, QStringLiteral("value output pointer is null"));
        return false;
    }
    if (frame_data == nullptr || frame_size <= 0) {
        setError(error_message, QStringLiteral("frame_data is empty"));
        return false;
    }
    if (word_length_bits <= 0 || word_ordinal < 0 || source_bit < 0 || bit_length < 0 || bit_length > 64) {
        setError(error_message,
                 QStringLiteral("Invalid word bit range word_length=%1 word=%2 source=%3 length=%4")
                     .arg(word_length_bits)
                     .arg(word_ordinal)
                     .arg(source_bit)
                     .arg(bit_length));
        return false;
    }

    // 旧版 source_bit 从最低有效位起算，先按小端读取整个 PCM 存储字。
    const int byte_count = (word_length_bits + 7) / 8;
    if (byte_count > 8 || source_bit > word_length_bits || bit_length > word_length_bits - source_bit) {
        setError(error_message, QStringLiteral("参数源位范围超出 PCM 字"));
        return false;
    }
    const qint64 offset = word_ordinal * byte_count;
    if (offset < 0 || offset > frame_size || byte_count > frame_size - offset) {
        setError(error_message, QStringLiteral("PCM 字地址超出帧数据"));
        return false;
    }
    quint64 word = 0;
    for (int index = 0; index < byte_count; ++index) {
        word |= quint64(static_cast<uchar>(frame_data[offset + index])) << (index * 8);
    }
    *value = source_bit == 64 ? 0 : (word >> source_bit) & bitMask(bit_length);
    return true;
}

} // namespace

struct FrameProcessor::StreamWorkerContext
{
    QString task_id;
    int stream_index = 0;
    QString stream_id;
    StreamLayoutTable layout_table;
    FrameTaskQueuePtr frame_queue;
    ParameterValueQueuePtr result_queue;
    QSet<QString> selected_parameter_ids;
    int wait_timeout_ms = 50;
    int value_batch_size = 512;
    int progress_interval_frames = 1024;
    int worker_index = 0;
    bool enable_result_queues = true;
    bool emit_individual_values = true;
};

FrameProcessor::FrameProcessor(QObject *parent)
    : QObject(parent)
{
    qRegisterMetaType<ParameterValue>("ParameterValue");
    qRegisterMetaType<QList<ParameterValue>>("QList<ParameterValue>");
    qRegisterMetaType<ParameterValueQueuePtr>("ParameterValueQueuePtr");
    qRegisterMetaType<StreamParameterValueQueues>("StreamParameterValueQueues");
    qRegisterMetaType<FrameProcessorOptions>("FrameProcessorOptions");
    qRegisterMetaType<FrameProcessorTask>("FrameProcessorTask");
}

FrameProcessor::FrameProcessor(const FrameProcessorTask &task,
                               const FrameProcessorOptions &options,
                               QObject *parent)
    : FrameProcessor(parent)
{
    task_ = task;
    options_ = options;
}

FrameProcessor::~FrameProcessor()
{
    requestStop();
    waitForFinished();
}

void FrameProcessor::setProcessingTask(const FrameProcessorTask &task)
{
    QMutexLocker locker(&mutex_);
    if (running_) {
        return;
    }

    task_ = task;
}

void FrameProcessor::setOptions(const FrameProcessorOptions &options)
{
    QMutexLocker locker(&mutex_);
    if (running_) {
        return;
    }

    options_ = options;
}

StreamParameterValueQueues FrameProcessor::resultQueues() const
{
    QMutexLocker locker(&mutex_);
    return task_.result_queues;
}

StreamParameterValueQueues FrameProcessor::createResultQueues(const QList<StreamLayoutTable> &layout_tables,
                                                              int max_queue_size)
{
    StreamParameterValueQueues result_queues;
    for (const StreamLayoutTable &layout_table : layout_tables) {
        if (!layout_table.stream_id.trimmed().isEmpty() && !result_queues.contains(layout_table.stream_id)) {
            result_queues.insert(layout_table.stream_id, ParameterValueQueuePtr::create(max_queue_size));
        }
    }

    return result_queues;
}

bool FrameProcessor::extractRawBits(const QByteArray &frame_data,
                                    const StreamLayoutTable &layout_table,
                                    const StreamLayoutEntry &entry,
                                    quint64 *raw_bits,
                                    int *bit_count,
                                    QString *error_message)
{
    return extractRawBitsInternal(frame_data,
                                  layout_table,
                                  entry,
                                  raw_bits,
                                  bit_count,
                                  error_message);
}

bool FrameProcessor::convertRawValue(quint64 raw_bits,
                                     int bit_count,
                                     const StreamLayoutEntry &entry,
                                     QVariant *value,
                                     QString *error_message)
{
    if (value == nullptr) {
        setError(error_message, QStringLiteral("value output pointer is null"));
        return false;
    }

    if (bit_count <= 0 || bit_count > 64) {
        setError(error_message,
                 QStringLiteral("Parameter %1 has invalid bit_count=%2")
                     .arg(parameterLabel(entry))
                     .arg(bit_count));
        return false;
    }

    const quint64 mask = bitMask(bit_count);
    quint64 normalized_bits = raw_bits & mask;

    QVariant decoded_value;
    double numeric_value = 0.0;
    switch (entry.code_type) {
    case ParameterCodeType::SignedInteger: {
        qlonglong signed_value = 0;
        int signed_width = 0;
        for (const ParameterWordSegment &segment : entry.word_segments) {
            signed_width += segment.bit_length;
        }
        signed_width = signed_width > 0 ? qMin(signed_width, 64) : bit_count;
        if (!decodeSignedInteger(normalized_bits, signed_width, 0, &signed_value, error_message)) {
            return false;
        }
        decoded_value = QVariant::fromValue(signed_value);
        numeric_value = static_cast<double>(signed_value);
        break;
    }
    case ParameterCodeType::Bcd: {
        qlonglong bcd_value = 0;
        if (!decodeBcd(normalized_bits, bit_count, &bcd_value, error_message)) {
            return false;
        }
        decoded_value = QVariant::fromValue(bcd_value);
        numeric_value = static_cast<double>(bcd_value);
        break;
    }
    case ParameterCodeType::FloatingPoint: {
        double floating_value = 0.0;
        if (bit_count <= 16) {
            qlonglong signed_value = 0;
            if (!decodeSignedInteger(normalized_bits, 16, 0, &signed_value, error_message)) {
                return false;
            }
            decoded_value = double(signed_value);
            numeric_value = double(signed_value);
            break;
        }
        if (!decodeFloatingPoint(normalized_bits, bit_count, &floating_value, error_message)) {
            return false;
        }
        decoded_value = floating_value;
        numeric_value = floating_value;
        break;
    }
    case ParameterCodeType::SignedMagnitude: {
        const quint64 sign = quint64(1) << (bit_count - 1);
        const qlonglong magnitude = static_cast<qlonglong>(normalized_bits & (sign - 1));
        decoded_value = QVariant::fromValue((normalized_bits & sign) ? -magnitude : magnitude);
        numeric_value = decoded_value.toDouble();
        break;
    }
    case ParameterCodeType::Fcs618:
        numeric_value = (normalized_bits >= 32768 ? double(normalized_bits) - 65536 : double(normalized_bits))
            * 10.0 / 32768.0;
        decoded_value = numeric_value;
        break;
    case ParameterCodeType::UnsignedInteger:
    case ParameterCodeType::Unknown:
    default:
        decoded_value = QVariant::fromValue(normalized_bits);
        numeric_value = static_cast<double>(normalized_bits);
        break;
    }

    bool calibration_applied = false;
    double engineering_value = numeric_value;
    if (!applyCalibration(numeric_value,
                          entry.calibration,
                          &engineering_value,
                          &calibration_applied,
                          error_message)) {
        return false;
    }

    if (!std::isfinite(engineering_value)) {
        setError(error_message, QStringLiteral("参数工程值不是有限数值"));
        return false;
    }
    *value = calibration_applied ? QVariant(engineering_value) : decoded_value;
    return true;
}

bool FrameProcessor::processFrame(const FrameTask &frame_task,
                                  const StreamLayoutTable &layout_table,
                                  QList<ParameterValue> *values,
                                  QString *error_message,
                                  const QSet<QString> &selected_parameter_ids)
{
    if (layout_table.legacy_semantics) {
        return LegacyPcmDecoder::decode(frame_task, layout_table, selected_parameter_ids, values, error_message);
    }
    return processFrameData(frame_task.frame_data.constData(),
                            frame_task.frame_data.size(),
                            frame_task.task_id,
                            frame_task.timestamp_seconds,
                            layout_table,
                            values,
                            error_message,
                            selected_parameter_ids);
}

bool FrameProcessor::processFrameData(const char *frame_data,
                                      qsizetype frame_size,
                                      const QString &task_id,
                                      double timestamp_seconds,
                                      const StreamLayoutTable &layout_table,
                                      QList<ParameterValue> *values,
                                      QString *error_message,
                                      const QSet<QString> &selected_parameter_ids)
{
    if (layout_table.legacy_semantics) {
        setError(error_message, QStringLiteral("旧版长周应使用携带采样上下文的 processFrame"));
        return false;
    }
    if (values == nullptr) {
        setError(error_message, QStringLiteral("values output pointer is null"));
        return false;
    }
    if (frame_data == nullptr || frame_size <= 0) {
        setError(error_message, QStringLiteral("frame_data is empty"));
        return false;
    }

    values->clear();
    for (const StreamLayoutEntry &entry : layout_table.entries) {
        if (!selected_parameter_ids.isEmpty() && !selected_parameter_ids.contains(entry.parameter_id)) {
            continue;
        }

        quint64 raw_bits = 0;
        int bit_count = 0;
        QString local_error;
        if (!extractRawBitsFromData(frame_data,
                                    frame_size,
                                    layout_table,
                                    entry,
                                    &raw_bits,
                                    &bit_count,
                                    &local_error)) {
            setError(error_message, local_error);
            return false;
        }

        StreamLayoutEntry raw_entry = entry;
        raw_entry.calibration.type = CalibrationType::None;
        QVariant decoded_raw_value;
        if (!convertRawValue(raw_bits, bit_count, raw_entry, &decoded_raw_value, &local_error)) {
            setError(error_message, local_error);
            return false;
        }

        QVariant converted_value;
        if (!convertRawValue(raw_bits, bit_count, entry, &converted_value, &local_error)) {
            setError(error_message, local_error);
            return false;
        }

        ParameterValue parameter_value;
        parameter_value.task_id = task_id;
        parameter_value.stream_index = layout_table.stream_index;
        parameter_value.parameter_id = entry.parameter_id;
        parameter_value.parameter_name = entry.parameter_name;
        parameter_value.timestamp_seconds = timestamp_seconds;
        parameter_value.raw_value = decoded_raw_value;
        parameter_value.value = converted_value;
        parameter_value.unit = entry.unit;
        values->append(parameter_value);
    }

    return true;
}

void FrameProcessor::waitForFinished()
{
    QList<QFuture<void>> futures;
    {
        QMutexLocker locker(&mutex_);
        futures = futures_;
    }

    for (QFuture<void> &future : futures) {
        future.waitForFinished();
    }
}

void FrameProcessor::start()
{
    stop_requested_.storeRelease(0);

    QList<StreamWorkerContext> contexts;
    QString task_id;
    QString error_message;
    {
        QMutexLocker locker(&mutex_);
        if (running_) {
            error_message = QStringLiteral("FrameProcessor for task '%1' is already running").arg(task_.task_id);
            task_id = task_.task_id;
        } else {
            task_id = task_.task_id;
            ensureResultQueues();
            if (validateTask(&error_message)) {
                const int consumer_count = qMax(1, options_.consumer_count_per_stream);
                const int wait_timeout_ms = qMax(0, options_.wait_timeout_ms);
                const int value_batch_size = qMax(1, options_.value_batch_size);
                const int progress_interval_frames = qMax(1, options_.progress_interval_frames);
                const bool enable_result_queues = options_.enable_result_queues;
                const bool emit_individual_values = options_.emit_individual_values;

                active_stream_workers_.clear();
                stream_processed_frames_.clear();
                stream_produced_values_.clear();
                futures_.clear();
                total_processed_frames_ = 0;
                total_produced_values_ = 0;
                active_worker_count_ = 0;

                for (const StreamLayoutTable &layout_table : task_.layout_tables) {
                    const QString stream_id = layout_table.stream_id;
                    const FrameTaskQueuePtr frame_queue = task_.frame_queues.value(stream_id);
                    const ParameterValueQueuePtr result_queue = task_.result_queues.value(stream_id);
                    active_stream_workers_.insert(stream_id, consumer_count);
                    stream_processed_frames_.insert(stream_id, 0);
                    stream_produced_values_.insert(stream_id, 0);
                    active_worker_count_ += consumer_count;

                    for (int worker_index = 0; worker_index < consumer_count; ++worker_index) {
                        StreamWorkerContext context;
                        context.task_id = task_.task_id;
                        context.stream_index = layout_table.stream_index;
                        context.stream_id = stream_id;
                        context.layout_table = layout_table;
                        context.frame_queue = frame_queue;
                        context.result_queue = result_queue;
                        context.selected_parameter_ids =
                            task_.selected_parameter_ids_by_stream.value(stream_id);
                        context.wait_timeout_ms = wait_timeout_ms;
                        context.value_batch_size = value_batch_size;
                        context.progress_interval_frames = progress_interval_frames;
                        context.worker_index = worker_index;
                        context.enable_result_queues = enable_result_queues;
                        context.emit_individual_values = emit_individual_values;
                        contexts.append(context);
                    }
                }

                running_ = true;
            }
        }
    }

    if (!error_message.isEmpty()) {
        reportFailure(task_id, error_message);
        closeResultQueues();
        emit finished(task_id, 0, 0);
        return;
    }

    emit started(task_id);
    log(LogLevel::Info,
        task_id,
        QString(),
        QStringLiteral("Frame processor started with %1 stream worker(s)").arg(contexts.size()));

    for (const StreamWorkerContext &context : contexts) {
        QFuture<void> future = QtConcurrent::run([this, context]() {
            processStream(context);
        });

        QMutexLocker locker(&mutex_);
        futures_.append(future);
    }
}

void FrameProcessor::requestStop()
{
    stop_requested_.storeRelease(1);
    closeResultQueues();
}

void FrameProcessor::ensureResultQueues()
{
    if (!options_.enable_result_queues) {
        task_.result_queues.clear();
        return;
    }

    for (const StreamLayoutTable &layout_table : task_.layout_tables) {
        if (layout_table.stream_id.trimmed().isEmpty()) {
            continue;
        }

        if (!task_.result_queues.contains(layout_table.stream_id)
            || task_.result_queues.value(layout_table.stream_id).isNull()) {
            task_.result_queues.insert(layout_table.stream_id,
                                       ParameterValueQueuePtr::create(options_.result_queue_max_size));
        }
    }
}

bool FrameProcessor::validateTask(QString *error_message) const
{
    if (task_.task_id.trimmed().isEmpty()) {
        setError(error_message, QStringLiteral("FrameProcessorTask.task_id is empty"));
        return false;
    }

    if (task_.layout_tables.isEmpty()) {
        setError(error_message,
                 QStringLiteral("Task '%1' has no stream layout tables").arg(task_.task_id));
        return false;
    }

    if (options_.consumer_count_per_stream <= 0) {
        setError(error_message, QStringLiteral("consumer_count_per_stream must be > 0"));
        return false;
    }

    if (options_.wait_timeout_ms < 0) {
        setError(error_message, QStringLiteral("wait_timeout_ms must be >= 0"));
        return false;
    }

    QSet<QString> stream_ids;
    QSet<int> stream_indexes;
    for (const StreamLayoutTable &layout_table : task_.layout_tables) {
        const QString stream_id = layout_table.stream_id.trimmed();
        if (stream_id.isEmpty()) {
            setError(error_message,
                     QStringLiteral("Task '%1' contains a layout table with an empty stream_id")
                         .arg(task_.task_id));
            return false;
        }

        if (stream_ids.contains(stream_id)) {
            setError(error_message,
                     QStringLiteral("Task '%1' contains duplicate layout stream_id '%2'")
                         .arg(task_.task_id, stream_id));
            return false;
        }

        if (stream_indexes.contains(layout_table.stream_index)) {
            setError(error_message,
                     QStringLiteral("Task '%1' contains duplicate layout stream_index %2")
                         .arg(task_.task_id)
                         .arg(layout_table.stream_index));
            return false;
        }

        if (layout_table.word_length_bits <= 0 || layout_table.frame_length_words <= 0
            || layout_table.subframe_depth <= 0) {
            setError(error_message,
                     QStringLiteral("Stream '%1' has invalid layout geometry").arg(stream_id));
            return false;
        }

        if (!task_.frame_queues.contains(stream_id) || task_.frame_queues.value(stream_id).isNull()) {
            setError(error_message,
                     QStringLiteral("Task '%1' stream '%2' has no FrameTask queue")
                         .arg(task_.task_id, stream_id));
            return false;
        }

        if (options_.enable_result_queues &&
            (!task_.result_queues.contains(stream_id) || task_.result_queues.value(stream_id).isNull())) {
            setError(error_message,
                     QStringLiteral("Task '%1' stream '%2' has no ParameterValue queue")
                         .arg(task_.task_id, stream_id));
            return false;
        }

        stream_ids.insert(stream_id);
        stream_indexes.insert(layout_table.stream_index);
    }

    return true;
}

void FrameProcessor::closeResultQueues()
{
    StreamParameterValueQueues result_queues;
    {
        QMutexLocker locker(&mutex_);
        result_queues = task_.result_queues;
    }

    for (const ParameterValueQueuePtr &queue : result_queues) {
        if (queue) {
            queue->close();
        }
    }
}

void FrameProcessor::processStream(const StreamWorkerContext &context)
{
    emit streamStarted(context.task_id, context.stream_index, context.stream_id);

    quint64 processed_frame_count = 0;
    quint64 produced_value_count = 0;
    FrameTask frame_task;
    QList<ParameterValue> pending_values;
    pending_values.reserve(context.value_batch_size);

    auto flushPendingValues = [&]() {
        if (pending_values.isEmpty()) {
            return;
        }

        emit parameterBatchReady(pending_values);
        pending_values.clear();
    };

    while (!stop_requested_.loadAcquire()) {
        if (!context.frame_queue->waitAndPop(&frame_task, context.wait_timeout_ms)) {
            if (context.frame_queue->isClosed()) {
                break;
            }
            continue;
        }

        if (frame_task.task_id != context.task_id) {
            reportFailure(context.task_id,
                          QStringLiteral("Dropped frame from task '%1' while processing task '%2'")
                              .arg(frame_task.task_id, context.task_id));
            continue;
        }

        if (frame_task.stream_id != context.stream_id) {
            reportFailure(context.task_id,
                          QStringLiteral("Dropped frame for stream '%1' from processor stream '%2'")
                              .arg(frame_task.stream_id, context.stream_id));
            continue;
        }

        QList<ParameterValue> values;
        QString error_message;
        if (!processFrame(frame_task,
                          context.layout_table,
                          &values,
                          &error_message,
                          context.selected_parameter_ids)) {
            reportFailure(context.task_id, error_message);
            ++processed_frame_count;
            continue;
        }

        for (const ParameterValue &value : values) {
            if (stop_requested_.loadAcquire()) {
                break;
            }

            if (context.enable_result_queues && context.result_queue && !context.result_queue->push(value)) {
                if (!stop_requested_.loadAcquire()) {
                    reportFailure(context.task_id,
                                  QStringLiteral("Result queue for stream '%1' rejected a value")
                                      .arg(context.stream_id));
                }
                break;
            }

            if (context.emit_individual_values) {
                emit parameterReady(value);
            }
            pending_values.append(value);
            if (pending_values.size() >= context.value_batch_size) {
                flushPendingValues();
            }
            ++produced_value_count;
        }

        ++processed_frame_count;
        if ((processed_frame_count % static_cast<quint64>(context.progress_interval_frames)) == 0) {
            emit progressChanged(context.task_id,
                                 context.stream_index,
                                 context.stream_id,
                                 processed_frame_count,
                                 produced_value_count);
        }
    }

    flushPendingValues();
    if (processed_frame_count > 0) {
        emit progressChanged(context.task_id,
                             context.stream_index,
                             context.stream_id,
                             processed_frame_count,
                             produced_value_count);
    }
    noteWorkerFinished(context, processed_frame_count, produced_value_count);
}

void FrameProcessor::noteWorkerFinished(const StreamWorkerContext &context,
                                        quint64 processed_frame_count,
                                        quint64 produced_value_count)
{
    bool stream_done = false;
    bool all_done = false;
    quint64 stream_processed = 0;
    quint64 stream_produced = 0;
    quint64 total_processed = 0;
    quint64 total_produced = 0;

    {
        QMutexLocker locker(&mutex_);
        stream_processed = stream_processed_frames_.value(context.stream_id, 0) + processed_frame_count;
        stream_produced = stream_produced_values_.value(context.stream_id, 0) + produced_value_count;
        stream_processed_frames_.insert(context.stream_id, stream_processed);
        stream_produced_values_.insert(context.stream_id, stream_produced);

        int remaining_stream_workers = active_stream_workers_.value(context.stream_id, 0);
        if (remaining_stream_workers > 0) {
            --remaining_stream_workers;
        }
        if (remaining_stream_workers == 0) {
            active_stream_workers_.remove(context.stream_id);
            stream_done = true;
        } else {
            active_stream_workers_.insert(context.stream_id, remaining_stream_workers);
        }

        if (active_worker_count_ > 0) {
            --active_worker_count_;
        }

        total_processed_frames_ += processed_frame_count;
        total_produced_values_ += produced_value_count;
        total_processed = total_processed_frames_;
        total_produced = total_produced_values_;

        if (active_worker_count_ == 0) {
            running_ = false;
            all_done = true;
        }
    }

    if (stream_done && context.result_queue) {
        context.result_queue->close();
        emit streamFinished(context.task_id,
                            context.stream_index,
                            context.stream_id,
                            stream_processed,
                            stream_produced);
    }

    if (all_done) {
        log(LogLevel::Info,
            context.task_id,
            QString(),
            QStringLiteral("Frame processor finished, frames=%1, values=%2")
                .arg(total_processed)
                .arg(total_produced));
        emit finished(context.task_id, total_processed, total_produced);
    }
}

void FrameProcessor::reportFailure(const QString &task_id, const QString &reason)
{
    log(LogLevel::Error, task_id, QString(), reason);
    emit failed(task_id, reason);
}

void FrameProcessor::log(LogLevel level,
                         const QString &task_id,
                         const QString &parameter_name,
                         const QString &message) const
{
    Logger::instance().log(level, QStringLiteral("FrameProcessor"), task_id, parameter_name, message);
}

bool FrameProcessor::extractRawBitsInternal(const QByteArray &frame_data,
                                            const StreamLayoutTable &layout_table,
                                            const StreamLayoutEntry &entry,
                                            quint64 *raw_bits,
                                            int *bit_count,
                                            QString *error_message)
{
    return extractRawBitsFromData(frame_data.constData(),
                                  frame_data.size(),
                                  layout_table,
                                  entry,
                                  raw_bits,
                                  bit_count,
                                  error_message);
}

bool FrameProcessor::extractRawBitsFromData(const char *frame_data,
                                            qsizetype frame_size,
                                            const StreamLayoutTable &layout_table,
                                            const StreamLayoutEntry &entry,
                                            quint64 *raw_bits,
                                            int *bit_count,
                                            QString *error_message)
{
    if (raw_bits == nullptr || bit_count == nullptr) {
        setError(error_message, QStringLiteral("raw_bits or bit_count output pointer is null"));
        return false;
    }
    if (frame_data == nullptr || frame_size <= 0) {
        setError(error_message, QStringLiteral("frame_data is empty"));
        return false;
    }

    if (entry.word_segments.isEmpty()) {
        setError(error_message,
                 QStringLiteral("Parameter %1 has no active word segments").arg(parameterLabel(entry)));
        return false;
    }

    bool object_bit_assembly = false;
    for (const ParameterWordSegment &segment : entry.word_segments) {
        object_bit_assembly |= segment.isActive() && segment.object_bit != 0;
    }
    int assembled_bit_count = 0;
    quint64 assembled_bits = 0;
    for (int segment_index = 0; segment_index < entry.word_segments.size(); ++segment_index) {
        const ParameterWordSegment &segment = entry.word_segments.at(segment_index);
        if (!segment.isActive()) {
            continue;
        }

        if (segment.bit_length > 64) {
            setError(error_message,
                     QStringLiteral("Parameter %1 segment %2 bit_length=%3 exceeds 64")
                         .arg(parameterLabel(entry))
                         .arg(segment_index)
                         .arg(segment.bit_length));
            return false;
        }

        if (!object_bit_assembly && assembled_bit_count > 64 - segment.bit_length) {
            setError(error_message,
                     QStringLiteral("Parameter %1 segment %2 makes assembled value exceed 64 bits")
                         .arg(parameterLabel(entry))
                         .arg(segment_index));
            return false;
        }

        int frame_index = 0;
        QString local_error;
        if (!normalizeLayoutIndex(segment.frame_no,
                                  layout_table.subframe_depth,
                                  &frame_index,
                                  &local_error)) {
            setError(error_message,
                     QStringLiteral("Parameter %1 segment %2 has invalid frame_no: %3")
                         .arg(parameterLabel(entry))
                         .arg(segment_index)
                         .arg(local_error));
            return false;
        }

        int word_index = 0;
        if (!normalizeLayoutIndex(segment.word_no,
                                  layout_table.frame_length_words,
                                  &word_index,
                                  &local_error)) {
            setError(error_message,
                     QStringLiteral("Parameter %1 segment %2 has invalid word_no: %3")
                         .arg(parameterLabel(entry))
                         .arg(segment_index)
                         .arg(local_error));
            return false;
        }

        if (segment.source_bit < 0 || segment.bit_length > layout_table.word_length_bits
            || segment.source_bit > layout_table.word_length_bits - segment.bit_length) {
            setError(error_message,
                     QStringLiteral("Parameter %1 segment %2 source range is outside word length")
                         .arg(parameterLabel(entry))
                         .arg(segment_index));
            return false;
        }

        const qint64 word_ordinal =
            (static_cast<qint64>(frame_index) * layout_table.frame_length_words) + word_index;
        quint64 segment_value = 0;
        const int read_start = entry.inverse ? 0 : segment.source_bit;
        const int read_length = entry.inverse ? layout_table.word_length_bits : segment.bit_length;
        if (!readBitsFromLittleEndianWordBytes(frame_data,
                                               frame_size,
                                               layout_table.word_length_bits,
                                               word_ordinal,
                                               read_start,
                                               read_length,
                                               &segment_value,
                                               &local_error)) {
            setError(error_message,
                     QStringLiteral("Parameter %1 segment %2 read failed: %3")
                         .arg(parameterLabel(entry))
                         .arg(segment_index)
                         .arg(local_error));
            return false;
        }

        if (entry.inverse) {
            if (layout_table.word_length_bits != 16) {
                setError(error_message, QStringLiteral("旧版 inverse 字节交换要求 16 位 PCM 字"));
                return false;
            }
            segment_value = ((segment_value & 0xff) << 8) | ((segment_value >> 8) & 0xff);
            segment_value = (segment_value >> segment.source_bit) & bitMask(segment.bit_length);
        }
        if (object_bit_assembly) {
            if (segment.object_bit < 0 || segment.object_bit > 63 ||
                segment.bit_length > 64 - segment.object_bit) {
                setError(error_message, QStringLiteral("object_bit 目标范围超出 64 位"));
                return false;
            }
            assembled_bits |= (segment_value & bitMask(segment.bit_length)) << segment.object_bit;
            assembled_bit_count = qMax(assembled_bit_count, segment.object_bit + segment.bit_length);
        } else {
            assembled_bits = segment.bit_length == 64 ? segment_value :
                (assembled_bits << segment.bit_length) | (segment_value & bitMask(segment.bit_length));
            assembled_bit_count += segment.bit_length;
        }
    }

    if (assembled_bit_count <= 0) {
        setError(error_message,
                 QStringLiteral("Parameter %1 produced an empty raw value").arg(parameterLabel(entry)));
        return false;
    }

    *raw_bits = assembled_bits;
    *bit_count = assembled_bit_count;
    return true;
}

bool FrameProcessor::readBitsMsbFirst(const QByteArray &frame_data,
                                      qint64 start_bit,
                                      int bit_length,
                                      quint64 *value,
                                      QString *error_message)
{
    return readBitsMsbFirst(frame_data.constData(),
                            static_cast<qint64>(frame_data.size()) * 8,
                            start_bit,
                            bit_length,
                            value,
                            error_message);
}

bool FrameProcessor::readBitsMsbFirst(const char *frame_data,
                                      qint64 available_bits,
                                      qint64 start_bit,
                                      int bit_length,
                                      quint64 *value,
                                      QString *error_message)
{
    if (value == nullptr) {
        setError(error_message, QStringLiteral("value output pointer is null"));
        return false;
    }
    if (frame_data == nullptr) {
        setError(error_message, QStringLiteral("frame_data is null"));
        return false;
    }

    if (start_bit < 0 || bit_length < 0 || bit_length > 64) {
        setError(error_message,
                 QStringLiteral("Invalid bit range start=%1 length=%2").arg(start_bit).arg(bit_length));
        return false;
    }

    if (start_bit > available_bits || bit_length > available_bits - start_bit) {
        setError(error_message,
                 QStringLiteral("Frame has %1 bits but range [%2, %3) was requested")
                     .arg(available_bits)
                     .arg(start_bit)
                     .arg(start_bit + bit_length));
        return false;
    }

    quint64 extracted_value = 0;
    for (int index = 0; index < bit_length; ++index) {
        const qint64 absolute_bit = start_bit + index;
        const int byte_index = static_cast<int>(absolute_bit / 8);
        const int bit_in_byte = 7 - static_cast<int>(absolute_bit % 8);
        const uchar byte = static_cast<uchar>(frame_data[byte_index]);
        extracted_value = (extracted_value << 1) | ((byte >> bit_in_byte) & 0x01);
    }

    *value = extracted_value;
    return true;
}

bool FrameProcessor::decodeSignedInteger(quint64 raw_bits,
                                         int bit_count,
                                         int sign_bit,
                                         qlonglong *value,
                                         QString *error_message)
{
    if (value == nullptr) {
        setError(error_message, QStringLiteral("value output pointer is null"));
        return false;
    }

    if (bit_count <= 0 || bit_count > 64) {
        setError(error_message, QStringLiteral("Signed integer bit_count must be in [1, 64]"));
        return false;
    }

    int resolved_sign_bit = sign_bit;
    if (resolved_sign_bit <= 0) {
        resolved_sign_bit = bit_count - 1;
    } else if (resolved_sign_bit == bit_count) {
        resolved_sign_bit = bit_count - 1;
    }

    if (resolved_sign_bit < 0 || resolved_sign_bit >= bit_count) {
        setError(error_message,
                 QStringLiteral("sign_bit=%1 is outside assembled bit_count=%2")
                     .arg(sign_bit)
                     .arg(bit_count));
        return false;
    }

    const quint64 width_mask = bitMask(resolved_sign_bit + 1);
    const quint64 value_bits = raw_bits & width_mask;
    const quint64 sign_mask = quint64(1) << resolved_sign_bit;
    if ((value_bits & sign_mask) == 0) {
        *value = static_cast<qlonglong>(value_bits);
        return true;
    }

    if (resolved_sign_bit == 63) {
        const quint64 without_sign = value_bits & (~sign_mask);
        *value = std::numeric_limits<qlonglong>::min() + static_cast<qlonglong>(without_sign);
        return true;
    }

    const quint64 magnitude = ((~value_bits) & width_mask) + quint64(1);
    *value = -static_cast<qlonglong>(magnitude);
    return true;
}

bool FrameProcessor::decodeBcd(quint64 raw_bits, int bit_count, qlonglong *value, QString *error_message)
{
    if (value == nullptr) {
        setError(error_message, QStringLiteral("value output pointer is null"));
        return false;
    }

    if (bit_count <= 0 || bit_count > 64) {
        setError(error_message, QStringLiteral("BCD bit_count must be in [1, 64]"));
        return false;
    }

    const int nibble_count = (bit_count + 3) / 4;
    qlonglong decoded_value = 0;
    for (int nibble_index = nibble_count - 1; nibble_index >= 0; --nibble_index) {
        const int digit = static_cast<int>((raw_bits >> (nibble_index * 4)) & 0x0F);
        if (digit > 9) {
            setError(error_message,
                     QStringLiteral("Invalid BCD digit %1 in raw value 0x%2")
                         .arg(digit)
                         .arg(QString::number(raw_bits, 16)));
            return false;
        }

        if (decoded_value > (std::numeric_limits<qlonglong>::max() - digit) / 10) {
            setError(error_message, QStringLiteral("BCD value overflows qlonglong"));
            return false;
        }

        decoded_value = (decoded_value * 10) + digit;
    }

    *value = decoded_value;
    return true;
}

bool FrameProcessor::decodeFloatingPoint(quint64 raw_bits,
                                         int bit_count,
                                         double *value,
                                         QString *error_message)
{
    if (value == nullptr) {
        setError(error_message, QStringLiteral("value output pointer is null"));
        return false;
    }

    if (bit_count == 32) {
        const quint32 bits = static_cast<quint32>(raw_bits);
        float float_value = 0.0F;
        std::memcpy(&float_value, &bits, sizeof(float_value));
        *value = static_cast<double>(float_value);
        return true;
    }

    if (bit_count == 64) {
        double double_value = 0.0;
        std::memcpy(&double_value, &raw_bits, sizeof(double_value));
        *value = double_value;
        return true;
    }

    setError(error_message,
             QStringLiteral("Floating point parameter requires 32 or 64 bits, got %1").arg(bit_count));
    return false;
}

bool FrameProcessor::applyCalibration(double raw_value,
                                      const CalibrationDefinition &calibration,
                                      double *engineering_value,
                                      bool *calibration_applied,
                                      QString *error_message)
{
    if (engineering_value == nullptr || calibration_applied == nullptr) {
        setError(error_message, QStringLiteral("calibration output pointer is null"));
        return false;
    }

    *engineering_value = raw_value;
    *calibration_applied = false;
    if (!hasEngineeringCalibration(calibration.type)) {
        return true;
    }

    *calibration_applied = true;
    switch (calibration.type) {
    case CalibrationType::Linear:
        if (calibration.coefficients.isEmpty()) {
            *engineering_value = raw_value;
        } else if (calibration.coefficients.size() == 1) {
            *engineering_value = calibration.coefficients.at(0) * raw_value;
        } else {
            *engineering_value = calibration.coefficients.at(0)
                + (calibration.coefficients.at(1) * raw_value);
        }
        return true;

    case CalibrationType::Polynomial: {
        if (calibration.coefficients.isEmpty()) {
            *engineering_value = raw_value;
            return true;
        }

        double result = 0.0;
        double power = 1.0;
        for (const double coefficient : calibration.coefficients) {
            result += coefficient * power;
            power *= raw_value;
        }
        *engineering_value = result;
        return true;
    }

    case CalibrationType::Hyperbolic: {
        if (calibration.coefficients.size() != 8) {
            setError(error_message, QStringLiteral("hyper 校准需要八项系数"));
            return false;
        }
        const int base = raw_value >= calibration.segment_point ? 0 : 4;
        const QList<double> &c = calibration.coefficients;
        const double denominator = c.at(base + 1) + raw_value * c.at(base + 2);
        if (denominator == 0.0) {
            setError(error_message, QStringLiteral("hyper 校准分母为零"));
            return false;
        }
        *engineering_value = c.at(base) / denominator + c.at(base + 3);
        return true;
    }
    case CalibrationType::Segmented: {
        if (calibration.points.isEmpty()) {
            setError(error_message, QStringLiteral("Segmented calibration has no points"));
            return false;
        }

        QList<CalibrationPoint> points = calibration.points;
        std::sort(points.begin(), points.end(), [](const CalibrationPoint &left, const CalibrationPoint &right) {
            return left.raw_value < right.raw_value;
        });

        if (points.size() >= 2 &&
            (raw_value < points.first().raw_value || raw_value > points.last().raw_value)) {
            const int index = raw_value < points.first().raw_value ? 1 : points.size() - 1;
            const CalibrationPoint &left = points.at(index - 1);
            const CalibrationPoint &right = points.at(index);
            if (right.raw_value == left.raw_value) {
                setError(error_message, QStringLiteral("分段校准码值重复"));
                return false;
            }
            *engineering_value = left.engineering_value +
                (raw_value - left.raw_value) * (right.engineering_value - left.engineering_value) /
                (right.raw_value - left.raw_value);
            return true;
        }
        if (points.size() == 1 || raw_value <= points.first().raw_value) {
            *engineering_value = points.first().engineering_value;
            return true;
        }

        if (raw_value >= points.last().raw_value) {
            *engineering_value = points.last().engineering_value;
            return true;
        }

        for (int index = 1; index < points.size(); ++index) {
            const CalibrationPoint &right = points.at(index);
            if (raw_value > right.raw_value) {
                continue;
            }

            const CalibrationPoint &left = points.at(index - 1);
            const double denominator = right.raw_value - left.raw_value;
            if (denominator == 0.0) {
                *engineering_value = right.engineering_value;
                return true;
            }

            const double ratio = (raw_value - left.raw_value) / denominator;
            *engineering_value = left.engineering_value
                + (ratio * (right.engineering_value - left.engineering_value));
            return true;
        }

        *engineering_value = points.last().engineering_value;
        return true;
    }

    case CalibrationType::None:
    case CalibrationType::Unknown:
    default:
        *calibration_applied = false;
        *engineering_value = raw_value;
        return true;
    }
}

bool FrameProcessor::normalizeLayoutIndex(int configured_index,
                                          int item_count,
                                          int *normalized_index,
                                          QString *error_message)
{
    if (normalized_index == nullptr) {
        setError(error_message, QStringLiteral("normalized_index output pointer is null"));
        return false;
    }

    if (item_count <= 0) {
        setError(error_message, QStringLiteral("item_count must be > 0"));
        return false;
    }

    if (configured_index >= 0 && configured_index < item_count) {
        *normalized_index = configured_index;
        return true;
    }

    setError(error_message,
             QStringLiteral("configured_index=%1 is outside zero-based range [0,%2]")
                 .arg(configured_index)
                 .arg(item_count - 1));
    return false;
}
