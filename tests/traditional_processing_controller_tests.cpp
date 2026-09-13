#include "traditional_processing_controller.h"

#include "fast_fixed_pcm_processor.h"

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
     * @brief Processes more values than the old bounded result queue could hold.
     */
    void runsPipelineBeyondLegacyResultQueueLimit();

    /**
     * @brief Uses the fixed-frame fast path for clean per-stream raw files.
     */
    void runsPerStreamFixedFastPath();

    /**
     * @brief Keeps the fixed fast path default block size large enough for throughput runs.
     */
    void usesLargeDefaultFastBlockSize();

    /**
     * @brief Fails fixed fast processing when a later frame loses its sync word.
     */
    void failsFastPathWhenFrameSyncIsLost();

    /**
     * @brief Accepts per-stream fixed files even when streams have different frame lengths.
     */
    void acceptsFastPathWithDifferentFrameLengths();

    /**
     * @brief Keeps the fast path available when selected parameters use the last valid zero-based indexes.
     */
    void acceptsFastPathWithZeroBasedLastIndexes();

    /**
     * @brief Keeps the controller alive and recoverable when a config contains duplicate stream sync words.
     */
    void processesDuplicateSyncWordsFromSeparateStreamFiles();

    /**
     * @brief Interprets sync and payload bytes as little-endian within each PCM word.
     */
    void matchesLittleEndianSyncAndPayloadWordBytes();

    /**
     * @brief Uses layout-array word order in the compiled fixed fast decoder.
     */
    void usesCompiledFastPathArrayOrderForMultiWordParameters();

    /**
     * @brief Writes input-order ranges to independent files and preserves absolute timestamps.
     */
    void runsMultipleRangesWithIndependentFiles();

    /**
     * @brief Restricts the fixed fast path to the half-open range and writes raw/engineering pairs.
     */
    void limitsFastPathToRequestedRange();

    /**
     * @brief Stops before later ranges while preserving the already committed output.
     */
    void stopsAfterCompletedRangeAndKeepsItsFile();
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
        "frame_no: 0,0,0,0,0,0\n"
        "word_no: 1,0,0,0,0,0\n"
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
        "frame_no: 0,0,0,0,0,0\n"
        "word_no: 1,0,0,0,0,0\n"
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
        "frame_no: 0,0,0,0,0,0\n"
        "word_no: 1,0,0,0,0,0\n"
        "source_bit: 0,0,0,0,0,0\n"
        "bit_length: 8,0,0,0,0,0\n"
        "object_bit: 0,0,0,0,0,0\n"
        "description: temperature B\n");
}

QString fixedTwoStreamConfigText()
{
    return QStringLiteral(
        "#CFTE/filemain/rec01.00\n"
        "Header_informations:\n"
        "Create_date: 2026-06-03\n"
        "test_id: fixed-fast\n"
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
        "sync_pattern: A1\n"
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
        "frame_no: 0,0,0,0,0,0\n"
        "word_no: 1,0,0,0,0,0\n"
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
        "sync_pattern: B2\n"
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
        "frame_no: 0,0,0,0,0,0\n"
        "word_no: 1,0,0,0,0,0\n"
        "source_bit: 0,0,0,0,0,0\n"
        "bit_length: 8,0,0,0,0,0\n"
        "object_bit: 0,0,0,0,0,0\n"
        "description: temperature B\n");
}

QString syncEndianConfigText()
{
    return QStringLiteral(
        "#CFTE/filemain/rec01.00\n"
        "Header_informations:\n"
        "Create_date: 2026-06-05\n"
        "test_id: sync-endian\n"
        "stream_number: 1\n"
        "derpar_number: 0\n"
        "\n"
        "The format description of stream 1:\n"
        "aircraft_name: AC1\n"
        "test_name: T1\n"
        "parameter_number: 1\n"
        "bit_rate: 32\n"
        "pcm_code: NRZ-L\n"
        "loop_width: 0.1\n"
        "word_length: 16\n"
        "parity: NONE\n"
        "frame_length: 2\n"
        "subframe_deepth: 1\n"
        "major_frame_rate: 1\n"
        "sync_word_length: 16\n"
        "sync_pattern: A1B2\n"
        "\n"
        "the_parameter_description_of_stream_1:\n"
        "serial_no.: 1\n"
        "id_no.: P016\n"
        "name: PAYLOAD\n"
        "unit: raw\n"
        "cal_type: NO\n"
        "code_type: uint\n"
        "cal_date: 2026-06-05\n"
        "cal_number: \n"
        "rate: 1\n"
        "word_interval: 1\n"
        "sign_bit: 0\n"
        "inverse: 0\n"
        "pcm_words: 1\n"
        "frame_no: 0,0,0,0,0,0\n"
        "word_no: 1,0,0,0,0,0\n"
        "source_bit: 0,0,0,0,0,0\n"
        "bit_length: 16,0,0,0,0,0\n"
        "object_bit: 0,0,0,0,0,0\n"
        "description: payload\n");
}

