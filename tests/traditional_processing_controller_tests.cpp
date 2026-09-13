#include "traditional_processing_controller.h"
#include "fast_fixed_pcm_processor.h"
#include "legacy_test_data.h"

#include <QFileInfo>
#include <QSignalSpy>
#include <QTemporaryDir>
#include <QtTest>

/** @brief 旧版 CFTE 配置经过真实控制器、文件读取和 TXT 写出的端到端回归。 */
class TraditionalProcessingControllerTests : public QObject
{
    Q_OBJECT
private slots:
    void runsMinimalSingleTaskPipeline();
    void runsPipelineBeyondLegacyResultQueueLimit();
    void processesDuplicateSyncWordsFromSeparateStreamFiles();
    void loadsCalibrationFromFile_data();
    void loadsCalibrationFromFile();
    void usesObjectBitAndInverseFromFile();
    void runsMultipleRangesWithIndependentFiles();
    void stopsAfterCompletedRangeAndKeepsItsFile();
    void recoversSyncAcrossBlocksAndKeepsLastFrame();
    void rejectsInvalidConfigurationBeforeOutput();
    void rejectsTimeRegressionWithoutPublishingOutput();
    void acceptsDifferentStreamFrameLengths();
    void alignsSubframeIds();
    void resetsSamplingPhaseForEachRange();
    void readsTwelveBitWordsInSixteenBitStorage();
};

namespace {
/** @brief 配置控制器的真实输入文件和任务五元组。 */
TraditionalProcessingOptions optionsFor(const QTemporaryDir &dir, int stream_count = 1)
{
    TraditionalProcessingOptions options;
    options.task_definition.task_id = "legacy-test";
    options.task_definition.config_file_path = dir.filePath("config.hea");
    options.task_definition.raw_file_path = dir.filePath("raw.pcm");
    options.task_definition.output_directory = dir.path();
    options.task_definition.user_sample_rate_hz = 1;
    options.parameter_names = QStringList{"TIME"};
    for (int index = 1; index <= stream_count; ++index) {
        options.parameter_names.append(stream_count == 1 ? QString("TEMP") : QString("TEMP_%1").arg(index));
    }
    options.output_file_path = dir.filePath("result.txt");
    return options;
}
/** @brief 返回输出的非空行。 */
QStringList lines(const QString &path)
{
    QFile file(path);
    if (!file.open(QIODevice::ReadOnly)) {
        return {};
    }
    return QString::fromUtf8(file.readAll()).replace(QStringLiteral("\r"), QString()).trimmed()
        .split(QLatin1Char('\n'));
}
/** @brief 等待真正终态，返回错误文本便于 QVERIFY 显示。 */
QString run(TraditionalProcessingController &controller, const TraditionalProcessingOptions &options)
{
    QSignalSpy finished(&controller, &TraditionalProcessingController::processingFinished);
    QSignalSpy failed(&controller, &TraditionalProcessingController::taskFailed);
    QSignalSpy stopped(&controller, &TraditionalProcessingController::taskStopped);
    controller.startProcessing(options);
    QElapsedTimer timer;
    timer.start();
    while (finished.isEmpty() && failed.isEmpty() && stopped.isEmpty() && timer.elapsed() < 15000) {
        QTest::qWait(5);
    }
    if (!failed.isEmpty()) {
        return failed.first().at(1).toString();
    }
    return !finished.isEmpty() || !stopped.isEmpty() ? QString() : QString("任务超时");
}
}

void TraditionalProcessingControllerTests::runsMinimalSingleTaskPipeline()
{
    QTemporaryDir dir;
    const auto options = optionsFor(dir);
    QVERIFY(LegacyTestData::write(options.task_definition.config_file_path, LegacyTestData::config().toUtf8()));
    QVERIFY(LegacyTestData::write(options.task_definition.raw_file_path,
        LegacyTestData::frame(3,17,1,2) + LegacyTestData::frame(4,34,1,2)));
    TraditionalProcessingController controller;
    QString error;
    QVERIFY2(controller.validateOptions(options, &error), qPrintable(error));
    error = run(controller, options);
    QVERIFY2(error.isEmpty(), qPrintable(error));
    QCOMPARE(lines(options.output_file_path), QStringList({"timestamp_seconds\tTEMP",
        "3723.000000000\t17", "3724.000000000\t34"}));
}

