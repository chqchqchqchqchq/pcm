#include "traditional_processing_controller.h"

#include <QFile>
#include <QFileInfo>
#include <QSignalSpy>
#include <QTemporaryDir>
#include <QtTest>

class TraditionalProcessingControllerTests : public QObject
{
    Q_OBJECT

private slots:
    /**
     * @brief Runs the minimal GUI-equivalent single-task backend chain.
     */
    void runsMinimalSingleTaskPipeline();

    /**
     * @brief Keeps the controller alive and recoverable when a config contains duplicate stream sync words.
     */
    void reportsDuplicateSyncWordsWithoutCrashing();
};

namespace {

QString sampleConfigText()
{
    return QStringLiteral(
        "#CFTE/filemain/rec01.00\n"
        "Header_informations:\n"
        "Create_date: 2026-06-03\n"
        "test_id: traditional-integration\n"
        "stream_number: 1\n"
        "derpar_number: 0\n"
        "\n"
        "The format description of stream 1:\n"
        "aircraft_name: AC1\n"
        "test_name: T1\n"
        "parameter_number: 1\n"
        "bit_rate: 16\n"
        "pcm_code: NRZ-L\n"
        "loop_width: 0.1\n"
        "word_length: 8\n"
        "parity: NONE\n"
        "frame_length: 2\n"
        "subframe_deepth: 1\n"
        "major_frame_rate: 1\n"
        "sync_word_length: 8\n"
        "sync_pattern: AA\n"
        "\n"
        "the_parameter_description_of_stream_1:\n"
        "serial_no.: 1\n"
        "id_no.: P001\n"
        "name: TEMP\n"
        "unit: C\n"
        "cal_type: NO\n"
        "code_type: uint\n"
        "cal_date: 2026-06-03\n"
        "cal_number: \n"
        "rate: 1\n"
        "word_interval: 1\n"
        "sign_bit: 0\n"
        "inverse: 0\n"
        "pcm_words: 1\n"
        "frame_no: 1,0,0,0,0,0\n"
        "word_no: 2,0,0,0,0,0\n"
        "source_bit: 0,0,0,0,0,0\n"
        "bit_length: 8,0,0,0,0,0\n"
        "object_bit: 0,0,0,0,0,0\n"
        "description: temperature\n");
}

QString duplicateSyncConfigText()
{
    return QStringLiteral(
        "#CFTE/filemain/rec01.00\n"
        "Header_informations:\n"
        "Create_date: 2026-06-03\n"
        "test_id: duplicate-sync\n"
        "stream_number: 2\n"
        "derpar_number: 0\n"
        "\n"
        "The format description of stream 1:\n"
        "aircraft_name: AC1\n"
        "test_name: T1\n"
        "parameter_number: 1\n"
        "bit_rate: 16\n"
        "pcm_code: NRZ-L\n"
        "loop_width: 0.1\n"
        "word_length: 8\n"
        "parity: NONE\n"
        "frame_length: 2\n"
        "subframe_deepth: 1\n"
        "major_frame_rate: 1\n"
        "sync_word_length: 8\n"
        "sync_pattern: AA\n"
        "\n"
        "the_parameter_description_of_stream_1:\n"
        "serial_no.: 1\n"
        "id_no.: P001\n"
        "name: TEMP_A\n"
        "unit: C\n"
        "cal_type: NO\n"
        "code_type: uint\n"
        "cal_date: 2026-06-03\n"
        "cal_number: \n"
        "rate: 1\n"
        "word_interval: 1\n"
        "sign_bit: 0\n"
        "inverse: 0\n"
        "pcm_words: 1\n"
        "frame_no: 1,0,0,0,0,0\n"
        "word_no: 2,0,0,0,0,0\n"
        "source_bit: 0,0,0,0,0,0\n"
        "bit_length: 8,0,0,0,0,0\n"
        "object_bit: 0,0,0,0,0,0\n"
        "description: temperature A\n"
        "\n"
        "The format description of stream 2:\n"
        "aircraft_name: AC2\n"
        "test_name: T2\n"
        "parameter_number: 1\n"
        "bit_rate: 16\n"
        "pcm_code: NRZ-L\n"
        "loop_width: 0.1\n"
        "word_length: 8\n"
        "parity: NONE\n"
        "frame_length: 2\n"
        "subframe_deepth: 1\n"
        "major_frame_rate: 1\n"
        "sync_word_length: 8\n"
        "sync_pattern: AA\n"
        "\n"
        "the_parameter_description_of_stream_2:\n"
        "serial_no.: 1\n"
        "id_no.: P002\n"
        "name: TEMP_B\n"
        "unit: C\n"
        "cal_type: NO\n"
        "code_type: uint\n"
        "cal_date: 2026-06-03\n"
        "cal_number: \n"
        "rate: 1\n"
        "word_interval: 1\n"
        "sign_bit: 0\n"
        "inverse: 0\n"
        "pcm_words: 1\n"
        "frame_no: 1,0,0,0,0,0\n"
        "word_no: 2,0,0,0,0,0\n"
        "source_bit: 0,0,0,0,0,0\n"
        "bit_length: 8,0,0,0,0,0\n"
        "object_bit: 0,0,0,0,0,0\n"
        "description: temperature B\n");
}

bool writeTextFile(const QString &file_path, const QString &content)
{
    QFile file(file_path);
    if (!file.open(QIODevice::WriteOnly | QIODevice::Text)) {
        return false;
    }

    return file.write(content.toUtf8()) == content.toUtf8().size();
}

bool writeRawFile(const QString &file_path)
{
    QByteArray raw_data = QByteArrayLiteral("noise");
    raw_data.append(static_cast<char>(0xAA));
    raw_data.append(static_cast<char>(0x11));
    raw_data.append(static_cast<char>(0xAA));
    raw_data.append(static_cast<char>(0x22));

    QFile file(file_path);
    if (!file.open(QIODevice::WriteOnly)) {
        return false;
    }
    return file.write(raw_data) == raw_data.size();
}

QStringList readOutputLines(const QString &file_path)
{
    QFile file(file_path);
    if (!file.open(QIODevice::ReadOnly | QIODevice::Text)) {
        return {};
    }

    QString content = QString::fromUtf8(file.readAll());
    content.replace(QStringLiteral("\r\n"), QStringLiteral("\n"));
    if (content.endsWith(QLatin1Char('\n'))) {
        content.chop(1);
    }
    return content.split(QLatin1Char('\n'));
}

} // namespace

