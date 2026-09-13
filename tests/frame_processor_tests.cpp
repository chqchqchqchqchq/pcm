#include "frame_processor.h"

#include <QSignalSpy>
#include <QtTest>

#include <initializer_list>

class FrameProcessorTests : public QObject
{
    Q_OBJECT

private slots:
    void extractsBitSegmentsAcrossWordsAndAppliesLinearCalibration();
    void extractsLittleEndianBytesInsidePcmWords();
    void assemblesWordSegmentsByLayoutArrayOrder();
    void rejectsOutOfRangeLayoutIndexes();
    void signExtendsMultiWordParameterBeforePolynomialCalibration();
    void consumesStreamsAsynchronouslyIntoSeparateResultQueues();
};

namespace {

ParameterWordSegment makeSegment(int frame_no,
                                 int word_no,
                                 int source_bit,
                                 int bit_length,
                                 int object_bit)
{
    ParameterWordSegment segment;
    segment.frame_no = frame_no;
    segment.word_no = word_no;
    segment.source_bit = source_bit;
    segment.bit_length = bit_length;
    segment.object_bit = object_bit;
    return segment;
}

StreamLayoutEntry makeEntry(const QString &parameter_id,
                            const QString &parameter_name,
                            const QList<ParameterWordSegment> &segments)
{
    StreamLayoutEntry entry;
    entry.parameter_id = parameter_id;
    entry.parameter_name = parameter_name;
    entry.unit = QStringLiteral("V");
    entry.code_type = ParameterCodeType::UnsignedInteger;
    entry.word_segments = segments;
    return entry;
}

StreamLayoutTable makeLayout(const QString &stream_id, int stream_index)
{
    StreamLayoutTable layout_table;
    layout_table.stream_index = stream_index;
    layout_table.stream_id = stream_id;
    layout_table.word_length_bits = 8;
    layout_table.frame_length_words = 2;
    layout_table.subframe_depth = 1;
    return layout_table;
}

FrameTask makeFrame(const QString &task_id,
                    const QString &stream_id,
                    const QByteArray &frame_data,
                    double timestamp_seconds)
{
    FrameTask frame_task;
    frame_task.task_id = task_id;
    frame_task.stream_id = stream_id;
    frame_task.frame_data = frame_data;
    frame_task.timestamp_seconds = timestamp_seconds;
    return frame_task;
}

QByteArray bytes(std::initializer_list<int> values)
{
    QByteArray data;
    for (const int value : values) {
        data.append(static_cast<char>(value));
    }

    return data;
}

} // namespace

void FrameProcessorTests::extractsBitSegmentsAcrossWordsAndAppliesLinearCalibration()
{
    StreamLayoutTable layout_table = makeLayout(QStringLiteral("S1"), 1);

    StreamLayoutEntry entry = makeEntry(QStringLiteral("P001"),
                                        QStringLiteral("joined"),
                                        {makeSegment(0, 0, 4, 4, 0),
                                         makeSegment(0, 1, 0, 4, 4)});
    entry.calibration.type = CalibrationType::Linear;
    entry.calibration.coefficients = {1.0, 0.5};
    layout_table.entries.append(entry);

    const FrameTask frame_task = makeFrame(QStringLiteral("task-bit"),
                                           QStringLiteral("S1"),
                                           bytes({0xAB, 0xC3}),
                                           12.5);

    QList<ParameterValue> values;
    QString error_message;
    QVERIFY2(FrameProcessor::processFrame(frame_task, layout_table, &values, &error_message),
             qPrintable(error_message));

    QCOMPARE(values.size(), 1);
    QCOMPARE(values.first().task_id, QStringLiteral("task-bit"));
    QCOMPARE(values.first().stream_index, 1);
    QCOMPARE(values.first().parameter_id, QStringLiteral("P001"));
    QCOMPARE(values.first().timestamp_seconds, 12.5);
    QCOMPARE(values.first().value.toDouble(), 30.0); // 低位起算后为 0x3A，工程值 1 + 58 * 0.5
}