void TraditionalProcessingControllerTests::runsPipelineBeyondLegacyResultQueueLimit()
{
    QTemporaryDir dir;
    auto options = optionsFor(dir);
    QVERIFY(LegacyTestData::write(options.task_definition.config_file_path, LegacyTestData::config().toUtf8()));
    QByteArray bytes;
    for (int index = 0; index < 12000; ++index) {
        bytes += LegacyTestData::frame(index % 60, index, index / 3600, (index / 60) % 60);
    }
    QVERIFY(LegacyTestData::write(options.task_definition.raw_file_path, bytes));
    TraditionalProcessingController controller;
    const QString error = run(controller, options);
    QVERIFY2(error.isEmpty(), qPrintable(error));
    QCOMPARE(lines(options.output_file_path).size(), 12001);
    QCOMPARE(lines(options.output_file_path).last(), QString("11999.000000000\t11999"));
}

void TraditionalProcessingControllerTests::processesDuplicateSyncWordsFromSeparateStreamFiles()
{
    QTemporaryDir dir;
    const auto options = optionsFor(dir, 2);
    QVERIFY(LegacyTestData::write(options.task_definition.config_file_path, LegacyTestData::config(2).toUtf8()));
    QVERIFY(LegacyTestData::write(options.task_definition.raw_file_path,
        LegacyTestData::frame(0,11) + LegacyTestData::frame(1,12)));
    QVERIFY(LegacyTestData::write(dir.filePath("raw_2.pcm"),
        LegacyTestData::frame(0,21) + LegacyTestData::frame(1,22)));
    TraditionalProcessingController controller;
    const QString error = run(controller, options);
    QVERIFY2(error.isEmpty(), qPrintable(error));
    const QStringList output = lines(options.output_file_path);
    QCOMPARE(output.size(), 3);
    QVERIFY(output.at(1).contains("11"));
    QVERIFY(output.at(1).contains("21"));
    QVERIFY(output.at(2).contains("12"));
    QVERIFY(output.at(2).contains("22"));
}

void TraditionalProcessingControllerTests::loadsCalibrationFromFile_data()
{
    QTest::addColumn<QString>("type");
    QTest::addColumn<QString>("fields");
    QTest::addColumn<int>("raw");
    QTest::addColumn<double>("expected");
    QTest::newRow("line") << QString("line") << QString(" coefficient: 2,3,0,0,0,0\n") << 5 << 17.0;
    QTest::newRow("poly") << QString("poly") << QString(" coefficient: 2,3,4,0,0,0\n") << 5 << 117.0;
    QTest::newRow("segm-extrapolate") << QString("segm")
        << QString(" segment_number: 2\n code,phy: 0,0\n code,phy: 10,100\n") << 15 << 150.0;
    QTest::newRow("hyper") << QString("hyper")
        << QString(" segment_point: 5\n first_segment: 20,0,1,2\n second_segment: 10,1,1,3\n") << 10 << 4.0;
}

void TraditionalProcessingControllerTests::loadsCalibrationFromFile()
{
    QFETCH(QString, type);
    QFETCH(QString, fields);
    QFETCH(int, raw);
    QFETCH(double, expected);
    QTemporaryDir dir;
    auto options = optionsFor(dir);
    options.include_raw_values = true;
    QVERIFY(LegacyTestData::write(options.task_definition.config_file_path,
                                 LegacyTestData::config(1, 1, type, fields).toUtf8()));
    QVERIFY(LegacyTestData::write(options.task_definition.raw_file_path, LegacyTestData::frame(0,raw)));
    TraditionalProcessingController controller;
    const QString error = run(controller, options);
    QVERIFY2(error.isEmpty(), qPrintable(error));
    const QStringList cells = lines(options.output_file_path).last().split('\t');
    QCOMPARE(cells.size(), 3);
    QCOMPARE(cells.at(1).toInt(), raw);
    QCOMPARE(cells.at(2).toDouble(), expected);
}

void TraditionalProcessingControllerTests::usesObjectBitAndInverseFromFile()
{
    QTemporaryDir dir;
    auto options = optionsFor(dir);
    QString config = LegacyTestData::config();
    const int last = config.lastIndexOf(" serial_no.:");
    QString parameter = config.mid(last);
    parameter.replace("pcm_words: 1", "pcm_words: 2");
    parameter.replace("word_no: 5,0", "word_no: 5,6");
    parameter.replace("bit_length: 16,0", "bit_length: 4,4");
    parameter.replace("object_bit: 0,0", "object_bit: 8,0");
    parameter.replace("inverse: 0", "inverse: 1");
    config = config.left(last) + parameter;
    QVERIFY(LegacyTestData::write(options.task_definition.config_file_path, config.toUtf8()));
    QByteArray bytes = LegacyTestData::frame(0, 0x0a00);
    bytes[13] = char(3);
    QVERIFY(LegacyTestData::write(options.task_definition.raw_file_path, bytes));
    TraditionalProcessingController controller;
    const QString error = run(controller, options);
    QVERIFY2(error.isEmpty(), qPrintable(error));
    QCOMPARE(lines(options.output_file_path).last(), QString("0.000000000\t2563"));
}