QString fastArrayOrderConfigText()
{
    return QStringLiteral(
        "#CFTE/filemain/rec01.00\n"
        "Header_informations:\n"
        "Create_date: 2026-06-07\n"
        "test_id: fast-array-order\n"
        "stream_number: 1\n"
        "derpar_number: 0\n"
        "\n"
        "The format description of stream 1:\n"
        "aircraft_name: AC1\n"
        "test_name: T1\n"
        "parameter_number: 1\n"
        "bit_rate: 24\n"
        "pcm_code: NRZ-L\n"
        "loop_width: 0.1\n"
        "word_length: 8\n"
        "parity: NONE\n"
        "frame_length: 3\n"
        "subframe_deepth: 1\n"
        "major_frame_rate: 1\n"
        "sync_word_length: 8\n"
        "sync_pattern: C3\n"
        "\n"
        "the_parameter_description_of_stream_1:\n"
        "serial_no.: 1\n"
        "id_no.: PSEQ\n"
        "name: ARRAY_ORDER\n"
        "unit: raw\n"
        "cal_type: NO\n"
        "code_type: uint\n"
        "cal_date: 2026-06-07\n"
        "cal_number: \n"
        "rate: 1\n"
        "word_interval: 1\n"
        "sign_bit: 0\n"
        "inverse: 0\n"
        "pcm_words: 2\n"
        "frame_no: 0,0,0,0,0,0\n"
        "word_no: 1,2,0,0,0,0\n"
        "source_bit: 0,0,0,0,0,0\n"
        "bit_length: 8,8,0,0,0,0\n"
        "object_bit: 0,0,0,0,0,0\n"
        "description: array order payload\n");
}

bool writeTextFile(const QString &file_path, const QString &content)
{
    QFile file(file_path);
    if (!file.open(QIODevice::WriteOnly | QIODevice::Text)) {
        return false;
    }

    return file.write(content.toUtf8()) == content.toUtf8().size();
}

bool writeRawFile(const QString &file_path, int frame_count = 2, bool use_incrementing_values = false)
{
    QByteArray raw_data = QByteArrayLiteral("noise");
    for (int frame_index = 0; frame_index < frame_count; ++frame_index) {
        raw_data.append(static_cast<char>(0xAA));
        const int value = use_incrementing_values
            ? ((frame_index + 1) & 0xff)
            : (frame_index == 0 ? 0x11 : 0x22);
        raw_data.append(static_cast<char>(value));
    }

    QFile file(file_path);
    if (!file.open(QIODevice::WriteOnly)) {
        return false;
    }
    return file.write(raw_data) == raw_data.size();
}

bool writeFixedTwoStreamRawFiles(const QString &first_file_path, const QString &second_file_path)
{
    QByteArray first_raw_data;
    first_raw_data.append(static_cast<char>(0xA1));
    first_raw_data.append(static_cast<char>(0x11));
    first_raw_data.append(static_cast<char>(0xA1));
    first_raw_data.append(static_cast<char>(0x12));

    QByteArray second_raw_data;
    second_raw_data.append(static_cast<char>(0xB2));
    second_raw_data.append(static_cast<char>(0x21));
    second_raw_data.append(static_cast<char>(0xB2));
    second_raw_data.append(static_cast<char>(0x22));

    QFile first_file(first_file_path);
    if (!first_file.open(QIODevice::WriteOnly)) {
        return false;
    }
    if (first_file.write(first_raw_data) != first_raw_data.size()) {
        return false;
    }

    QFile second_file(second_file_path);
    if (!second_file.open(QIODevice::WriteOnly)) {
        return false;
    }
    return second_file.write(second_raw_data) == second_raw_data.size();
}

bool writeSyncEndianRawFile(const QString &file_path)
{
    QByteArray raw_data;
    raw_data.append(static_cast<char>(0x00));
    raw_data.append(static_cast<char>(0xB2));
    raw_data.append(static_cast<char>(0xA1));
    raw_data.append(static_cast<char>(0x34));
    raw_data.append(static_cast<char>(0x12));

    QFile file(file_path);
    if (!file.open(QIODevice::WriteOnly)) {
        return false;
    }
    return file.write(raw_data) == raw_data.size();
}

bool writeFastArrayOrderRawFile(const QString &file_path)
{
    QByteArray raw_data;
    raw_data.append(static_cast<char>(0xC3));
    raw_data.append(static_cast<char>(0x12));
    raw_data.append(static_cast<char>(0x34));
    raw_data.append(static_cast<char>(0xC3));
    raw_data.append(static_cast<char>(0x56));
    raw_data.append(static_cast<char>(0x78));

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
    QCOMPARE(lines.at(0), QStringLiteral("timestamp_seconds\tTEMP"));
    QCOMPARE(lines.at(1), QStringLiteral("0.000000000\t17"));
    QCOMPARE(lines.at(2), QStringLiteral("1.000000000\t34"));
}

