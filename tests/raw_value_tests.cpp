#include "frame_processor.h"
#include "result_writer.h"

#include <QFile>
#include <QTemporaryDir>
#include <QtTest>

#include <initializer_list>

class RawValueTests : public QObject
{
    Q_OBJECT

private slots:
    void keepsDecodedValueBeforeEngineeringCalibration();
    void decodesSignedBcdAndFloatingRawValues();
    void writesRawThenEngineeringColumns();
};

namespace {

QByteArray bytes(std::initializer_list<int> values)
{
    QByteArray result;
    for (int value : values) {
        result.append(static_cast<char>(value));
    }
    return result;
}

ParameterWordSegment segment(int word_no, int bit_length, int object_bit = 0)
{
    ParameterWordSegment result;
    result.frame_no = 0;
    result.word_no = word_no;
    result.source_bit = 0;
    result.bit_length = bit_length;
    result.object_bit = object_bit;
    return result;
}

ParameterValue decodeOne(const QByteArray &frame_data,
                         StreamLayoutEntry entry,
                         int word_length_bits,
                         int frame_length_words)
{
    StreamLayoutTable layout;
    layout.stream_index = 1;
    layout.stream_id = QStringLiteral("S1");
    layout.word_length_bits = word_length_bits;
    layout.frame_length_words = frame_length_words;
    layout.subframe_depth = 1;
    layout.entries.append(entry);

    FrameTask frame;
    frame.task_id = QStringLiteral("raw-value-task");
    frame.stream_id = layout.stream_id;
    frame.frame_data = frame_data;

    QList<ParameterValue> values;
    QString error_message;
    if (!FrameProcessor::processFrame(frame, layout, &values, &error_message)) {
        qWarning().noquote() << QStringLiteral("decode failed: %1").arg(error_message);
        return {};
    }
    if (values.isEmpty()) {
        return {};
    }
    return values.first();
}

StreamLayoutEntry entry(const QString &id, ParameterCodeType code_type, const QList<ParameterWordSegment> &segments)
{
    StreamLayoutEntry result;
    result.parameter_id = id;
    result.parameter_name = id;
    result.code_type = code_type;
    result.word_segments = segments;
    return result;
}

} // namespace

void RawValueTests::keepsDecodedValueBeforeEngineeringCalibration()
{
    StreamLayoutEntry parameter = entry(QStringLiteral("UINT"),
                                        ParameterCodeType::UnsignedInteger,
                                        {segment(0, 8)});
    parameter.calibration.type = CalibrationType::Linear;
    parameter.calibration.coefficients = {1.0, 2.0};

    const ParameterValue value = decodeOne(bytes({10}), parameter, 8, 1);
    QCOMPARE(value.raw_value.toULongLong(), static_cast<qulonglong>(10));
    QCOMPARE(value.value.toDouble(), 21.0);
}

void RawValueTests::decodesSignedBcdAndFloatingRawValues()
{
    StreamLayoutEntry signed_parameter = entry(QStringLiteral("SIGNED"),
                                               ParameterCodeType::SignedInteger,
                                               {segment(0, 8)});
    signed_parameter.sign_bit = 7;
    const ParameterValue signed_value = decodeOne(bytes({0xFB}), signed_parameter, 8, 1);
    QCOMPARE(signed_value.raw_value.toLongLong(), static_cast<qlonglong>(-5));
    QCOMPARE(signed_value.value.toLongLong(), static_cast<qlonglong>(-5));

    const ParameterValue bcd_value = decodeOne(
        bytes({0x42}),
        entry(QStringLiteral("BCD"), ParameterCodeType::Bcd, {segment(0, 8)}),
        8,
        1);
    QCOMPARE(bcd_value.raw_value.toULongLong(), static_cast<qulonglong>(42));
    QCOMPARE(bcd_value.value.toULongLong(), static_cast<qulonglong>(42));

    const ParameterValue float_value = decodeOne(
        bytes({0x00, 0x00, 0xC0, 0x3F}),
        entry(QStringLiteral("FLOAT"),
              ParameterCodeType::FloatingPoint,
              {segment(0, 32)}),
        32,
        1);
    QVERIFY(qAbs(float_value.raw_value.toDouble() - 1.5) < 0.000001);
    QVERIFY(qAbs(float_value.value.toDouble() - 1.5) < 0.000001);
}

void RawValueTests::writesRawThenEngineeringColumns()
{
    QTemporaryDir temp_dir;
    QVERIFY(temp_dir.isValid());
    const QString output_path = temp_dir.filePath(QStringLiteral("raw_columns.txt"));

    TaskDefinition task;
    task.task_id = QStringLiteral("raw-writer-task");
    task.output_directory = temp_dir.path();
    ParameterGroupSelection group;
    group.stream_index = 1;
    group.parameter_ids = {QStringLiteral("P1")};
    task.parameter_groups.append(group);

    ResultWriter writer;
    ResultWriterOptions options;
    options.output_file_path = output_path;
    options.include_raw_values = true;
    options.timestamp_precision = 3;
    writer.setOptions(options);

    QString error_message;
    QVERIFY2(writer.startTask(task, &error_message), qPrintable(error_message));
    ParameterValue value;
    value.task_id = task.task_id;
    value.stream_index = 1;
    value.parameter_id = QStringLiteral("P1");
    value.parameter_name = QStringLiteral("TEMP");
    value.timestamp_seconds = 1.25;
    value.raw_value = 10;
    value.value = 21;
    QVERIFY2(writer.appendValue(value, &error_message), qPrintable(error_message));

    QString finished_path;
    QVERIFY2(writer.finishTask(task.task_id, &finished_path, &error_message), qPrintable(error_message));
    QCOMPARE(finished_path, QFileInfo(output_path).absoluteFilePath());

    QFile file(finished_path);
    QVERIFY(file.open(QIODevice::ReadOnly | QIODevice::Text));
    QString content = QString::fromUtf8(file.readAll());
    content.replace(QStringLiteral("\r\n"), QStringLiteral("\n"));
    const QStringList lines = content.trimmed().split(QLatin1Char('\n'));
    QCOMPARE(lines.at(0), QStringLiteral("timestamp_seconds\tTEMP_raw\tTEMP"));
    QCOMPARE(lines.at(1), QStringLiteral("1.250\t10\t21"));
}

QTEST_MAIN(RawValueTests)

#include "raw_value_tests.moc"