void FrameProcessorTests::extractsLittleEndianBytesInsidePcmWords()
{
    StreamLayoutTable layout_table;
    layout_table.stream_index = 1;
    layout_table.stream_id = QStringLiteral("S1");
    layout_table.word_length_bits = 16;
    layout_table.frame_length_words = 2;
    layout_table.subframe_depth = 1;

    layout_table.entries.append(makeEntry(QStringLiteral("P016"),
                                          QStringLiteral("payload16"),
                                          {makeSegment(0, 0, 0, 16, 0)}));
    layout_table.entries.append(makeEntry(QStringLiteral("P032"),
                                          QStringLiteral("payload32"),
                                          {makeSegment(0, 0, 0, 16, 16),
                                           makeSegment(0, 1, 0, 16, 0)}));

    const FrameTask frame_task = makeFrame(QStringLiteral("task-word-endian"),
                                           QStringLiteral("S1"),
                                           bytes({0x34, 0x12, 0xCD, 0xAB}),
                                           0.0);

    QList<ParameterValue> values;
    QString error_message;
    QVERIFY2(FrameProcessor::processFrame(frame_task, layout_table, &values, &error_message),
             qPrintable(error_message));

    QCOMPARE(values.size(), 2);
    QCOMPARE(values.at(0).parameter_id, QStringLiteral("P016"));
    QCOMPARE(values.at(0).value.toULongLong(), static_cast<qulonglong>(0x1234));
    QCOMPARE(values.at(1).parameter_id, QStringLiteral("P032"));
    QCOMPARE(values.at(1).value.toULongLong(), static_cast<qulonglong>(0x1234ABCDULL));
}

void FrameProcessorTests::assemblesWordSegmentsByLayoutArrayOrder()
{
    StreamLayoutTable layout_table;
    layout_table.stream_index = 1;
    layout_table.stream_id = QStringLiteral("S1");
    layout_table.word_length_bits = 8;
    layout_table.frame_length_words = 2;
    layout_table.subframe_depth = 2;

    StreamLayoutEntry entry = makeEntry(QStringLiteral("PSEQ"),
                                        QStringLiteral("array_order"),
                                        {makeSegment(1, 0, 0, 8, 0),
                                         makeSegment(0, 0, 0, 8, 0),
                                         makeSegment(0, 1, 0, 8, 0)});
    layout_table.entries.append(entry);

    const FrameTask frame_task = makeFrame(QStringLiteral("task-array-order"),
                                           QStringLiteral("S1"),
                                           bytes({0x11, 0x22, 0x33, 0x44}),
                                           0.0);

    QList<ParameterValue> values;
    QString error_message;
    QVERIFY2(FrameProcessor::processFrame(frame_task, layout_table, &values, &error_message),
             qPrintable(error_message));

    QCOMPARE(values.size(), 1);
    QCOMPARE(values.first().parameter_id, QStringLiteral("PSEQ"));
    QCOMPARE(values.first().value.toULongLong(), static_cast<qulonglong>(0x331122));
}

void FrameProcessorTests::rejectsOutOfRangeLayoutIndexes()
{
    StreamLayoutTable layout_table;
    layout_table.stream_index = 1;
    layout_table.stream_id = QStringLiteral("S1");
    layout_table.word_length_bits = 8;
    layout_table.frame_length_words = 2;
    layout_table.subframe_depth = 2;

    StreamLayoutEntry entry = makeEntry(QStringLiteral("PMIX"),
                                        QStringLiteral("mixed_index"),
                                        {makeSegment(0, 0, 0, 8, 0),
                                         makeSegment(2, 2, 0, 8, 0)});
    layout_table.entries.append(entry);

    const FrameTask frame_task = makeFrame(QStringLiteral("task-mixed-index"),
                                           QStringLiteral("S1"),
                                           bytes({0x11, 0x22, 0x33, 0x44}),
                                           0.0);

    QList<ParameterValue> values;
    QString error_message;
    QVERIFY(!FrameProcessor::processFrame(frame_task, layout_table, &values, &error_message));
    QVERIFY(error_message.contains(QStringLiteral("outside zero-based range")));
}

void FrameProcessorTests::signExtendsMultiWordParameterBeforePolynomialCalibration()
{
    StreamLayoutTable layout_table = makeLayout(QStringLiteral("S1"), 1);

    StreamLayoutEntry entry = makeEntry(QStringLiteral("P002"),
                                        QStringLiteral("signed_joined"),
                                        {makeSegment(0, 0, 0, 8, 4),
                                         makeSegment(0, 1, 0, 4, 0)});
    entry.code_type = ParameterCodeType::SignedInteger;
    entry.sign_bit = 11;
    entry.calibration.type = CalibrationType::Polynomial;
    entry.calibration.coefficients = {2.0, 3.0};
    layout_table.entries.append(entry);

    const FrameTask frame_task = makeFrame(QStringLiteral("task-sign"),
                                           QStringLiteral("S1"),
                                           bytes({0xFF, 0x0B}),
                                           0.0);

    QList<ParameterValue> values;
    QString error_message;
    QVERIFY2(FrameProcessor::processFrame(frame_task, layout_table, &values, &error_message),
             qPrintable(error_message));

    QCOMPARE(values.size(), 1);
    QCOMPARE(values.first().parameter_id, QStringLiteral("P002"));
    QCOMPARE(values.first().value.toDouble(), -13.0);
}