void TraditionalProcessingControllerTests::runsPipelineBeyondLegacyResultQueueLimit()
{
    QTemporaryDir temp_dir;
    QVERIFY(temp_dir.isValid());

    const QString config_path = temp_dir.filePath(QStringLiteral("config.cfte"));
    const QString raw_path = temp_dir.filePath(QStringLiteral("raw_many.pcm"));
    const QString output_path = temp_dir.filePath(QStringLiteral("result_many.txt"));
    const int frame_count = 300;
    QVERIFY(writeTextFile(config_path, sampleConfigText()));
    QVERIFY(writeRawFile(raw_path, frame_count, true));

    TraditionalProcessingOptions options;
    options.parameter_names << QStringLiteral("TIME") << QStringLiteral("TEMP");
    options.output_file_path = output_path;
    options.task_definition.task_id = QStringLiteral("traditional-many-values");
    options.task_definition.config_file_path = config_path;
    options.task_definition.raw_file_path = raw_path;
    options.task_definition.output_directory = temp_dir.path();
    options.task_definition.user_sample_rate_hz = 2.0;

    TraditionalProcessingController controller;
    QSignalSpy finished_spy(&controller, &TraditionalProcessingController::taskFinished);
    QSignalSpy failed_spy(&controller, &TraditionalProcessingController::taskFailed);
    QSignalSpy stopped_spy(&controller, &TraditionalProcessingController::taskStopped);

    controller.startProcessing(options);
    while (finished_spy.count() == 0 && failed_spy.count() == 0 && stopped_spy.count() == 0) {
        QVERIFY(finished_spy.wait(5000) || failed_spy.count() > 0 || stopped_spy.count() > 0);
    }

    QCOMPARE(failed_spy.count(), 0);
    QCOMPARE(stopped_spy.count(), 0);
    QCOMPARE(finished_spy.count(), 1);

    const QStringList lines = readOutputLines(output_path);
    QCOMPARE(lines.size(), frame_count + 1);
    QCOMPARE(lines.at(1), QStringLiteral("0.000000000\t1"));
    QCOMPARE(lines.at(256), QStringLiteral("255.000000000\t0"));
    QCOMPARE(lines.at(300), QStringLiteral("299.000000000\t44"));
}

void TraditionalProcessingControllerTests::runsPerStreamFixedFastPath()
{
    QTemporaryDir temp_dir;
    QVERIFY(temp_dir.isValid());

    const QString config_path = temp_dir.filePath(QStringLiteral("fixed_config.cfte"));
    const QString raw_path = temp_dir.filePath(QStringLiteral("fixed_raw.pcm"));
    const QString second_raw_path = temp_dir.filePath(QStringLiteral("fixed_raw_2.pcm"));
    const QString output_path = temp_dir.filePath(QStringLiteral("fixed_result.txt"));
    QVERIFY(writeTextFile(config_path, fixedTwoStreamConfigText()));
    QVERIFY(writeFixedTwoStreamRawFiles(raw_path, second_raw_path));

    TraditionalProcessingOptions options;
    options.parameter_names << QStringLiteral("TIME")
                            << QStringLiteral("TEMP_A")
                            << QStringLiteral("TEMP_B");
    options.output_file_path = output_path;
    options.task_definition.task_id = QStringLiteral("fixed-fast");
    options.task_definition.config_file_path = config_path;
    options.task_definition.raw_file_path = raw_path;
    options.task_definition.output_directory = temp_dir.path();
    options.task_definition.user_sample_rate_hz = 2.0;

    TraditionalProcessingController controller;
    QSignalSpy finished_spy(&controller, &TraditionalProcessingController::taskFinished);
    QSignalSpy failed_spy(&controller, &TraditionalProcessingController::taskFailed);
    QSignalSpy stopped_spy(&controller, &TraditionalProcessingController::taskStopped);

    controller.startProcessing(options);
    while (finished_spy.count() == 0 && failed_spy.count() == 0 && stopped_spy.count() == 0) {
        QVERIFY(finished_spy.wait(5000) || failed_spy.count() > 0 || stopped_spy.count() > 0);
    }

    QCOMPARE(failed_spy.count(), 0);
    QCOMPARE(stopped_spy.count(), 0);
    QCOMPARE(finished_spy.count(), 1);

    const QStringList lines = readOutputLines(output_path);
    QCOMPARE(lines.size(), 3);
    QCOMPARE(lines.at(0), QStringLiteral("timestamp_seconds\tTEMP_A\tTEMP_B"));
    QCOMPARE(lines.at(1), QStringLiteral("0.000000000\t17\t33"));
    QCOMPARE(lines.at(2), QStringLiteral("1.000000000\t18\t34"));
}