void TraditionalProcessingControllerTests::runsMultipleRangesWithIndependentFiles()
{
    QTemporaryDir dir;
    auto options = optionsFor(dir);
    options.time_ranges = {TaskTimeRange{3723,3724}, TaskTimeRange{3724,3725}};
    QVERIFY(LegacyTestData::write(options.task_definition.config_file_path, LegacyTestData::config().toUtf8()));
    QVERIFY(LegacyTestData::write(options.task_definition.raw_file_path,
        LegacyTestData::frame(3,17,1,2) + LegacyTestData::frame(4,34,1,2)));
    TraditionalProcessingController controller;
    QSignalSpy ranges(&controller, &TraditionalProcessingController::rangeFinished);
    const QString error = run(controller, options);
    QVERIFY2(error.isEmpty(), qPrintable(error));
    QCOMPARE(ranges.size(), 2);
    QCOMPARE(lines(dir.filePath("result_range001.txt")).last(), QString("3723.000000000\t17"));
    QCOMPARE(lines(dir.filePath("result_range002.txt")).last(), QString("3724.000000000\t34"));
}

void TraditionalProcessingControllerTests::stopsAfterCompletedRangeAndKeepsItsFile()
{
    QTemporaryDir dir;
    auto options = optionsFor(dir);
    options.time_ranges = {TaskTimeRange{0,1}, TaskTimeRange{1,2}};
    QVERIFY(LegacyTestData::write(options.task_definition.config_file_path, LegacyTestData::config().toUtf8()));
    QVERIFY(LegacyTestData::write(options.task_definition.raw_file_path,
        LegacyTestData::frame(0,17) + LegacyTestData::frame(1,34)));
    TraditionalProcessingController controller;
    QSignalSpy stopped(&controller, &TraditionalProcessingController::taskStopped);
    connect(&controller, &TraditionalProcessingController::rangeFinished,
            &controller, [&controller]() { controller.requestStop(); });
    const QString error = run(controller, options);
    QVERIFY2(error.isEmpty(), qPrintable(error));
    QCOMPARE(stopped.size(), 1);
    QVERIFY(QFileInfo::exists(dir.filePath("result_range001.txt")));
    QVERIFY(!QFileInfo::exists(dir.filePath("result_range002.txt")));
}

void TraditionalProcessingControllerTests::recoversSyncAcrossBlocksAndKeepsLastFrame()
{
    QTemporaryDir dir;
    auto options = optionsFor(dir);
    QVERIFY(LegacyTestData::write(options.task_definition.config_file_path, LegacyTestData::config().toUtf8()));
    QVERIFY(LegacyTestData::write(options.task_definition.raw_file_path,
        QByteArray(4095, char(0)) + LegacyTestData::frame(0,17) +
        QByteArray(3, char(0)) + LegacyTestData::frame(1,34)));
    TraditionalProcessingController controller;
    const QString error = run(controller, options);
    QVERIFY2(error.isEmpty(), qPrintable(error));
    QCOMPARE(lines(options.output_file_path).size(), 3);
    QCOMPARE(lines(options.output_file_path).last(), QString("1.000000000\t34"));
}

void TraditionalProcessingControllerTests::rejectsInvalidConfigurationBeforeOutput()
{
    QTemporaryDir dir;
    const auto options = optionsFor(dir);
    QVERIFY(LegacyTestData::write(options.task_definition.config_file_path,
                                 LegacyTestData::config(1,1,"poly").toUtf8()));
    QVERIFY(LegacyTestData::write(options.task_definition.raw_file_path, LegacyTestData::frame(0,17)));
    TraditionalProcessingController controller;
    QString error;
    QVERIFY(!controller.validateOptions(options, &error));
    QVERIFY(error.contains("coefficient"));
    error = run(controller, options);
    QVERIFY(error.contains("coefficient"));
    QVERIFY(!QFileInfo::exists(options.output_file_path));
}

