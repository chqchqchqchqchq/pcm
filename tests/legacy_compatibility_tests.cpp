#include "legacy_test_data.h"
#include "legacy_pcm_decoder.h"
#include "fast_fixed_pcm_processor.h"
#include "result_writer.h"
#include <QTextCodec>
#include <QTemporaryDir>
#include <QtTest>

/** @brief 基于旧源码和手算结果的格式、校准、位拼接和采样回归。 */
class LegacyCompatibilityTests : public QObject
{
    Q_OBJECT
private slots:
    void importsAndExportsOldFormat();
    void loadsGbkAndPreservesCalibration();
    void validatesCalibrationData();
    void decodesEquipmentTimes_data();
    void decodesEquipmentTimes();
    void rejectsInvalidTime();
    void discardsUncommittedOutputOnDestruction();
    void appliesObjectBitsAndByteSwap();
    void appliesLegacyCalibration();
    void samplesAndHoldsByOccurrences();
    void downsamplingKeepsRecordedTime();
    void rejectsMissingTimeAndInvalidRates();
    /** @brief 预计算路径对照通用路径，覆盖采样、补点、拼字及原码开关。 */
    void preparedDecoderMatchesReference_data();
    void preparedDecoderMatchesReference();
    /** @brief 复用计划仍拒绝短帧、非法时间及重新准备失败后的调用。 */
    void preparedDecoderRejectsInvalidInput();
    /** @brief 4 KiB 预读边界跨帧时保留短尾，恢复同步，并在取消时丢弃未提交结果。 */
    void prefetchedBlocksPreserveTailsAndCancellation();
};

void LegacyCompatibilityTests::importsAndExportsOldFormat()
{
    QString error;
    ConfigFileDocument doc;
    QVERIFY2(doc.loadFromText(LegacyTestData::config(), QString(), &error), qPrintable(error));
    QCOMPARE(doc.parameters().size(), 5);
    const QString output = doc.toText(&error);
    QVERIFY(output.contains("\nthe_parameter_description_of_stream 1:\n"));
    QVERIFY(output.contains("\nOnboard_acq._equipments_type: DM6\n"));
    QVERIFY(output.contains("                 Description: fixture"));
    QVERIFY(!output.contains("the_parameter_description_of_stream_1"));
    ConfigFileDocument reload;
    QVERIFY2(reload.loadFromText(output, QString(), &error), qPrintable(error));
    QCOMPARE(reload.parameters().last().name(), QString("TEMP"));
    QString alias = output;
    alias.replace("the_parameter_description_of_stream 1", "the_parameter_description_of_stream_1");
    QVERIFY(reload.loadFromText(alias));
    QVERIFY(reload.toText().contains("\nthe_parameter_description_of_stream 1:\n"));
}

void LegacyCompatibilityTests::loadsGbkAndPreservesCalibration()
{
    QTemporaryDir dir;
    QString input = LegacyTestData::config(1, 1, "segm",
        " segment_number: 3\n code,phy: 0,0\n code,phy: 10,100\n code,phy: 20,300\n");
    input.replace("fixture", QStringLiteral("温度校准（保留说明）"));
    const QString path = dir.filePath("old.hea");
    QVERIFY(LegacyTestData::write(path, QTextCodec::codecForName("GB18030")->fromUnicode(input)));
    ConfigFileDocument doc;
    QString error;
    QVERIFY2(doc.loadFromFile(path, &error), qPrintable(error));
    QCOMPARE(doc.parameters().last().value("Description"), QStringLiteral("温度校准（保留说明）"));
    const QString exported = dir.filePath("export.hea");
    QVERIFY2(doc.saveAs(exported, &error), qPrintable(error));
    ConfigFileDocument reload;
    QVERIFY(reload.loadFromFile(exported));
    CalibrationDefinition calibration;
    QVERIFY2(ConfigCalibrationParser::parse(reload.parameters().last(), &calibration, &error), qPrintable(error));
    QCOMPARE(calibration.points.size(), 3);
    QCOMPARE(calibration.points.at(2).engineering_value, 300.0);
}