void TraditionalProcessingControllerTests::usesLargeDefaultFastBlockSize()
{
    const FastFixedPcmProcessorOptions options;
    QCOMPARE(options.block_size_bytes, static_cast<qint64>(256 * 1024 * 1024));
}

void TraditionalProcessingControllerTests::failsFastPathWhenFrameSyncIsLost()
{
    QTemporaryDir temp_dir;
    QVERIFY(temp_dir.isValid());

    const QString config_path = temp_dir.filePath(QStringLiteral("fixed_config.cfte"));
    const QString raw_path = temp_dir.filePath(QStringLiteral("fixed_raw.pcm"));
    const QString second_raw_path = temp_dir.filePath(QStringLiteral("fixed_raw_2.pcm"));
    const QString output_path = temp_dir.filePath(QStringLiteral("fixed_result.txt"));
    QVERIFY(writeTextFile(config_path, fixedTwoStreamConfigText()));

    QByteArray first_raw_data;
    first_raw_data.append(static_cast<char>(0xA1));
    first_raw_data.append(static_cast<char>(0x11));
    first_raw_data.append(static_cast<char>(0x00));
    first_raw_data.append(static_cast<char>(0x12));

    QByteArray second_raw_data;
    second_raw_data.append(static_cast<char>(0xB2));
    second_raw_data.append(static_cast<char>(0x21));
    second_raw_data.append(static_cast<char>(0xB2));
    second_raw_data.append(static_cast<char>(0x22));

    QFile first_file(raw_path);
    QVERIFY(first_file.open(QIODevice::WriteOnly));
    QCOMPARE(first_file.write(first_raw_data), static_cast<qint64>(first_raw_data.size()));
    first_file.close();

    QFile second_file(second_raw_path);
    QVERIFY(second_file.open(QIODevice::WriteOnly));
    QCOMPARE(second_file.write(second_raw_data), static_cast<qint64>(second_raw_data.size()));
    second_file.close();

    TraditionalProcessingOptions options;
    options.parameter_names << QStringLiteral("TIME")
                            << QStringLiteral("TEMP_A")
                            << QStringLiteral("TEMP_B");
    options.output_file_path = output_path;
    options.task_definition.task_id = QStringLiteral("fixed-fast-lost-sync");
    options.task_definition.config_file_path = config_path;
    options.task_definition.raw_file_path = raw_path;
    options.task_definition.output_directory = temp_dir.path();
    options.task_definition.user_sample_rate_hz = 2.0;

    TraditionalProcessingController controller;
    QSignalSpy finished_spy(&controller, &TraditionalProcessingController::taskFinished);
    QSignalSpy failed_spy(&controller, &TraditionalProcessingController::taskFailed);
    QSignalSpy stopped_spy(&controller, &TraditionalProcessingController::taskStopped);

    controller.startProcessing(options);
    while (finished_spy.count() == 0 && failed_spy.count() == 0 && stopped_spy.count() == 0) {
        QVERIFY(finished_spy.wait(5000) || failed_spy.count() > 0 || stopped_spy.count() > 0);
    }

    QCOMPARE(finished_spy.count(), 0);
    QCOMPARE(stopped_spy.count(), 0);
    QCOMPARE(failed_spy.count(), 1);
    QVERIFY(failed_spy.first().at(1).toString().contains(QStringLiteral("lost sync")));
}

