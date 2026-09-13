#pragma once

#include "thread_safe_queue.h"

#include <QByteArray>
#include <QHash>
#include <QList>
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
    QString source_file_path;
    quint64 frame_index = 0;
    qint64 file_offset = 0;
    quint64 sample_index = 0;
    double timestamp_seconds = 0.0;
    QByteArray frame_data;
};

/**
 * @brief References one frame inside a shared fixed-PCM block without copying frame bytes.
 */
struct FrameBlockReference
{
    QString stream_id;
    int stream_index = 0;
    quint64 frame_index = 0;
    qint64 file_offset = 0;
    qsizetype block_offset = 0;
    qsizetype frame_length = 0;
    quint64 sample_index = 0;
    double timestamp_seconds = 0.0;
};

/**
 * @brief Block-level task used by the fixed-frame fast path.
 */
struct FrameBlockTask
{
    QString task_id;
    qint64 block_file_offset = 0;
    QByteArray block_data;
    QList<FrameBlockReference> frames;
};

using FrameTaskQueue = ThreadSafeQueue<FrameTask>;
using FrameTaskQueuePtr = QSharedPointer<FrameTaskQueue>;
using StreamFrameQueues = QHash<QString, FrameTaskQueuePtr>;
using FrameBlockTaskQueue = ThreadSafeQueue<FrameBlockTask>;
using FrameBlockTaskQueuePtr = QSharedPointer<FrameBlockTaskQueue>;

Q_DECLARE_METATYPE(FrameTask)
Q_DECLARE_METATYPE(FrameBlockReference)
Q_DECLARE_METATYPE(FrameBlockTask)
Q_DECLARE_METATYPE(FrameTaskQueuePtr)
Q_DECLARE_METATYPE(StreamFrameQueues)
Q_DECLARE_METATYPE(FrameBlockTaskQueuePtr)