void TraditionalProcessingControllerTests::runsMinimalSingleTaskPipeline()
{
    QTemporaryDir temp_dir;
    QVERIFY(temp_dir.isValid());

    const QString config_path = temp_dir.filePath(QStringLiteral("config.cfte"));
    const QString raw_path = temp_dir.filePath(QStringLiteral("raw.pcm"));
    const QString output_path = temp_dir.filePath(QStringLiteral("result.txt"));
    QVERIFY(writeTextFile(config_path, sampleConfigText()));
    QVERIFY(writeRawFile(raw_path));

    TraditionalProcessingOptions options;
    options.parameter_names << QStringLiteral("TIME") << QStringLiteral("TEMP");
    options.output_file_path = output_path;
    options.task_definition.task_id = QStringLiteral("traditional-integration");
    options.task_definition.config_file_path = config_path;
    options.task_definition.raw_file_path = raw_path;
    options.task_definition.output_directory = temp_dir.path();
    options.task_definition.user_sample_rate_hz = 2.0;

    TraditionalProcessingController controller;
    QSignalSpy finished_spy(&controller, &TraditionalProcessingController::taskFinished);
    QSignalSpy failed_spy(&controller, &TraditionalProcessingController::taskFailed);
    QSignalSpy stopped_spy(&controller, &TraditionalProcessingController::taskStopped);
    QSignalSpy progress_spy(&controller, &TraditionalProcessingController::taskProgressChanged);

    controller.startProcessing(options);
    while (finished_spy.count() == 0 && failed_spy.count() == 0 && stopped_spy.count() == 0) {
        QVERIFY(finished_spy.wait(5000) || failed_spy.count() > 0 || stopped_spy.count() > 0);
    }

    QCOMPARE(failed_spy.count(), 0);
    QCOMPARE(stopped_spy.count(), 0);
    QCOMPARE(finished_spy.count(), 1);
    QVERIFY(progress_spy.count() > 0);
    QCOMPARE(finished_spy.first().at(0).toString(), options.task_definition.task_id);
    QCOMPARE(finished_spy.first().at(1).toString(), QFileInfo(output_path).absoluteFilePath());

    const QStringList lines = readOutputLines(output_path);
    QCOMPARE(lines.size(), 3);
    QCOMPARE(lines.at(0), QStringLiteral("timestamp_seconds\tstream_1.P001:TEMP[C]"));
    QCOMPARE(lines.at(1), QStringLiteral("0.000000000\t17"));
    QCOMPARE(lines.at(2), QStringLiteral("1.000000000\t34"));
}

void TraditionalProcessingControllerTests::reportsDuplicateSyncWordsWithoutCrashing()
{
    QTemporaryDir temp_dir;
    QVERIFY(temp_dir.isValid());

    const QString config_path = temp_dir.filePath(QStringLiteral("duplicate-sync.cfte"));
    const QString raw_path = temp_dir.filePath(QStringLiteral("raw.pcm"));
    const QString output_path = temp_dir.filePath(QStringLiteral("result.txt"));
    QVERIFY(writeTextFile(config_path, duplicateSyncConfigText()));
    QVERIFY(writeRawFile(raw_path));

    TraditionalProcessingOptions options;
    options.parameter_names << QStringLiteral("TIME")
                            << QStringLiteral("TEMP_A")
                            << QStringLiteral("TEMP_B");
    options.output_file_path = output_path;
    options.task_definition.task_id = QStringLiteral("duplicate-sync");
    options.task_definition.config_file_path = config_path;
    options.task_definition.raw_file_path = raw_path;
    options.task_definition.output_directory = temp_dir.path();
    options.task_definition.user_sample_rate_hz = 2.0;

    TraditionalProcessingController controller;
    QSignalSpy finished_spy(&controller, &TraditionalProcessingController::taskFinished);
    QSignalSpy failed_spy(&controller, &TraditionalProcessingController::taskFailed);
    QSignalSpy stopped_spy(&controller, &TraditionalProcessingController::taskStopped);

    controller.startProcessing(options);
    while (failed_spy.count() == 0 && finished_spy.count() == 0 && stopped_spy.count() == 0) {
        QVERIFY(failed_spy.wait(5000) || finished_spy.count() > 0 || stopped_spy.count() > 0);
    }

    QCOMPARE(finished_spy.count(), 0);
    QCOMPARE(stopped_spy.count(), 0);
    QCOMPARE(failed_spy.count(), 1);
    QVERIFY2(failed_spy.first().at(1).toString().contains(QStringLiteral("同步字")),
             qPrintable(failed_spy.first().at(1).toString()));
    QTest::qWait(50);
}

QTEST_MAIN(TraditionalProcessingControllerTests)

#include "traditional_processing_controller_tests.moc"