void TraditionalProcessingControllerTests::acceptsFastPathWithDifferentFrameLengths()
{
    QTemporaryDir temp_dir;
    QVERIFY(temp_dir.isValid());

    const QString raw_path = temp_dir.filePath(QStringLiteral("fixed_mixed_lengths.pcm"));
    const QString second_raw_path = temp_dir.filePath(QStringLiteral("fixed_mixed_lengths_2.pcm"));
    QByteArray first_raw_data;
    first_raw_data.append(static_cast<char>(0xA1));
    first_raw_data.append(static_cast<char>(0x11));

    QByteArray second_raw_data;
    second_raw_data.append(static_cast<char>(0xB2));
    second_raw_data.append(static_cast<char>(0x00));
    second_raw_data.append(static_cast<char>(0x21));

    QFile first_file(raw_path);
    QVERIFY(first_file.open(QIODevice::WriteOnly));
    QCOMPARE(first_file.write(first_raw_data), static_cast<qint64>(first_raw_data.size()));
    first_file.close();

    QFile second_file(second_raw_path);
    QVERIFY(second_file.open(QIODevice::WriteOnly));
    QCOMPARE(second_file.write(second_raw_data), static_cast<qint64>(second_raw_data.size()));
    second_file.close();

    StreamDefinition first_stream;
    first_stream.stream_index = 1;
    first_stream.stream_id = QStringLiteral("S1");
    first_stream.sync_word = QByteArray(1, static_cast<char>(0xA1));
    first_stream.frame_length_bytes = 2;
    first_stream.samples_per_frame = 2;

    StreamDefinition second_stream;
    second_stream.stream_index = 2;
    second_stream.stream_id = QStringLiteral("S2");
    second_stream.sync_word = QByteArray(1, static_cast<char>(0xB2));
    second_stream.frame_length_bytes = 3;
    second_stream.samples_per_frame = 3;

    StreamLayoutTable first_layout;
    first_layout.stream_id = first_stream.stream_id;
    first_layout.stream_index = 1;
    first_layout.word_length_bits = 8;
    first_layout.frame_length_words = 2;
    first_layout.subframe_depth = 1;
    StreamLayoutEntry first_entry;
    first_entry.parameter_id = QStringLiteral("A");
    first_entry.parameter_name = QStringLiteral("A");
    first_entry.code_type = ParameterCodeType::UnsignedInteger;
    ParameterWordSegment first_segment;
    first_segment.frame_no = 0;
    first_segment.word_no = 1;
    first_segment.source_bit = 0;
    first_segment.bit_length = 8;
    first_entry.word_segments.append(first_segment);
    first_layout.entries.append(first_entry);

    StreamLayoutTable second_layout;
    second_layout.stream_id = second_stream.stream_id;
    second_layout.stream_index = 2;
    second_layout.word_length_bits = 8;
    second_layout.frame_length_words = 3;
    second_layout.subframe_depth = 1;
    StreamLayoutEntry second_entry;
    second_entry.parameter_id = QStringLiteral("B");
    second_entry.parameter_name = QStringLiteral("B");
    second_entry.code_type = ParameterCodeType::UnsignedInteger;
    ParameterWordSegment second_segment;
    second_segment.frame_no = 0;
    second_segment.word_no = 2;
    second_segment.source_bit = 0;
    second_segment.bit_length = 8;
    second_entry.word_segments.append(second_segment);
    second_layout.entries.append(second_entry);

    TaskDefinition definition;
    definition.task_id = QStringLiteral("fixed-mixed-lengths");
    definition.raw_file_path = raw_path;

    QString reason;
    QVERIFY2(FastFixedPcmProcessor::canUseFastPath(definition,
                                                  {first_stream, second_stream},
                                                  {first_layout, second_layout},
                                                  QHash<QString, QSet<QString>>(),
                                                  &reason),
             qPrintable(reason));
}

void TraditionalProcessingControllerTests::acceptsFastPathWithZeroBasedLastIndexes()
{
    QTemporaryDir temp_dir;
    QVERIFY(temp_dir.isValid());

    const QString raw_path = temp_dir.filePath(QStringLiteral("zero-based-last-index.pcm"));
    QByteArray raw_data;
    raw_data.append(static_cast<char>(0xA1));
    raw_data.append(static_cast<char>(0x11));

    QFile raw_file(raw_path);
    QVERIFY(raw_file.open(QIODevice::WriteOnly));
    QCOMPARE(raw_file.write(raw_data), static_cast<qint64>(raw_data.size()));
    raw_file.close();

    StreamDefinition stream;
    stream.stream_index = 1;
    stream.stream_id = QStringLiteral("S1");
    stream.sync_word = QByteArray(1, static_cast<char>(0xA1));
    stream.frame_length_bytes = 2;
    stream.samples_per_frame = 2;

    StreamLayoutTable layout;
    layout.stream_id = stream.stream_id;
    layout.stream_index = 1;
    layout.word_length_bits = 8;
    layout.frame_length_words = 2;
    layout.subframe_depth = 1;

    StreamLayoutEntry offset_entry;
    offset_entry.parameter_id = QStringLiteral("P0");
    offset_entry.parameter_name = QStringLiteral("OFFSET_STYLE");
    offset_entry.code_type = ParameterCodeType::UnsignedInteger;
    ParameterWordSegment offset_segment;
    offset_segment.frame_no = 0;
    offset_segment.word_no = 0;
    offset_segment.source_bit = 0;
    offset_segment.bit_length = 8;
    offset_entry.word_segments.append(offset_segment);
    layout.entries.append(offset_entry);

    StreamLayoutEntry last_entry;
    last_entry.parameter_id = QStringLiteral("PLAST");
    last_entry.parameter_name = QStringLiteral("LAST_WORD");
    last_entry.code_type = ParameterCodeType::UnsignedInteger;
    ParameterWordSegment last_segment;
    last_segment.frame_no = 0;
    last_segment.word_no = 1;
    last_segment.source_bit = 0;
    last_segment.bit_length = 8;
    last_entry.word_segments.append(last_segment);
    layout.entries.append(last_entry);
    layout.rebuildIndexes();

    TaskDefinition definition;
    definition.task_id = QStringLiteral("zero-based-last-index");
    definition.raw_file_path = raw_path;

    QHash<QString, QSet<QString>> selected;
    selected.insert(stream.stream_id, {QStringLiteral("P0"), QStringLiteral("PLAST")});

    QString reason;
    QVERIFY2(FastFixedPcmProcessor::canUseFastPath(definition,
                                                  {stream},
                                                  {layout},
                                                  selected,
                                                  &reason),
             qPrintable(reason));
}