void LegacyCompatibilityTests::validatesCalibrationData()
{
    ConfigFileDocument doc;
    QVERIFY(doc.loadFromText(LegacyTestData::config(1, 1, "poly", " coefficient: 2,3,4,0,0,0\n")));
    CalibrationDefinition calibration;
    QString error;
    ConfigParameterRecord record = doc.parameters().last();
    QVERIFY(ConfigCalibrationParser::parse(record, &calibration, &error));
    QCOMPARE(calibration.coefficients.at(2), 4.0);
    record.setValue("coefficient", "2,bad,4");
    QVERIFY(!ConfigCalibrationParser::parse(record, &calibration, &error));
    record.setValue("coefficient", "");
    QVERIFY(!ConfigCalibrationParser::parse(record, &calibration, &error));
    record.setValue("cal_type", "segm");
    record.setValue("segment_number", "2");
    record.setValue("code,phy", "1,10;1,20");
    QVERIFY(!ConfigCalibrationParser::parse(record, &calibration, &error));
    record.setValue("code,phy", "1,10");
    QVERIFY(!ConfigCalibrationParser::parse(record, &calibration, &error));
}

void LegacyCompatibilityTests::decodesEquipmentTimes_data()
{
    QTest::addColumn<QString>("equipment");
    QTest::addColumn<QList<quint16>>("words");
    QTest::addColumn<double>("seconds");
    QTest::newRow("dm6") << QString("DM6") << QList<quint16>{1,2,3,456} << 3723.456;
    QTest::newRow("kam500") << QString("KAM500") << QList<quint16>{0x82,0x0345,0x6000} << 3723.456;
    QTest::newRow("qinchuan") << QStringLiteral("秦川") << QList<quint16>{8,0x103,0} << 3723.0;
    QTest::newRow("770") << QString("770") << QList<quint16>{1,0x040d,0x1580} << 3723.456;
    QTest::newRow("jqd") << QString("JQD") << QList<quint16>{0x102,0x600,quint16(456<<6)} << 3723.456;
    QTest::newRow("xfc100") << QString("XFC100") << QList<quint16>{0,15,16960} << 1.0;
    QTest::newRow("xma") << QString("XMA") << QList<quint16>{0,15,16960} << 1.0;
    QTest::newRow("uma2000") << QString("UMA2000") << QList<quint16>{0,100,0} << 1.0;
    QTest::newRow("xyd") << QString("XYD") << QList<quint16>{0,0x82,0x307} << 3723.007;
    QTest::newRow("ttc") << QString("TTC") << QList<quint16>{1,100,0} << 656.36;
}

void LegacyCompatibilityTests::decodesEquipmentTimes()
{
    QFETCH(QString, equipment);
    QFETCH(QList<quint16>, words);
    QFETCH(double, seconds);
    double actual = -1;
    QString error;
    QVERIFY2(LegacyPcmDecoder::decodeTimeWords(equipment, words, &actual, &error), qPrintable(error));
    auto table = LegacyTestData::layout();
    table.time_code_type = equipment;
    FrameTask frame;
    frame.frame_data = QByteArray(16, '\0');
    for (int index = 0; index < words.size() && index < 4; ++index) {
        frame.frame_data[(index + 1) * 2] = char(words.at(index) & 255);
        frame.frame_data[(index + 1) * 2 + 1] = char(words.at(index) >> 8);
    }
    LegacyPcmDecodePlan plan;
    QVERIFY2(plan.prepare(table, 1, {"5"}, false, &error), qPrintable(error));
    double prepared_time = 0;
    QVERIFY2(plan.decodeFrameTime(frame, &prepared_time, &error), qPrintable(error));
    QCOMPARE(prepared_time, actual);
    QVERIFY2(qAbs(actual - seconds) < 1e-8, qPrintable(QString::number(actual, 'f', 9)));
}

