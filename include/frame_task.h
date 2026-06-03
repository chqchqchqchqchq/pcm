#pragma once

#include "thread_safe_queue.h"

#include <QByteArray>
#include <QHash>
#include <QMetaType>
#include <QSharedPointer>
#include <QString>

/**
 * @brief Lightweight frame task produced by RawFileReader and consumed by frame processors.
 *
 * The payload uses QByteArray implicit sharing, while task_id and stream_id route the frame to task-local processing
 * and output. file_offset preserves the original byte location for diagnostics and replay.
 */
struct FrameTask
{
    QString task_id;
    QString stream_id;
    quint64 frame_index = 0;
    qint64 file_offset = 0;
    quint64 sample_index = 0;
    double timestamp_seconds = 0.0;
    QByteArray frame_data;
};

using FrameTaskQueue = ThreadSafeQueue<FrameTask>;
using FrameTaskQueuePtr = QSharedPointer<FrameTaskQueue>;
using StreamFrameQueues = QHash<QString, FrameTaskQueuePtr>;

Q_DECLARE_METATYPE(FrameTask)
Q_DECLARE_METATYPE(FrameTaskQueuePtr)
Q_DECLARE_METATYPE(StreamFrameQueues)