void TraditionalProcessingControllerTests::processesDuplicateSyncWordsFromSeparateStreamFiles()
{
    QTemporaryDir temp_dir;
    QVERIFY(temp_dir.isValid());

    const QString config_path = temp_dir.filePath(QStringLiteral("duplicate-sync.cfte"));
    const QString raw_path = temp_dir.filePath(QStringLiteral("raw.pcm"));
    const QString second_raw_path = temp_dir.filePath(QStringLiteral("raw_2.pcm"));
    const QString output_path = temp_dir.filePath(QStringLiteral("result.txt"));
    QVERIFY(writeTextFile(config_path, duplicateSyncConfigText()));
    QVERIFY(writeRawFile(raw_path));
    QVERIFY(writeRawFile(second_raw_path));

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

    QCOMPARE(finished_spy.count(), 1);
    QCOMPARE(stopped_spy.count(), 0);
    QCOMPARE(failed_spy.count(), 0);
    const QStringList lines = readOutputLines(output_path);
    QCOMPARE(lines.size(), 3);
    QCOMPARE(lines.at(0), QStringLiteral("timestamp_seconds\tTEMP_A\tTEMP_B"));
    QCOMPARE(lines.at(1), QStringLiteral("0.000000000\t17\t17"));
    QCOMPARE(lines.at(2), QStringLiteral("1.000000000\t34\t34"));
    QTest::qWait(50);
}

void TraditionalProcessingControllerTests::matchesLittleEndianSyncAndPayloadWordBytes()
{
    QTemporaryDir temp_dir;
    QVERIFY(temp_dir.isValid());

    const QString config_path = temp_dir.filePath(QStringLiteral("sync-endian.cfte"));
    const QString raw_path = temp_dir.filePath(QStringLiteral("sync-endian.pcm"));
    const QString output_path = temp_dir.filePath(QStringLiteral("sync-endian-result.txt"));
    QVERIFY(writeTextFile(config_path, syncEndianConfigText()));
    QVERIFY(writeSyncEndianRawFile(raw_path));

    TraditionalProcessingOptions options;
    options.parameter_names << QStringLiteral("TIME") << QStringLiteral("PAYLOAD");
    options.output_file_path = output_path;
    options.task_definition.task_id = QStringLiteral("sync-endian");
    options.task_definition.config_file_path = config_path;
    options.task_definition.raw_file_path = raw_path;
    options.task_definition.output_directory = temp_dir.path();
    options.task_definition.user_sample_rate_hz = 2.0;

    TraditionalProcessingController controller;
    QSignalSpy finished_spy(&controller, &TraditionalProcessingController::taskFinished);
    QSignalSpy failed_spy(&controller, &TraditionalProcessingController::taskFailed);
    QSignalSpy stopped_spy(&controller, &TraditionalProcessingController::taskStopped);

    controller.startProcessing(options);
    while (finished_spy.count() == 0 && failed_spy.count() == 0 && stopped_spy.count() == 0) {
        QVERIFY(finished_spy.wait(5000) || failed_spy.count() > 0 || stopped_spy.count() > 0);
    }

    QCOMPARE(failed_spy.count(), 0);
    QCOMPARE(stopped_spy.count(), 0);
    QCOMPARE(finished_spy.count(), 1);

    const QStringList lines = readOutputLines(output_path);
    QCOMPARE(lines.size(), 2);
    QCOMPARE(lines.at(0), QStringLiteral("timestamp_seconds\tPAYLOAD"));
    QCOMPARE(lines.at(1), QStringLiteral("0.000000000\t4660"));
}