void LegacyCompatibilityTests::rejectsInvalidTime()
{
    double time = 0;
    QVERIFY(!LegacyPcmDecoder::decodeTimeWords("DM6", {25,0,0}, &time));
    QVERIFY(!LegacyPcmDecoder::decodeTimeWords("DM6", {1,0}, &time));
}

void LegacyCompatibilityTests::appliesObjectBitsAndByteSwap()
{
    auto table = LegacyTestData::layout();
    StreamLayoutEntry entry = table.entries.last();
    entry.word_segments.first().bit_length = 4;
    entry.word_segments.first().object_bit = 8;
    ParameterWordSegment second = entry.word_segments.first();
    second.word_no = 6;
    second.object_bit = 0;
    entry.word_segments.append(second);
    QByteArray bytes = LegacyTestData::frame(0, 0x000a);
    bytes[12] = char(3);
    quint64 value = 0;
    int width = 0;
    QVERIFY(FrameProcessor::extractRawBits(bytes, table, entry, &value, &width));
    QCOMPARE(value, quint64(0xa03));
    QCOMPARE(width, 12);
    entry.word_segments[0].object_bit = 0;
    QVERIFY(FrameProcessor::extractRawBits(bytes, table, entry, &value, &width));
    QCOMPARE(value, quint64(0xa3));
    entry.word_segments = {entry.word_segments.first()};
    entry.word_segments[0].bit_length = 16;
    entry.inverse = true;
    bytes = LegacyTestData::frame(0, 0x1234);
    QVERIFY(FrameProcessor::extractRawBits(bytes, table, entry, &value, &width));
    QCOMPARE(value, quint64(0x3412));
    entry.word_segments[0].object_bit = 63;
    QVERIFY(!FrameProcessor::extractRawBits(bytes, table, entry, &value, &width));
}

void LegacyCompatibilityTests::appliesLegacyCalibration()
{
    auto entry = LegacyTestData::layout().entries.last();
    QVariant value;
    entry.calibration.type = CalibrationType::Polynomial;
    entry.calibration.coefficients = {2,3,4};
    QVERIFY(FrameProcessor::convertRawValue(5, 16, entry, &value));
    QCOMPARE(value.toDouble(), 117.0);
    entry.calibration.type = CalibrationType::Segmented;
    entry.calibration.points = {{0,0},{10,100}};
    QVERIFY(FrameProcessor::convertRawValue(15, 16, entry, &value));
    QCOMPARE(value.toDouble(), 150.0);
    entry.calibration.type = CalibrationType::Hyperbolic;
    entry.calibration.segment_point = 5;
    entry.calibration.coefficients = {20,0,1,2,10,1,1,3};
    QVERIFY(FrameProcessor::convertRawValue(10, 16, entry, &value));
    QCOMPARE(value.toDouble(), 4.0);
    entry.calibration.segment_point = 0;
    QVERIFY(!FrameProcessor::convertRawValue(0, 16, entry, &value));
    entry.calibration.type = CalibrationType::None;
    entry.code_type = ParameterCodeType::FloatingPoint;
    QVERIFY(FrameProcessor::convertRawValue(65535, 16, entry, &value));
    QCOMPARE(value.toDouble(), -1.0);
    entry.code_type = ParameterCodeType::SignedMagnitude;
    QVERIFY(FrameProcessor::convertRawValue(0x8003, 16, entry, &value));
    QCOMPARE(value.toLongLong(), qlonglong(-3));
}