void TraditionalProcessingControllerTests::rejectsTimeRegressionWithoutPublishingOutput()
{
    QTemporaryDir dir;
    const auto options = optionsFor(dir);
    QVERIFY(LegacyTestData::write(options.task_definition.config_file_path, LegacyTestData::config().toUtf8()));
    QVERIFY(LegacyTestData::write(options.task_definition.raw_file_path,
        LegacyTestData::frame(2,17) + LegacyTestData::frame(1,34)));
    TraditionalProcessingController controller;
    const QString error = run(controller, options);
    QVERIFY(error.contains(QStringLiteral("时间码倒退")));
    QVERIFY(!QFileInfo::exists(options.output_file_path));
}

void TraditionalProcessingControllerTests::acceptsDifferentStreamFrameLengths()
{
    QTemporaryDir dir;
    const auto options = optionsFor(dir,2);
    QString config = LegacyTestData::config(2);
    const int second = config.indexOf(" The format description of stream 2:");
    QString tail = config.mid(second);
    tail.replace("frame_length: 8", "frame_length: 10");
    config = config.left(second) + tail;
    QVERIFY(LegacyTestData::write(options.task_definition.config_file_path, config.toUtf8()));
    QVERIFY(LegacyTestData::write(options.task_definition.raw_file_path, LegacyTestData::frame(0,17)));
    QVERIFY(LegacyTestData::write(dir.filePath("raw_2.pcm"), LegacyTestData::frame(0,34) + QByteArray(4,0)));
    TraditionalProcessingController controller;
    const QString error = run(controller, options);
    QVERIFY2(error.isEmpty(), qPrintable(error));
    QCOMPARE(lines(options.output_file_path).size(), 2);
}

void TraditionalProcessingControllerTests::alignsSubframeIds()
{
    QTemporaryDir dir;
    auto options = optionsFor(dir);
    QString config = LegacyTestData::config();
    config.replace("subframe_deepth: 1", "subframe_deepth: 2\n id_position: 7\n id_lsb: 0");
    QVERIFY(LegacyTestData::write(options.task_definition.config_file_path, config.toUtf8()));
    QByteArray first = LegacyTestData::frame(0,17);
    QByteArray second = LegacyTestData::frame(0,34);
    second[14] = 1;
    // 从 ID=1 子帧开始，必须先恢复 ID=0 的完整长周。
    QVERIFY(LegacyTestData::write(options.task_definition.raw_file_path, second + first + second));
    TraditionalProcessingController controller;
    const QString error = run(controller, options);
    QVERIFY2(error.isEmpty(), qPrintable(error));
    QCOMPARE(lines(options.output_file_path).last(), QString("0.000000000\t17"));
}
void TraditionalProcessingControllerTests::readsTwelveBitWordsInSixteenBitStorage()
{
    QTemporaryDir dir;
    const auto options = optionsFor(dir);
    QString config = LegacyTestData::config();
    config.replace(" word_length: 16", " word_length: 12");
    config.replace("bit_length: 16", "bit_length: 12");
    QVERIFY(LegacyTestData::write(options.task_definition.config_file_path, config.toUtf8()));
    QVERIFY(LegacyTestData::write(options.task_definition.raw_file_path, LegacyTestData::frame(0,0xabc)));
    TraditionalProcessingController controller;
    const QString error = run(controller, options);
    QVERIFY2(error.isEmpty(), qPrintable(error));
    QCOMPARE(lines(options.output_file_path).last(), QString("0.000000000\t2748"));
}
void TraditionalProcessingControllerTests::resetsSamplingPhaseForEachRange()
{
    QTemporaryDir dir;
    auto options = optionsFor(dir);
    options.task_definition.user_sample_rate_hz = 2;
    options.time_ranges = {TaskTimeRange{0.25,1.0}};
    QVERIFY(LegacyTestData::write(options.task_definition.config_file_path, LegacyTestData::config(1,4).toUtf8()));
    QByteArray bytes;
    for (int index = 0; index < 4; ++index) {
        bytes += LegacyTestData::frame(0, index, 0, 0, index * 250);
    }
    QVERIFY(LegacyTestData::write(options.task_definition.raw_file_path, bytes));
    TraditionalProcessingController controller;
    const QString error = run(controller, options);
    QVERIFY2(error.isEmpty(), qPrintable(error));
    const QStringList output = lines(options.output_file_path);
    QCOMPARE(output.size(), 3);
    QCOMPARE(output.at(1), QString("0.250000000\t1"));
    QCOMPARE(output.at(2), QString("0.750000000\t3"));
}
QTEST_GUILESS_MAIN(TraditionalProcessingControllerTests)
#include "traditional_processing_controller_tests.moc"