void TraditionalProcessingControllerTests::usesCompiledFastPathArrayOrderForMultiWordParameters()
{
    QTemporaryDir temp_dir;
    QVERIFY(temp_dir.isValid());

    const QString config_path = temp_dir.filePath(QStringLiteral("fast-array-order.cfte"));
    const QString raw_path = temp_dir.filePath(QStringLiteral("fast-array-order.pcm"));
    const QString output_path = temp_dir.filePath(QStringLiteral("fast-array-order-result.txt"));
    QVERIFY(writeTextFile(config_path, fastArrayOrderConfigText()));
    QVERIFY(writeFastArrayOrderRawFile(raw_path));

    TraditionalProcessingOptions options;
    options.parameter_names << QStringLiteral("TIME") << QStringLiteral("ARRAY_ORDER");
    options.output_file_path = output_path;
    options.task_definition.task_id = QStringLiteral("fast-array-order");
    options.task_definition.config_file_path = config_path;
    options.task_definition.raw_file_path = raw_path;
    options.task_definition.output_directory = temp_dir.path();
    options.task_definition.user_sample_rate_hz = 3.0;

    TraditionalProcessingController controller;
    QSignalSpy finished_spy(&controller, &TraditionalProcessingController::taskFinished);
    QSignalSpy failed_spy(&controller, &TraditionalProcessingController::taskFailed);
    QSignalSpy stopped_spy(&controller, &TraditionalProcessingController::taskStopped);

    controller.startProcessing(options);
    while (finished_spy.count() == 0 && failed_spy.count() == 0 && stopped_spy.count() == 0) {
        QVERIFY(finished_spy.wait(5000) || failed_spy.count() > 0 || stopped_spy.count() > 0);
    }

    QCOMPARE(failed_spy.count(), 0);
    QCOMPARE(stopped_spy.count(), 0);
    QCOMPARE(finished_spy.count(), 1);

    const QStringList lines = readOutputLines(output_path);
    QCOMPARE(lines.size(), 3);
    QCOMPARE(lines.at(0), QStringLiteral("timestamp_seconds\tARRAY_ORDER"));
    QCOMPARE(lines.at(1), QStringLiteral("0.000000000\t4660"));
    QCOMPARE(lines.at(2), QStringLiteral("1.000000000\t22136"));
}

void TraditionalProcessingControllerTests::runsMultipleRangesWithIndependentFiles()
{
    QTemporaryDir temp_dir;
    QVERIFY(temp_dir.isValid());

    const QString config_path = temp_dir.filePath(QStringLiteral("config.cfte"));
    const QString raw_path = temp_dir.filePath(QStringLiteral("raw.pcm"));
    const QString output_path = temp_dir.filePath(QStringLiteral("multi.txt"));
    QVERIFY(writeTextFile(config_path, sampleConfigText()));
    QVERIFY(writeRawFile(raw_path));

    TraditionalProcessingOptions options;
    options.parameter_names << QStringLiteral("TIME") << QStringLiteral("TEMP");
    options.output_file_path = output_path;
    options.task_definition.task_id = QStringLiteral("multi-range");
    options.task_definition.config_file_path = config_path;
    options.task_definition.raw_file_path = raw_path;
    options.task_definition.output_directory = temp_dir.path();
    options.task_definition.user_sample_rate_hz = 2.0;
    TaskTimeRange first_range;
    first_range.start_seconds = 0.0;
    first_range.end_seconds = 1.0;
    TaskTimeRange second_range;
    second_range.start_seconds = 1.0;
    second_range.end_seconds = 2.0;
    options.time_ranges = {first_range, second_range};

    TraditionalProcessingController controller;
    QSignalSpy range_spy(&controller, &TraditionalProcessingController::rangeFinished);
    QSignalSpy final_spy(&controller, &TraditionalProcessingController::processingFinished);
    QSignalSpy failed_spy(&controller, &TraditionalProcessingController::taskFailed);
    QSignalSpy stopped_spy(&controller, &TraditionalProcessingController::taskStopped);

    controller.startProcessing(options);
    while (final_spy.count() == 0 && failed_spy.count() == 0 && stopped_spy.count() == 0) {
        QVERIFY(final_spy.wait(5000) || failed_spy.count() > 0 || stopped_spy.count() > 0);
    }

    QCOMPARE(failed_spy.count(), 0);
    QCOMPARE(stopped_spy.count(), 0);
    QCOMPARE(range_spy.count(), 2);
    QCOMPARE(final_spy.count(), 1);
    QCOMPARE(final_spy.first().at(0).toString(), options.task_definition.task_id);

    const QString first_output = temp_dir.filePath(QStringLiteral("multi_range001.txt"));
    const QString second_output = temp_dir.filePath(QStringLiteral("multi_range002.txt"));
    QCOMPARE(final_spy.first().at(1).toStringList(), QStringList({first_output, second_output}));
    const QStringList first_lines = readOutputLines(first_output);
    const QStringList second_lines = readOutputLines(second_output);
    QCOMPARE(first_lines.size(), 2);
    QCOMPARE(second_lines.size(), 2);
    QCOMPARE(first_lines.at(1), QStringLiteral("0.000000000\t17"));
    QCOMPARE(second_lines.at(1), QStringLiteral("1.000000000\t34"));
}