void LegacyCompatibilityTests::samplesAndHoldsByOccurrences()
{
    auto table = LegacyTestData::layout();
    table.entries.last().rate_hz = 2;
    table.entries.last().word_segments.first().word_no = 1;
    auto held = table.entries.last();
    held.parameter_id = "6";
    held.parameter_name = "HELD";
    held.rate_hz = 1;
    held.word_segments.first().word_no = 2;
    table.entries.append(held);
    FrameTask frame;
    frame.task_id = "sample";
    frame.frame_data = LegacyTestData::frame(3, 20, 1, 2, 0);
    frame.output_sample_rate_hz = 2;
    QList<ParameterValue> values;
    QString error;
    QVERIFY2(LegacyPcmDecoder::decode(frame, table, {"5","6"}, &values, &error), qPrintable(error));
    QCOMPARE(values.size(), 4);
    QCOMPARE(values.at(0).value.toUInt(), uint(1));
    QCOMPARE(values.at(2).value.toUInt(), uint(20));
    QCOMPARE(values.at(1).value.toUInt(), uint(2));
    QCOMPARE(values.at(3).value.toUInt(), uint(2));
    QCOMPARE(values.at(0).timestamp_seconds, 3723.0);
    QCOMPARE(values.at(2).timestamp_seconds, 3723.5);
}

void LegacyCompatibilityTests::downsamplingKeepsRecordedTime()
{
    auto table = LegacyTestData::layout(4);
    FrameTask frame;
    frame.task_id = "downsample";
    frame.output_sample_rate_hz = 2;
    frame.frame_data = LegacyTestData::frame(3, 123, 1, 2, 250);
    QList<ParameterValue> values;
    QVERIFY(LegacyPcmDecoder::decode(frame, table, {"5"}, &values));
    QCOMPARE(values.first().timestamp_seconds, 3723.25);
    frame.frame_index = 1;
    QVERIFY(LegacyPcmDecoder::decode(frame, table, {"5"}, &values));
    QVERIFY(values.isEmpty());
    frame.frame_index = 2;
    frame.range_start_seconds = 3723.5;
    QVERIFY(LegacyPcmDecoder::decode(frame, table, {"5"}, &values));
    QVERIFY(values.isEmpty());
    frame.range_start_seconds = 0;
    frame.range_end_seconds = 3723.25;
    QVERIFY(LegacyPcmDecoder::decode(frame, table, {"5"}, &values));
    QVERIFY(values.isEmpty());
}

void LegacyCompatibilityTests::rejectsMissingTimeAndInvalidRates()
{
    auto table = LegacyTestData::layout();
    QVERIFY(!LegacyPcmDecoder::validate(table, 0, {"5"}));
    table.entries.removeFirst();
    QVERIFY(!LegacyPcmDecoder::validate(table, 1, {"5"}));
    table = LegacyTestData::layout();
    table.entries.last().rate_hz = 0;
    QVERIFY(!LegacyPcmDecoder::validate(table, 1, {"5"}));
}
void LegacyCompatibilityTests::discardsUncommittedOutputOnDestruction()
{
    QTemporaryDir dir;
    {
        ResultWriter writer;
        ResultWriterOptions options;
        options.output_file_path = dir.filePath("uncommitted.txt");
        writer.setOptions(options);
        TaskDefinition task;
        task.task_id = "destructor";
        task.output_directory = dir.path();
        ParameterGroupSelection group;
        group.stream_index = 1;
        group.parameter_ids = QStringList{"5"};
        task.parameter_groups = {group};
        QString error;
        QVERIFY2(writer.startStreamingTask(task, {LegacyTestData::layout()}, &error), qPrintable(error));
    }
    QVERIFY(QDir(dir.path()).entryList(QDir::Files | QDir::Hidden | QDir::NoDotAndDotDot).isEmpty());
}

void LegacyCompatibilityTests::preparedDecoderMatchesReference_data()
{
    QTest::addColumn<int>("sample_rate");
    QTest::addColumn<bool>("raw_output");
    for (int rate : {1, 2, 8, 16, 64, 128}) {
        for (bool raw : {false, true}) {
            const QByteArray name = QByteArray::number(rate) + (raw ? "-raw" : "-engineering");
            QTest::newRow(name.constData()) << rate << raw;
        }
    }
}