void FrameProcessorTests::consumesStreamsAsynchronouslyIntoSeparateResultQueues()
{
    const QString task_id = QStringLiteral("task-async");

    StreamLayoutTable first_layout = makeLayout(QStringLiteral("S1"), 1);
    first_layout.frame_length_words = 1;
    first_layout.entries.append(makeEntry(QStringLiteral("A"),
                                          QStringLiteral("stream_a"),
                                          {makeSegment(0, 0, 0, 8, 0)}));

    StreamLayoutTable second_layout = makeLayout(QStringLiteral("S2"), 2);
    second_layout.frame_length_words = 1;
    second_layout.entries.append(makeEntry(QStringLiteral("B"),
                                           QStringLiteral("stream_b"),
                                           {makeSegment(0, 0, 0, 8, 0)}));

    const FrameTaskQueuePtr first_frame_queue = FrameTaskQueuePtr::create();
    const FrameTaskQueuePtr second_frame_queue = FrameTaskQueuePtr::create();

    FrameProcessorTask task;
    task.task_id = task_id;
    task.layout_tables = {first_layout, second_layout};
    task.frame_queues.insert(QStringLiteral("S1"), first_frame_queue);
    task.frame_queues.insert(QStringLiteral("S2"), second_frame_queue);
    task.result_queues = FrameProcessor::createResultQueues(task.layout_tables);

    FrameProcessorOptions options;
    options.consumer_count_per_stream = 1;
    options.wait_timeout_ms = 10;

    FrameProcessor processor(task, options);
    QSignalSpy failed_spy(&processor, &FrameProcessor::failed);
    QSignalSpy finished_spy(&processor, &FrameProcessor::finished);

    processor.start();

    QVERIFY(first_frame_queue->push(makeFrame(QStringLiteral("other-task"),
                                              QStringLiteral("S1"),
                                              bytes({0x99}),
                                              0.0)));
    QVERIFY(first_frame_queue->push(makeFrame(task_id, QStringLiteral("S2"), bytes({0x88}), 0.0)));
    QVERIFY(first_frame_queue->push(makeFrame(task_id, QStringLiteral("S1"), bytes({0x12}), 1.0)));
    QVERIFY(second_frame_queue->push(makeFrame(task_id, QStringLiteral("S2"), bytes({0x34}), 2.0)));
    first_frame_queue->close();
    second_frame_queue->close();

    if (finished_spy.count() == 0) {
        QVERIFY(finished_spy.wait(5000));
    }
    while (failed_spy.count() < 2) {
        QVERIFY(failed_spy.wait(5000));
    }

    QCOMPARE(finished_spy.count(), 1);
    QCOMPARE(failed_spy.count(), 2);

    const QList<QVariant> finished_args = finished_spy.takeFirst();
    QCOMPARE(finished_args.at(0).toString(), task_id);
    QCOMPARE(finished_args.at(1).toULongLong(), static_cast<qulonglong>(2));
    QCOMPARE(finished_args.at(2).toULongLong(), static_cast<qulonglong>(2));

    ParameterValue value;
    const ParameterValueQueuePtr first_result_queue = task.result_queues.value(QStringLiteral("S1"));
    QVERIFY(first_result_queue->pop(&value));
    QCOMPARE(value.task_id, task_id);
    QCOMPARE(value.stream_index, 1);
    QCOMPARE(value.parameter_id, QStringLiteral("A"));
    QCOMPARE(value.value.toULongLong(), static_cast<qulonglong>(0x12));
    QVERIFY(!first_result_queue->pop(&value));
    QVERIFY(first_result_queue->isClosed());

    const ParameterValueQueuePtr second_result_queue = task.result_queues.value(QStringLiteral("S2"));
    QVERIFY(second_result_queue->pop(&value));
    QCOMPARE(value.task_id, task_id);
    QCOMPARE(value.stream_index, 2);
    QCOMPARE(value.parameter_id, QStringLiteral("B"));
    QCOMPARE(value.value.toULongLong(), static_cast<qulonglong>(0x34));
    QVERIFY(!second_result_queue->pop(&value));
    QVERIFY(second_result_queue->isClosed());
}

QTEST_MAIN(FrameProcessorTests)

#include "frame_processor_tests.moc"