void TraditionalProcessingControllerTests::limitsFastPathToRequestedRange()
{
    QTemporaryDir temp_dir;
    QVERIFY(temp_dir.isValid());

    const QString config_path = temp_dir.filePath(QStringLiteral("fixed_config.cfte"));
    const QString raw_path = temp_dir.filePath(QStringLiteral("fixed_raw.pcm"));
    const QString second_raw_path = temp_dir.filePath(QStringLiteral("fixed_raw_2.pcm"));
    const QString output_path = temp_dir.filePath(QStringLiteral("fixed_range.txt"));
    QVERIFY(writeTextFile(config_path, fixedTwoStreamConfigText()));
    QVERIFY(writeFixedTwoStreamRawFiles(raw_path, second_raw_path));

    TraditionalProcessingOptions options;
    options.parameter_names << QStringLiteral("TIME")
                            << QStringLiteral("TEMP_A")
                            << QStringLiteral("TEMP_B");
    options.output_file_path = output_path;
    options.include_raw_values = true;
    options.task_definition.task_id = QStringLiteral("fixed-fast-range");
    options.task_definition.config_file_path = config_path;
    options.task_definition.raw_file_path = raw_path;
    options.task_definition.output_directory = temp_dir.path();
    options.task_definition.user_sample_rate_hz = 2.0;
    TaskTimeRange range;
    range.start_seconds = 1.0;
    range.end_seconds = 2.0;
    options.time_ranges = {range};

    TraditionalProcessingController controller;
    QSignalSpy final_spy(&controller, &TraditionalProcessingController::processingFinished);
    QSignalSpy failed_spy(&controller, &TraditionalProcessingController::taskFailed);
    QSignalSpy stopped_spy(&controller, &TraditionalProcessingController::taskStopped);

    controller.startProcessing(options);
    while (final_spy.count() == 0 && failed_spy.count() == 0 && stopped_spy.count() == 0) {
        QVERIFY(final_spy.wait(5000) || failed_spy.count() > 0 || stopped_spy.count() > 0);
    }

    QCOMPARE(failed_spy.count(), 0);
    QCOMPARE(stopped_spy.count(), 0);
    QCOMPARE(final_spy.count(), 1);
    const QStringList lines = readOutputLines(output_path);
    QCOMPARE(lines.size(), 2);
    QCOMPARE(lines.at(0),
             QStringLiteral("timestamp_seconds\tTEMP_A_raw\tTEMP_A\tTEMP_B_raw\tTEMP_B"));
    QCOMPARE(lines.at(1), QStringLiteral("1.000000000\t18\t18\t34\t34"));
}

void TraditionalProcessingControllerTests::stopsAfterCompletedRangeAndKeepsItsFile()
{
    QTemporaryDir temp_dir;
    QVERIFY(temp_dir.isValid());
    const QString config_path = temp_dir.filePath(QStringLiteral("config.cfte"));
    const QString raw_path = temp_dir.filePath(QStringLiteral("raw.pcm"));
    const QString output_path = temp_dir.filePath(QStringLiteral("stopped.txt"));
    QVERIFY(writeTextFile(config_path, sampleConfigText()));
    QVERIFY(writeRawFile(raw_path));

    TraditionalProcessingOptions options;
    options.parameter_names << QStringLiteral("TIME") << QStringLiteral("TEMP");
    options.output_file_path = output_path;
    options.task_definition.task_id = QStringLiteral("stop-between-ranges");
    options.task_definition.config_file_path = config_path;
    options.task_definition.raw_file_path = raw_path;
    options.task_definition.output_directory = temp_dir.path();
    options.task_definition.user_sample_rate_hz = 2.0;
    for (int range_index = 0; range_index < 3; ++range_index) {
        TaskTimeRange range;
        range.start_seconds = range_index;
        range.end_seconds = range_index + 1;
        options.time_ranges.append(range);
    }

    TraditionalProcessingController controller;
    QSignalSpy range_spy(&controller, &TraditionalProcessingController::rangeFinished);
    QSignalSpy stopped_spy(&controller, &TraditionalProcessingController::taskStopped);
    QSignalSpy failed_spy(&controller, &TraditionalProcessingController::taskFailed);
    QSignalSpy final_spy(&controller, &TraditionalProcessingController::processingFinished);
    connect(&controller,
            &TraditionalProcessingController::rangeFinished,
            &controller,
            [&controller](const QString &, int range_index, int, const QString &) {
                if (range_index == 1) {
                    controller.requestStop();
                }
            });

    controller.startProcessing(options);
    while (stopped_spy.count() == 0 && failed_spy.count() == 0 && final_spy.count() == 0) {
        QVERIFY(stopped_spy.wait(5000) || failed_spy.count() > 0 || final_spy.count() > 0);
    }

    QCOMPARE(failed_spy.count(), 0);
    QCOMPARE(final_spy.count(), 0);
    QCOMPARE(stopped_spy.count(), 1);
    QCOMPARE(stopped_spy.first().at(0).toString(), options.task_definition.task_id);
    QCOMPARE(range_spy.count(), 1);
    const QString first_output = temp_dir.filePath(QStringLiteral("stopped_range001.txt"));
    const QString second_output = temp_dir.filePath(QStringLiteral("stopped_range002.txt"));
    QVERIFY(QFile::exists(first_output));
    QVERIFY(!QFile::exists(second_output));
    QCOMPARE(readOutputLines(first_output).at(1), QStringLiteral("0.000000000\t17"));
}

QTEST_MAIN(TraditionalProcessingControllerTests)

#include "traditional_processing_controller_tests.moc"
