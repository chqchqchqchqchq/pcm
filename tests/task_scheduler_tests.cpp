#include "task_scheduler.h"

#include <QFile>
#include <QSignalSpy>
#include <QTemporaryDir>
#include <QtTest>

class TaskSchedulerTests : public QObject
{
    Q_OBJECT

private slots:
    /**
     * @brief Verifies submission, one-task concurrency, independent queues, and lifecycle completion.
     */
    void runsSubmittedTasksWithBoundedActiveReaders();
};

namespace {

/**
 * @brief Creates a fixed-length stream definition used by the scheduler smoke test.
 */
StreamDefinition makeStreamDefinition()
{
    QByteArray sync_word;
    sync_word.append(static_cast<char>(0xAA));
    sync_word.append(static_cast<char>(0x55));

    StreamDefinition stream;
    stream.stream_id = QStringLiteral("S1");
    stream.sync_word = sync_word;
    stream.frame_length_bytes = 5;
    stream.samples_per_frame = 2;
    return stream;
}

/**
 * @brief Writes a tiny noisy raw file that RawFileReader can demultiplex by sync word.
 */
bool writeRawFile(const QString &file_path, const QList<QByteArray> &frames)
{
    QFile file(file_path);
    if (!file.open(QIODevice::WriteOnly)) {
        return false;
    }

    QByteArray raw_data = QByteArrayLiteral("noise");
    for (const QByteArray &frame : frames) {
        raw_data.append(frame);
    }

    return file.write(raw_data) == raw_data.size();
}

/**
 * @brief Creates the user-facing task definition submitted to TaskScheduler.
 */
TaskDefinition makeTask(const QString &task_id,
                        const QString &config_file_path,
                        const QString &raw_file_path)
{
    TaskDefinition definition;
    definition.task_id = task_id;
    definition.config_file_path = config_file_path;
    definition.raw_file_path = raw_file_path;
    definition.user_sample_rate_hz = 1000.0;
    return definition;
}

} // namespace

void TaskSchedulerTests::runsSubmittedTasksWithBoundedActiveReaders()
{
    QTemporaryDir temp_dir;
    QVERIFY(temp_dir.isValid());

    const StreamDefinition stream = makeStreamDefinition();
    const QByteArray first_frame = stream.sync_word + QByteArrayLiteral("A01");
    const QByteArray second_frame = stream.sync_word + QByteArrayLiteral("A02");
    const QByteArray third_frame = stream.sync_word + QByteArrayLiteral("B01");

    const QString first_raw_path = temp_dir.filePath(QStringLiteral("first.pcm"));
    const QString second_raw_path = temp_dir.filePath(QStringLiteral("second.pcm"));
    QVERIFY(writeRawFile(first_raw_path, {first_frame, second_frame}));
    QVERIFY(writeRawFile(second_raw_path, {third_frame}));

    TaskScheduler scheduler;
    scheduler.setMaxActiveTasks(1);

    RawFileReaderOptions reader_options;
    reader_options.segment_size_bytes = 4;
    reader_options.enable_premap = true;
    reader_options.premap_segment_count = 1;
    scheduler.setReaderOptions(reader_options);

    const QString config_path = temp_dir.filePath(QStringLiteral("config.cfte"));
    scheduler.registerConfigStreams(config_path, {stream});

    QHash<QString, StreamFrameQueues> queues_by_task;
    int max_active_seen = 0;
    QObject::connect(&scheduler,
                     &TaskScheduler::taskQueuesReady,
                     &scheduler,
                     [&](const TaskDefinition &definition,
                         const QList<StreamDefinition> &streams,
                         const StreamFrameQueues &frame_queues) {
                         QCOMPARE(streams.size(), 1);
                         queues_by_task.insert(definition.task_id, frame_queues);
                     });
    QObject::connect(&scheduler, &TaskScheduler::activeTaskCountChanged, &scheduler, [&](int active_count) {
        max_active_seen = qMax(max_active_seen, active_count);
        QVERIFY(active_count <= 1);
    });

    QSignalSpy failed_spy(&scheduler, &TaskScheduler::taskFailed);
    QSignalSpy finished_spy(&scheduler, &TaskScheduler::taskFinished);
    QSignalSpy all_finished_spy(&scheduler, &TaskScheduler::allTasksFinished);

    QString error_message;
    QVERIFY2(scheduler.submitTask(makeTask(QStringLiteral("task-1"), config_path, first_raw_path), &error_message),
             qPrintable(error_message));
    QVERIFY2(scheduler.submitTask(makeTask(QStringLiteral("task-2"), config_path, second_raw_path), &error_message),
             qPrintable(error_message));

    while (finished_spy.count() < 2) {
        QVERIFY(finished_spy.wait(5000));
    }
    while (all_finished_spy.count() < 1) {
        QVERIFY(all_finished_spy.wait(5000));
    }

    QCOMPARE(failed_spy.count(), 0);
    QCOMPARE(finished_spy.count(), 2);
    QCOMPARE(all_finished_spy.count(), 1);
    QCOMPARE(max_active_seen, 1);
    QCOMPARE(queues_by_task.size(), 2);

    FrameTask frame;
    const StreamFrameQueues first_queues = queues_by_task.value(QStringLiteral("task-1"));
    QVERIFY(first_queues.contains(stream.stream_id));
    QVERIFY(first_queues.value(stream.stream_id)->pop(&frame));
    QCOMPARE(frame.task_id, QStringLiteral("task-1"));
    QCOMPARE(frame.frame_data, first_frame);
    QVERIFY(first_queues.value(stream.stream_id)->pop(&frame));
    QCOMPARE(frame.task_id, QStringLiteral("task-1"));
    QCOMPARE(frame.frame_data, second_frame);
    QVERIFY(!first_queues.value(stream.stream_id)->pop(&frame));
    QVERIFY(first_queues.value(stream.stream_id)->isClosed());

    const StreamFrameQueues second_queues = queues_by_task.value(QStringLiteral("task-2"));
    QVERIFY(second_queues.contains(stream.stream_id));
    QVERIFY(second_queues.value(stream.stream_id)->pop(&frame));
    QCOMPARE(frame.task_id, QStringLiteral("task-2"));
    QCOMPARE(frame.frame_data, third_frame);
    QVERIFY(!second_queues.value(stream.stream_id)->pop(&frame));
    QVERIFY(second_queues.value(stream.stream_id)->isClosed());
}

QTEST_MAIN(TaskSchedulerTests)

#include "task_scheduler_tests.moc"
