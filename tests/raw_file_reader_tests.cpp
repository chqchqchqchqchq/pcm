#include "raw_file_reader.h"

#include <QFile>
#include <QFileInfo>
#include <QSignalSpy>
#include <QTemporaryDir>
#include <QtTest>

class RawFileReaderTests : public QObject
{
    Q_OBJECT

private slots:
    void demuxesFramesAcrossMappedSegmentBoundaries();
    void readsSeparateRawFileForEachStream();
    void rejectsAmbiguousSyncWords();
};

void RawFileReaderTests::demuxesFramesAcrossMappedSegmentBoundaries()
{
    QTemporaryDir temp_dir;
    QVERIFY(temp_dir.isValid());

    QByteArray sync_a;
    sync_a.append(static_cast<char>(0xAA));
    sync_a.append(static_cast<char>(0x55));

    QByteArray sync_b;
    sync_b.append(static_cast<char>(0xCC));
    sync_b.append(static_cast<char>(0x33));

    const QByteArray frame_a_0 = sync_a + QByteArrayLiteral("ABCD");
    const QByteArray frame_a_1 = sync_a + QByteArrayLiteral("EFGH");
    const QByteArray frame_b_0 = sync_b + QByteArrayLiteral("XYZ");

    QByteArray raw_data;
    raw_data.append(QByteArrayLiteral("xxxx"));
    raw_data.append(frame_a_0);
    raw_data.append(QByteArrayLiteral("nn"));
    raw_data.append(frame_b_0);
    raw_data.append(frame_a_1);

    const QString file_path = temp_dir.filePath(QStringLiteral("raw.pcm"));
    QFile file(file_path);
    QVERIFY(file.open(QIODevice::WriteOnly));
    QCOMPARE(file.write(raw_data), static_cast<qint64>(raw_data.size()));
    file.close();

    const FrameTaskQueuePtr queue_a = FrameTaskQueuePtr::create();
    const FrameTaskQueuePtr queue_b = FrameTaskQueuePtr::create();

    StreamDefinition stream_a;
    stream_a.stream_id = QStringLiteral("A");
    stream_a.sync_word = sync_a;
    stream_a.frame_length_bytes = frame_a_0.size();
    stream_a.samples_per_frame = 8;

    StreamDefinition stream_b;
    stream_b.stream_id = QStringLiteral("B");
    stream_b.sync_word = sync_b;
    stream_b.frame_length_bytes = frame_b_0.size();
    stream_b.samples_per_frame = 4;

    RawReadTask task;
    task.task_id = QStringLiteral("task-001");
    task.raw_file_path = file_path;
    task.sample_rate_hz = 1000.0;
    task.streams = {stream_a, stream_b};
    task.frame_queues.insert(stream_a.stream_id, queue_a);
    task.frame_queues.insert(stream_b.stream_id, queue_b);

    RawFileReaderOptions options;
    options.segment_size_bytes = 5;
    options.enable_premap = true;
    options.premap_segment_count = 1;

    RawFileReader reader(task, options);
    QSignalSpy failed_spy(&reader, &RawFileReader::failed);
    QSignalSpy finished_spy(&reader, &RawFileReader::finished);

    reader.readFrames();

    QCOMPARE(failed_spy.count(), 0);
    QCOMPARE(finished_spy.count(), 1);

    FrameTask frame;
    QVERIFY(queue_a->pop(&frame));
    QCOMPARE(frame.task_id, task.task_id);
    QCOMPARE(frame.stream_id, stream_a.stream_id);
    QCOMPARE(frame.frame_index, static_cast<quint64>(0));
    QCOMPARE(frame.file_offset, static_cast<qint64>(4));
    QCOMPARE(frame.frame_data, frame_a_0);
    QCOMPARE(frame.sample_index, static_cast<quint64>(0));

    QVERIFY(queue_a->pop(&frame));
    QCOMPARE(frame.stream_id, stream_a.stream_id);
    QCOMPARE(frame.frame_index, static_cast<quint64>(1));
    QCOMPARE(frame.frame_data, frame_a_1);
    QCOMPARE(frame.sample_index, static_cast<quint64>(8));
    QVERIFY(qFuzzyCompare(frame.timestamp_seconds + 1.0, 1.008));
    QVERIFY(!queue_a->pop(&frame));

    QVERIFY(queue_b->pop(&frame));
    QCOMPARE(frame.stream_id, stream_b.stream_id);
    QCOMPARE(frame.frame_index, static_cast<quint64>(0));
    QCOMPARE(frame.frame_data, frame_b_0);
    QVERIFY(!queue_b->pop(&frame));
}

