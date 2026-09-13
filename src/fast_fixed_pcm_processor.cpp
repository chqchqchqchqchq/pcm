#include "fast_fixed_pcm_processor.h"
#include "legacy_pcm_decoder.h"

#include <QFile>
#include <exception>

FastFixedPcmProcessor::FastFixedPcmProcessor(const FastFixedPcmTask &task,
                                           const FastFixedPcmProcessorOptions &options, QObject *parent)
    : QObject(parent), task_(task), options_(options)
{
    qRegisterMetaType<FastFixedPcmProcessorOptions>("FastFixedPcmProcessorOptions");
    qRegisterMetaType<FastFixedPcmTask>("FastFixedPcmTask");
}

bool FastFixedPcmProcessor::canUseFastPath(const TaskDefinition &definition,
                                          const QList<StreamDefinition> &streams,
                                          const QList<StreamLayoutTable> &layout_tables,
                                          const QHash<QString, QSet<QString>> &selected_ids,
                                          QString *reason, qint64 resync_window_bytes)
{
    if (streams.isEmpty() || layout_tables.isEmpty() || resync_window_bytes <= 0) {
        setError(reason, QStringLiteral("旧版长周处理缺少有效的流布局或重同步窗口"));
        return false;
    }
    int count = 0;
    for (const StreamLayoutTable &layout : layout_tables) {
        if (!selected_ids.isEmpty() && !selected_ids.contains(layout.stream_id)) {
            continue;
        }
        if (!LegacyPcmDecoder::validate(layout, definition.user_sample_rate_hz,
                                       selected_ids.value(layout.stream_id), reason)) {
            return false;
        }
        bool found = false;
        for (const StreamDefinition &stream : streams) {
            found |= stream.stream_id == layout.stream_id && !stream.sync_word.isEmpty();
        }
        if (!found) {
            setError(reason, QStringLiteral("缺少流 %1 的同步字").arg(layout.stream_id));
            return false;
        }
        QFile file(rawFilePathForStream(definition.raw_file_path, layout.stream_index));
        if (!file.open(QIODevice::ReadOnly)) {
            setError(reason, file.errorString());
            return false;
        }
        ++count;
    }
    if (count == 0) {
        setError(reason, QStringLiteral("没有选中的输出流"));
        return false;
    }
    return true;
}

void FastFixedPcmProcessor::process()
{
    try {
        processLegacy();
    } catch (const std::exception &exception) {
        const QString reason = QStringLiteral("旧版处理异常：%1").arg(QString::fromUtf8(exception.what()));
        log(LogLevel::Error, reason);
        emit failed(task_.definition.task_id, reason);
    } catch (...) {
        const QString reason = QStringLiteral("旧版处理发生未知异常");
        log(LogLevel::Error, reason);
        emit failed(task_.definition.task_id, reason);
    }
}

void FastFixedPcmProcessor::requestStop()
{
    stop_requested_.storeRelease(1);
}

void FastFixedPcmProcessor::log(LogLevel level, const QString &message) const
{
    Logger::instance().log(level, QStringLiteral("LegacyPcmProcessor"), task_.definition.task_id, QString(), message);
}

void FastFixedPcmProcessor::setError(QString *error_message, const QString &message)
{
    if (error_message != nullptr) {
        *error_message = message;
    }
}