void LegacyCompatibilityTests::preparedDecoderMatchesReference()
{
    QFETCH(int, sample_rate);
    QFETCH(bool, raw_output);
    StreamLayoutTable table = LegacyTestData::layout(8);
    table.frame_length_words = 32;
    table.subframe_depth = 4;
    table.id_position_word = 0;
    table.entries.removeLast();
    QSet<QString> selected_ids;
    for (int mode = 0; mode < 6; ++mode) {
        StreamLayoutEntry entry;
        entry.parameter_id = QString::number(10 + mode);
        entry.parameter_name = QStringLiteral("VALUE_%1").arg(mode);
        entry.code_type = mode == 3 ? ParameterCodeType::SignedInteger : ParameterCodeType::UnsignedInteger;
        entry.rate_hz = 1 << (mode % 4);
        entry.inverse = mode == 2 || mode == 5;
        for (int index = 0; index < (mode < 3 ? 4 : 2); ++index) {
            ParameterWordSegment segment;
            segment.frame_no = entry.rate_hz == 1 ? index % 2 : 0;
            segment.word_no = 8 + index;
            segment.bit_length = mode < 2 ? 16 : 8;
            segment.source_bit = mode == 2 ? 3 : 0;
            segment.object_bit = mode == 1 ? (3 - index) * 16 : (mode == 2 ? index * 3 : 0);
            entry.word_segments.append(segment);
        }
        if (mode == 4) {
            entry.calibration.type = CalibrationType::Polynomial;
            entry.calibration.coefficients = {2, 3, 4};
        } else if (mode == 5) {
            entry.calibration.type = CalibrationType::Segmented;
            entry.calibration.points = {{100, 200}, {0, -100}, {50, 80}};
        }
        selected_ids.insert(entry.parameter_id);
        table.entries.append(entry);
    }
    LegacyPcmDecodePlan plan;
    QString error;
    QVERIFY2(plan.prepare(table, sample_rate, selected_ids, raw_output, &error), qPrintable(error));
    for (int index = 0; index < 16; ++index) {
        FrameTask frame;
        frame.task_id = "prepared";
        frame.frame_index = index;
        frame.output_sample_rate_hz = sample_rate;
        frame.range_start_seconds = 3723.25;
        frame.range_end_seconds = 3724.7;
        for (int word = 0; word < 128; ++word) {
            const quint16 bits = quint16(word * 40503 + index * 3079);
            frame.frame_data.append(char(bits & 255));
            frame.frame_data.append(char(bits >> 8));
        }
        const QByteArray time = LegacyTestData::frame(3 + index / 8, 0, 1, 2, (index % 8) * 125);
        frame.frame_data.replace(2, 8, time.mid(2, 8));
        double actual_time = 0, expected_time = 0;
        QVERIFY2(plan.decodeFrameTime(frame, &actual_time, &error), qPrintable(error));
        QVERIFY(LegacyPcmDecoder::decodeFrameTime(frame, table, &expected_time));
        QCOMPARE(actual_time, expected_time);
        QList<ParameterValue> actual, expected;
        QVERIFY2(LegacyPcmDecoder::decode(frame, table, selected_ids, &expected, &error), qPrintable(error));
        QVERIFY2(plan.decode(frame, actual_time, &actual, &error), qPrintable(error));
        QCOMPARE(actual.size(), expected.size());
        for (int value = 0; value < actual.size(); ++value) {
            QCOMPARE(actual.at(value).task_id, expected.at(value).task_id);
            QCOMPARE(actual.at(value).parameter_id, expected.at(value).parameter_id);
            QCOMPARE(actual.at(value).timestamp_seconds, expected.at(value).timestamp_seconds);
            QCOMPARE(actual.at(value).value.userType(), expected.at(value).value.userType());
            QCOMPARE(actual.at(value).value, expected.at(value).value);
            if (raw_output) {
                QCOMPARE(actual.at(value).raw_value.userType(), expected.at(value).raw_value.userType());
                QCOMPARE(actual.at(value).raw_value, expected.at(value).raw_value);
            } else {
                QVERIFY(!actual.at(value).raw_value.isValid());
            }
        }
    }
}