void RawFileReaderTests::readsSeparateRawFileForEachStream()
{
    QTemporaryDir temp_dir;
    QVERIFY(temp_dir.isValid());

    QByteArray sync_a;
    sync_a.append(static_cast<char>(0xA1));

    QByteArray sync_b;
    sync_b.append(static_cast<char>(0xB2));

    const QByteArray frame_a = sync_a + QByteArrayLiteral("A0");
    const QByteArray frame_b = sync_b + QByteArrayLiteral("B0");

    const QString first_file_path = temp_dir.filePath(QStringLiteral("data.pcm"));
    const QString second_file_path = temp_dir.filePath(QStringLiteral("data_2.pcm"));

    QFile first_file(first_file_path);
    QVERIFY(first_file.open(QIODevice::WriteOnly));
    QCOMPARE(first_file.write(frame_a), static_cast<qint64>(frame_a.size()));
    first_file.close();

    QFile second_file(second_file_path);
    QVERIFY(second_file.open(QIODevice::WriteOnly));
    QCOMPARE(second_file.write(frame_b), static_cast<qint64>(frame_b.size()));
    second_file.close();

    const FrameTaskQueuePtr queue_a = FrameTaskQueuePtr::create();
    const FrameTaskQueuePtr queue_b = FrameTaskQueuePtr::create();

    StreamDefinition stream_a;
    stream_a.stream_index = 1;
    stream_a.stream_id = QStringLiteral("S1");
    stream_a.sync_word = sync_a;
    stream_a.frame_length_bytes = frame_a.size();

    StreamDefinition stream_b;
    stream_b.stream_index = 2;
    stream_b.stream_id = QStringLiteral("S2");
    stream_b.sync_word = sync_b;
    stream_b.frame_length_bytes = frame_b.size();

    RawReadTask task;
    task.task_id = QStringLiteral("task-multi-file");
    task.raw_file_path = first_file_path;
    task.raw_file_paths_by_stream.insert(stream_a.stream_id, rawFilePathForStream(first_file_path, 1));
    task.raw_file_paths_by_stream.insert(stream_b.stream_id, rawFilePathForStream(first_file_path, 2));
    task.streams = {stream_a, stream_b};
    task.frame_queues.insert(stream_a.stream_id, queue_a);
    task.frame_queues.insert(stream_b.stream_id, queue_b);

    RawFileReaderOptions options;
    options.segment_size_bytes = 2;

    RawFileReader reader(task, options);
    QSignalSpy failed_spy(&reader, &RawFileReader::failed);
    reader.readFrames();

    QCOMPARE(failed_spy.count(), 0);

    FrameTask frame;
    QVERIFY(queue_a->pop(&frame));
    QCOMPARE(frame.stream_id, stream_a.stream_id);
    QCOMPARE(frame.frame_data, frame_a);
    QCOMPARE(frame.source_file_path, QFileInfo(first_file_path).absoluteFilePath());
    QVERIFY(!queue_a->pop(&frame));

    QVERIFY(queue_b->pop(&frame));
    QCOMPARE(frame.stream_id, stream_b.stream_id);
    QCOMPARE(frame.frame_data, frame_b);
    QCOMPARE(frame.source_file_path, QFileInfo(second_file_path).absoluteFilePath());
    QVERIFY(!queue_b->pop(&frame));
}

void RawFileReaderTests::rejectsAmbiguousSyncWords()
{
    QTemporaryDir temp_dir;
    QVERIFY(temp_dir.isValid());

    const QString file_path = temp_dir.filePath(QStringLiteral("raw.pcm"));
    QFile file(file_path);
    QVERIFY(file.open(QIODevice::WriteOnly));
    QVERIFY(file.write(QByteArrayLiteral("ABCDEF")) > 0);
    file.close();

    const FrameTaskQueuePtr queue_a = FrameTaskQueuePtr::create();
    const FrameTaskQueuePtr queue_b = FrameTaskQueuePtr::create();

    StreamDefinition stream_a;
    stream_a.stream_id = QStringLiteral("A");
    stream_a.sync_word = QByteArrayLiteral("AB");
    stream_a.frame_length_bytes = 4;

    StreamDefinition stream_b;
    stream_b.stream_id = QStringLiteral("B");
    stream_b.sync_word = QByteArrayLiteral("ABC");
    stream_b.frame_length_bytes = 5;

    RawReadTask task;
    task.task_id = QStringLiteral("task-ambiguous");
    task.raw_file_path = file_path;
    task.streams = {stream_a, stream_b};
    task.frame_queues.insert(stream_a.stream_id, queue_a);
    task.frame_queues.insert(stream_b.stream_id, queue_b);

    RawFileReader reader(task, RawFileReaderOptions());
    QSignalSpy failed_spy(&reader, &RawFileReader::failed);

    reader.readFrames();

    QCOMPARE(failed_spy.count(), 1);
    QVERIFY(queue_a->isClosed());
    QVERIFY(queue_b->isClosed());
}

QTEST_MAIN(RawFileReaderTests)

#include "raw_file_reader_tests.moc"