void LegacyCompatibilityTests::preparedDecoderRejectsInvalidInput()
{
    LegacyPcmDecodePlan plan;
    auto table = LegacyTestData::layout();
    FrameTask frame;
    frame.frame_data = LegacyTestData::frame(3, 10, 1, 2, 0);
    double seconds = 0;
    QList<ParameterValue> values;
    QVERIFY(!plan.decodeFrameTime(frame, &seconds));
    QVERIFY(plan.prepare(table, 1, {"5"}, false));
    frame.frame_data.chop(1);
    QVERIFY(!plan.decodeFrameTime(frame, &seconds));
    QVERIFY(!plan.decode(frame, 3723, &values));
    frame.frame_data = LegacyTestData::frame(3, 10, 24, 2, 0);
    QVERIFY(!plan.decodeFrameTime(frame, &seconds));
    table.entries.removeFirst();
    QVERIFY(!plan.prepare(table, 1, {"5"}, false));
    frame.frame_data = LegacyTestData::frame(3, 10, 1, 2, 0);
    QVERIFY(!plan.decode(frame, 3723, &values));
}

void LegacyCompatibilityTests::prefetchedBlocksPreserveTailsAndCancellation()
{
    QTemporaryDir directory;
    QByteArray raw(3, char(0x13));
    for (int index = 0; index < 700; ++index) {
        raw.append(LegacyTestData::frame(index % 60, index, 0, index / 60));
    }
    raw.append(QByteArray(5, char(0x13)));
    const QString raw_path = directory.filePath("source.pcm");
    QVERIFY(LegacyTestData::write(raw_path, raw));
    FastFixedPcmTask task;
    task.definition.task_id = "prefetch";
    task.definition.raw_file_path = raw_path;
    task.definition.user_sample_rate_hz = 1;
    ParameterGroupSelection group;
    group.stream_index = 1;
    group.parameter_ids = QStringList{"5"};
    task.definition.parameter_groups = {group};
    task.layout_tables = {LegacyTestData::layout()};
    StreamDefinition stream;
    stream.stream_index = 1;
    stream.stream_id = "stream_1";
    stream.sync_word = QByteArray::fromHex("55aa");
    task.streams = {stream};
    task.selected_parameter_ids_by_stream.insert(stream.stream_id, {"5"});
    task.writer_options.output_file_path = directory.filePath("complete.txt");
    task.writer_options.legacy_time_format = true;
    FastFixedPcmProcessorOptions options;
    options.block_size_bytes = 4096;
    FastFixedPcmProcessor processor(task, options);
    QSignalSpy finished(&processor, &FastFixedPcmProcessor::finished);
    QSignalSpy failed(&processor, &FastFixedPcmProcessor::failed);
    processor.process();
    QCOMPARE(failed.size(), 0);
    QCOMPARE(finished.size(), 1);
    QCOMPARE(finished.first().at(2).toULongLong(), quint64(700));
    QFile output(task.writer_options.output_file_path);
    QVERIFY(output.open(QIODevice::ReadOnly | QIODevice::Text));
    const QList<QByteArray> lines = output.readAll().trimmed().split('\n');
    QCOMPARE(lines.size(), 701);
    for (int index = 0; index < 700; ++index) {
        const QByteArray expected = QStringLiteral("00:%1:%2:000\t%3")
            .arg(index / 60, 2, 10, QChar('0')).arg(index % 60, 2, 10, QChar('0')).arg(index).toUtf8();
        QCOMPARE(lines.at(index + 1), expected);
    }

    task.definition.task_id = "prefetch-cancel";
    task.writer_options.output_file_path = directory.filePath("cancelled.txt");
    FastFixedPcmProcessor cancelled(task, options);
    QSignalSpy stopped(&cancelled, &FastFixedPcmProcessor::stopped);
    QObject::connect(&cancelled, &FastFixedPcmProcessor::started, &cancelled, [&]() {
        cancelled.requestStop();
    });
    cancelled.process();
    QCOMPARE(stopped.size(), 1);
    QVERIFY(!QFile::exists(task.writer_options.output_file_path));
}
QTEST_GUILESS_MAIN(LegacyCompatibilityTests)
#include "legacy_compatibility_tests.moc"
