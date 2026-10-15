#include "result_writer.h"

#include <QFile>
#include <QSignalSpy>
#include <QTemporaryDir>
#include <QtTest>
#include <QtConcurrent/QtConcurrentRun>
#include <cmath>
#include <cstring>
#include <limits>
#include <random>

class ResultWriterTests : public QObject
{
    Q_OBJECT

private slots:
    /**
     * @brief Verifies timestamp reorder and multi-stream timestamp-row merging.
     */
    void writesTimeOrderedMergedTaskFile();

    /**
     * @brief Verifies that interleaved task results never share the same output file.
     */
    void keepsTaskOutputsIndependent();
    /** @brief 流式及批量输出均保留同毫秒内的多行，并使用旧版 TIME 格式。 */
    void legacyTimeFormatKeepsSubmillisecondRows_data();
    void legacyTimeFormatKeepsSubmillisecondRows();
    /** @brief 顺序列提示在乱序、跨流同 ID、批次切割时仍保持列映射和数值精度。 */
    void streamingColumnHintsPreserveOutput();
    /** @brief 直接对照未优化的批量输出路径，覆盖数值边界、编码、乱序及缓冲边界。 */
    void streamingMatchesOriginalBytes_data();
    void streamingMatchesOriginalBytes();
    /** @brief 启动后的选项不受其他任务修改影响，批量和流式路径均隔离。 */
    void taskOptionsRemainIndependent_data();
    void taskOptionsRemainIndependent();
    /** @brief 跨缓冲写入后取消仍保留既有文件，不影响另一任务提交。 */
    void abortPreservesDestinationAndOtherTask();
    /** @brief 三个工作线程交错追加，其中一个取消，其余文件与独立批量输出一致。 */
    void concurrentStreamingTasksStayIndependent();
    /** @brief 空任务只输出表头，写入及提交失败不会发出成功信号。 */
    void emptyAndFailedStreamingTasks();
};

namespace {

TaskDefinition makeTask(const QString &task_id, const QString &output_directory)
{
    TaskDefinition definition;
    definition.task_id = task_id;
    definition.config_file_path = QStringLiteral("config.cfte");
    definition.raw_file_path = QStringLiteral("raw.pcm");
    definition.output_directory = output_directory;
    return definition;
}

ParameterGroupSelection makeGroup(int stream_index, const QList<QString> &parameter_ids)
{
    ParameterGroupSelection group;
    group.stream_index = stream_index;
    group.parameter_ids = parameter_ids;
    return group;
}

ParameterValue makeValue(const QString &task_id,
                         int stream_index,
                         const QString &parameter_id,
                         const QString &parameter_name,
                         double timestamp_seconds,
                         const QVariant &value,
                         const QString &unit = QString())
{
    ParameterValue parameter_value;
    parameter_value.task_id = task_id;
    parameter_value.stream_index = stream_index;
    parameter_value.parameter_id = parameter_id;
    parameter_value.parameter_name = parameter_name;
    parameter_value.timestamp_seconds = timestamp_seconds;
    parameter_value.value = value;
    parameter_value.unit = unit;
    return parameter_value;
}

QStringList readOutputLines(const QString &path)
{
    QFile file(path);
    if (!file.open(QIODevice::ReadOnly | QIODevice::Text)) {
        return {};
    }

    QString content = QString::fromUtf8(file.readAll());
    content.replace(QStringLiteral("\r\n"), QStringLiteral("\n"));
    if (content.endsWith('\n')) {
        content.chop(1);
    }

    return content.split('\n');
}

/** @brief 不启用 Text 模式，比较包含换行和 UTF-8 编码在内的原始字节。 */
QByteArray readOutputBytes(const QString &path)
{
    QFile file(path);
    return file.open(QIODevice::ReadOnly) ? file.readAll() : QByteArray();
}

/** @brief 根据测试任务的分组选列创建同名布局，允许跨流重复 ID。 */
QList<StreamLayoutTable> makeLayouts(const TaskDefinition &task)
{
    QList<StreamLayoutTable> layouts;
    for (const ParameterGroupSelection &group : task.parameter_groups) {
        StreamLayoutTable layout;
        layout.stream_index = group.stream_index;
        for (const QString &id : group.parameter_ids) {
            StreamLayoutEntry entry;
            entry.parameter_id = id;
            entry.parameter_name = id;
            layout.entries.append(entry);
        }
        layout.rebuildIndexes();
        layouts.append(layout);
    }
    return layouts;
}

} // namespace

void ResultWriterTests::writesTimeOrderedMergedTaskFile()
{
    QTemporaryDir temp_dir;
    QVERIFY(temp_dir.isValid());

    TaskDefinition task = makeTask(QStringLiteral("task-merge"), temp_dir.path());
    task.parameter_groups = {
        makeGroup(1, {QStringLiteral("P1"), QStringLiteral("P2")}),
        makeGroup(2, {QStringLiteral("P3")}),
    };

    ResultWriter writer;
    ResultWriterOptions options;
    options.timestamp_precision = 3;
    writer.setOptions(options);

    QSignalSpy finished_spy(&writer, &ResultWriter::taskOutputFinished);

    QString error_message;
    QVERIFY2(writer.startTask(task, &error_message), qPrintable(error_message));

    QList<ParameterValue> values;
    values.append(makeValue(task.task_id,
                            1,
                            QStringLiteral("P1"),
                            QStringLiteral("alpha"),
                            1.0,
                            11.0,
                            QStringLiteral("V")));
    values.append(makeValue(task.task_id,
                            2,
                            QStringLiteral("P3"),
                            QStringLiteral("gamma"),
                            0.0,
                            30.0,
                            QStringLiteral("A")));
    values.append(makeValue(task.task_id,
                            1,
                            QStringLiteral("P1"),
                            QStringLiteral("alpha"),
                            0.0,
                            10.0,
                            QStringLiteral("V")));
    values.append(makeValue(task.task_id,
                            1,
                            QStringLiteral("P2"),
                            QStringLiteral("beta"),
                            0.5,
                            20.0,
                            QStringLiteral("kPa")));
    QVERIFY2(writer.appendValues(values, &error_message), qPrintable(error_message));

    QString output_path;
    QVERIFY2(writer.finishTask(task.task_id, &output_path, &error_message), qPrintable(error_message));
    QCOMPARE(finished_spy.count(), 1);

    const QStringList lines = readOutputLines(output_path);
    QCOMPARE(lines.size(), 4);
    QCOMPARE(lines.at(0), QStringLiteral("timestamp_seconds\talpha\tbeta\tgamma"));
    QCOMPARE(lines.at(1), QStringLiteral("0.000\t10\t\t30"));
    QCOMPARE(lines.at(2), QStringLiteral("0.500\t\t20\t"));
    QCOMPARE(lines.at(3), QStringLiteral("1.000\t11\t\t"));
}

void ResultWriterTests::keepsTaskOutputsIndependent()
{
    QTemporaryDir temp_dir;
    QVERIFY(temp_dir.isValid());

    ResultWriter writer;
    ResultWriterOptions options;
    options.timestamp_precision = 1;
    writer.setOptions(options);

    const TaskDefinition first_task = makeTask(QStringLiteral("task-A"), temp_dir.path());
    const TaskDefinition second_task = makeTask(QStringLiteral("task-B"), temp_dir.path());

    QString error_message;
    QVERIFY2(writer.startTask(first_task, &error_message), qPrintable(error_message));
    QVERIFY2(writer.startTask(second_task, &error_message), qPrintable(error_message));
    QVERIFY2(writer.appendValue(makeValue(first_task.task_id, 1, QStringLiteral("P1"), QString(), 0.0, 100),
                                &error_message),
             qPrintable(error_message));
    QVERIFY2(writer.appendValue(makeValue(second_task.task_id, 1, QStringLiteral("P1"), QString(), 0.0, 200),
                                &error_message),
             qPrintable(error_message));

    QString first_output_path;
    QString second_output_path;
    QVERIFY2(writer.finishTask(second_task.task_id, &second_output_path, &error_message), qPrintable(error_message));
    QVERIFY2(writer.finishTask(first_task.task_id, &first_output_path, &error_message), qPrintable(error_message));

    QVERIFY(first_output_path != second_output_path);
    QVERIFY(QFile::exists(first_output_path));
    QVERIFY(QFile::exists(second_output_path));

    const QStringList first_lines = readOutputLines(first_output_path);
    const QStringList second_lines = readOutputLines(second_output_path);
    QCOMPARE(first_lines.at(1), QStringLiteral("0.0\t100"));
    QCOMPARE(second_lines.at(1), QStringLiteral("0.0\t200"));
}

void ResultWriterTests::legacyTimeFormatKeepsSubmillisecondRows_data()
{
    QTest::addColumn<bool>("streaming");
    QTest::newRow("streaming") << true;
    QTest::newRow("buffered") << false;
}

void ResultWriterTests::legacyTimeFormatKeepsSubmillisecondRows()
{
    QFETCH(bool, streaming);
    QTemporaryDir directory;
    auto task = makeTask("legacy-time", directory.path());
    task.parameter_groups = {makeGroup(1, {"P1"})};
    ResultWriterOptions options;
    options.output_file_path = directory.filePath("clock.txt");
    options.legacy_time_format = true;
    ResultWriter writer;
    writer.setOptions(options);
    QList<ParameterValue> values;
    for (const auto &item : QList<QPair<double, int>>{{3723.015625, 1}, {3723.0159, 2},
                                                     {3723.016, 3}, {86399.999999, 4}, {86400.0, 5}}) {
        values.append(makeValue(task.task_id, 1, "P1", "VALUE", item.first, item.second));
    }
    QString error;
    if (streaming) {
        StreamLayoutTable layout;
        layout.stream_index = 1;
        StreamLayoutEntry entry;
        entry.parameter_id = "P1";
        entry.parameter_name = "VALUE";
        layout.entries.append(entry);
        layout.rebuildIndexes();
        QVERIFY2(writer.startStreamingTask(task, {layout}, &error), qPrintable(error));
        // 跨批次、同一可见毫秒仍必须保留独立行。
        QVERIFY2(writer.appendDecodedBlock(task.task_id, values.mid(0, 1), &error), qPrintable(error));
        QVERIFY2(writer.appendDecodedBlock(task.task_id, values.mid(1), &error), qPrintable(error));
    } else {
        QVERIFY2(writer.startTask(task, &error), qPrintable(error));
        QVERIFY2(writer.appendValues(values, &error), qPrintable(error));
    }
    QString output;
    QVERIFY2(writer.finishTask(task.task_id, &output, &error), qPrintable(error));
    QCOMPARE(readOutputLines(output), QStringList({"TIME\tVALUE", "01:02:03:015\t1",
        "01:02:03:015\t2", "01:02:03:016\t3", "23:59:59:999\t4", "00:00:00:000\t5"}));
}


void ResultWriterTests::streamingColumnHintsPreserveOutput()
{
    QTemporaryDir directory;
    TaskDefinition task = makeTask("hints", directory.path());
    task.parameter_groups = {makeGroup(1, {"P", "Q"}), makeGroup(2, {"P"})};
    QList<StreamLayoutTable> layouts;
    for (int stream = 1; stream <= 2; ++stream) {
        StreamLayoutTable layout;
        layout.stream_index = stream;
        for (const QString &id : (stream == 1 ? QStringList{"P", "Q"} : QStringList{"P"})) {
            StreamLayoutEntry entry;
            entry.parameter_id = id;
            entry.parameter_name = id + QString::number(stream);
            layout.entries.append(entry);
        }
        layout.rebuildIndexes();
        layouts.append(layout);
    }
    const QList<QVariant> numbers = {
        QVariant::fromValue(std::numeric_limits<quint64>::max()),
        QVariant::fromValue(quint64(9007199254740993ULL)),
        QVariant::fromValue(std::numeric_limits<qlonglong>::min()),
        QVariant(1.2345678901234567), QVariant(-0.0), QVariant(0.0), QVariant(),
        QVariant(QStringLiteral("a\tb\nc")), QVariant(true), QVariant(1e-200), QVariant(1e200)
    };
    QList<ParameterValue> values;
    QStringList expected_lines = {"TIME\tP1_raw\tP1\tQ1_raw\tQ1\tP2_raw\tP2"};
    for (int row = 0; row < numbers.size(); ++row) {
        // 同一行按不同次序到达，并使用带空白的 ID 验证查找回退。
        QList<ParameterValue> row_values;
        row_values.append(makeValue(task.task_id, 2, " P ", "P2", row, row + 200));
        row_values.append(makeValue(task.task_id, 1, "P", "P1", row, numbers.at(row)));
        row_values.append(makeValue(task.task_id, 1, "Q", "Q1", row, row + 100));
        if (row % 2 == 0) {
            std::reverse(row_values.begin(), row_values.end());
        }
        for (ParameterValue &value : row_values) {
            value.raw_value = QVariant::fromValue(quint64(9007199254740993ULL + row));
            values.append(value);
        }
        const QVariant &number = numbers.at(row);
        QString numeric_text;
        if (number.userType() == QMetaType::Double) {
            numeric_text = QLocale::c().toString(number.toDouble(), 'g', 17);
        } else if (number.userType() == QMetaType::Bool) {
            numeric_text = number.toBool() ? "true" : "false";
        } else {
            numeric_text = number.toString();
        }
        numeric_text.replace('\t', ' ').replace('\r', ' ').replace('\n', ' ');
        const QString raw = QString::number(quint64(9007199254740993ULL + row));
        expected_lines.append(QStringLiteral("00:00:%1:000\t%2\t%3\t%2\t%4\t%2\t%5")
            .arg(row, 2, 10, QChar('0')).arg(raw, numeric_text).arg(row + 100).arg(row + 200));
    }
    ResultWriter writer;
    ResultWriterOptions options;
    options.legacy_time_format = true;
    options.include_raw_values = true;
    options.output_file_path = directory.filePath("hints.txt");
    writer.setOptions(options);
    QString error, path;
    QVERIFY2(writer.startStreamingTask(task, layouts, &error), qPrintable(error));
    for (int offset = 0; offset < values.size(); offset += 2) {
        QVERIFY2(writer.appendDecodedBlock(task.task_id, values.mid(offset, 2), &error), qPrintable(error));
    }
    QVERIFY2(writer.finishTask(task.task_id, &path, &error), qPrintable(error));
    QCOMPARE(readOutputLines(path), expected_lines);
}

void ResultWriterTests::streamingMatchesOriginalBytes_data()
{
    QTest::addColumn<bool>("raw");
    QTest::addColumn<bool>("legacy");
    QTest::newRow("engineering-seconds") << false << false;
    QTest::newRow("raw-seconds") << true << false;
    QTest::newRow("engineering-legacy") << false << true;
    QTest::newRow("raw-legacy") << true << true;
}

void ResultWriterTests::streamingMatchesOriginalBytes()
{
    QFETCH(bool, raw);
    QFETCH(bool, legacy);
    QTemporaryDir directory;
    TaskDefinition task = makeTask("bytes", directory.path());
    const QString id = QStringLiteral("中\t文\n列");
    task.parameter_groups = {makeGroup(1, {id, "missing"}), makeGroup(2, {id})};
    QList<QVariant> numbers = {
        QVariant(), QVariant(QString()), QVariant(0), QVariant(-1), QVariant(uint(4294967295U)),
        QVariant::fromValue(std::numeric_limits<qlonglong>::min()),
        QVariant::fromValue(std::numeric_limits<qlonglong>::max()),
        QVariant::fromValue(std::numeric_limits<quint64>::max()),
        QVariant::fromValue(quint64(9007199254740993ULL)), QVariant(-0.0), QVariant(0.0),
        QVariant(std::numeric_limits<double>::infinity()), QVariant(-std::numeric_limits<double>::infinity()),
        QVariant(std::numeric_limits<double>::quiet_NaN()), QVariant(std::numeric_limits<double>::denorm_min()),
        QVariant(std::numeric_limits<double>::max()), QVariant(std::numeric_limits<double>::min()),
        QVariant(true), QVariant(false), QVariant(QStringLiteral("文本😀\t\r\n\ufeff")),
        QVariant(QString(QChar(0xd800))), QVariant(QByteArray("\0\xff\t", 3)),
        QVariant(QDate(2024, 2, 29)), QVariant(QTime(23, 59, 59, 999)),
        QVariant(QDateTime(QDate(2024, 2, 29), QTime(12, 34, 56, 789), Qt::UTC))
    };
    for (quint64 power = 1; power <= 1000000000000000000ULL; power *= 10) {
        for (int delta = -1; delta <= 1; ++delta) {
            numbers.append(QVariant::fromValue(power + delta));
            numbers.append(QVariant::fromValue(-qlonglong(power) + delta));
        }
    }
    for (double boundary : {1e-5, 1e-4, 1.0, 1e15, 1e16, 1e17, 1e18, 9007199254740992.0}) {
        for (double number : {std::nextafter(boundary, 0.0), boundary,
                              std::nextafter(boundary, std::numeric_limits<double>::infinity())}) {
            numbers.append(number);
            numbers.append(-number);
        }
    }
    std::mt19937_64 random(20260920);
    for (int index = 0; index < 4500; ++index) {
        const quint64 bits = random();
        double value;
        std::memcpy(&value, &bits, sizeof(value));
        numbers.append(value);
        const quint32 float_bits = quint32(random());
        float float_value;
        std::memcpy(&float_value, &float_bits, sizeof(float_value));
        numbers.append(QVariant::fromValue(float_value));
    }
    QList<ParameterValue> values;
    for (int row = 0; row < numbers.size(); ++row) {
        for (int stream : {2, 1}) {
            ParameterValue value = makeValue(task.task_id, stream, id, id,
                86399.98 + double(row) / 65536, numbers.at(row));
            value.raw_value = numbers.at((row + 1) % numbers.size());
            values.append(value);
            if (row % 19 == 0) {
                value.value = -42; // 相同列重复值按最后到达者覆盖。
                values.append(value);
            }
        }
    }
    ResultWriterOptions options;
    options.output_file_path = directory.filePath("streaming.txt");
    options.include_raw_values = raw;
    options.legacy_time_format = legacy;
    options.timestamp_precision = legacy ? 15 : 9;
    options.missing_value_text = QStringLiteral("缺失");
    ResultWriter streaming, buffered;
    streaming.setOptions(options);
    QString error, path;
    QVERIFY2(streaming.startStreamingTask(task, makeLayouts(task), &error), qPrintable(error));
    for (int offset = 0; offset < values.size(); offset += 37) {
        QVERIFY2(streaming.appendDecodedBlock(task.task_id, values.mid(offset, 37), &error), qPrintable(error));
    }
    QVERIFY2(streaming.finishTask(task.task_id, &path, &error), qPrintable(error));
    options.output_file_path = directory.filePath("buffered.txt");
    buffered.setOptions(options);
    QVERIFY2(buffered.writeTaskResult(task, values, nullptr, &error), qPrintable(error));
    const QByteArray expected = readOutputBytes(options.output_file_path);
    QVERIFY(expected.size() > 256 * 1024);
    QCOMPARE(readOutputBytes(path), expected);
}

void ResultWriterTests::taskOptionsRemainIndependent_data()
{
    QTest::addColumn<bool>("streaming");
    QTest::newRow("streaming") << true;
    QTest::newRow("buffered") << false;
}

void ResultWriterTests::taskOptionsRemainIndependent()
{
    QFETCH(bool, streaming);
    QTemporaryDir directory;
    ResultWriter writer;
    ResultWriterOptions options;
    options.timestamp_precision = 2;
    TaskDefinition a = makeTask("a", directory.path()), b = makeTask("b", directory.path());
    a.parameter_groups = b.parameter_groups = {makeGroup(1, {"P"})};
    QString error, a_path, b_path;
    writer.setOptions(options);
    QVERIFY(streaming ? writer.startStreamingTask(a, makeLayouts(a), &error) : writer.startTask(a, &error));
    options.include_raw_values = true;
    options.legacy_time_format = true;
    writer.setOptions(options);
    QVERIFY(streaming ? writer.startStreamingTask(b, makeLayouts(b), &error) : writer.startTask(b, &error));
    // 再次改变后续任务配置，已启动的两份任务都不能受影响。
    writer.setOptions(ResultWriterOptions());
    ParameterValue a_value = makeValue("a", 1, "P", "P", 3.123456, 42);
    ParameterValue b_value = makeValue("b", 1, "P", "P", 3.123456, 7);
    b_value.raw_value = QVariant::fromValue(std::numeric_limits<quint64>::max());
    QVERIFY(streaming ? writer.appendDecodedBlock("b", {b_value}, &error) : writer.appendValue(b_value, &error));
    QVERIFY(streaming ? writer.appendDecodedBlock("a", {a_value}, &error) : writer.appendValue(a_value, &error));
    QVERIFY2(writer.finishTask("b", &b_path, &error), qPrintable(error));
    QVERIFY2(writer.finishTask("a", &a_path, &error), qPrintable(error));
    QCOMPARE(readOutputLines(a_path), QStringList({"timestamp_seconds\tP", "3.12\t42"}));
    QCOMPARE(readOutputLines(b_path), QStringList({"TIME\tP_raw\tP", "00:00:03:123\t18446744073709551615\t7"}));
}

void ResultWriterTests::abortPreservesDestinationAndOtherTask()
{
    QTemporaryDir directory;
    const QString destination = directory.filePath("a_results.txt");
    const QByteArray old_content("previous result\r\n");
    QFile existing(destination);
    QVERIFY(existing.open(QIODevice::WriteOnly));
    QCOMPARE(existing.write(old_content), qint64(old_content.size()));
    existing.close();
    TaskDefinition a = makeTask("a", directory.path()), b = makeTask("b", directory.path());
    a.parameter_groups = b.parameter_groups = {makeGroup(1, {"P"})};
    ResultWriter writer;
    QSignalSpy finished(&writer, &ResultWriter::taskOutputFinished);
    QString error, b_path;
    QVERIFY(writer.startStreamingTask(a, makeLayouts(a), &error));
    QVERIFY(writer.startStreamingTask(b, makeLayouts(b), &error));
    QList<ParameterValue> block;
    for (int row = 0; row < 10000; ++row) {
        block.append(makeValue("a", 1, "P", "P", row, QString(64, QChar('x'))));
    }
    QVERIFY(writer.appendDecodedBlock("a", block, &error));
    QCOMPARE(readOutputBytes(destination), old_content); // 多次刷新也只写临时文件。
    QVERIFY(!writer.appendDecodedBlock("b", {block.first()}, &error));
    QVERIFY(error.contains("task 'a'"));
    QVERIFY(writer.appendDecodedBlock("b", {makeValue("b", 1, "P", "P", 0, 99)}, &error));
    QVERIFY(writer.abortTask("a"));
    QVERIFY(!writer.abortTask("a"));
    QVERIFY(!writer.hasTask("a"));
    QVERIFY(!writer.finishTask("a", nullptr, &error));
    QCOMPARE(readOutputBytes(destination), old_content);
    QCOMPARE(finished.count(), 0);
    QVERIFY(writer.finishTask("b", &b_path, &error));
    QCOMPARE(finished.count(), 1);
    QCOMPARE(readOutputLines(b_path), QStringList({"timestamp_seconds\tP", "0.000000000\t99"}));
    // 已完成任务不受后续任务取消或输出器析构影响。
    {
        ResultWriter unfinished;
        a.task_id = "unfinished";
        QVERIFY(unfinished.startStreamingTask(a, makeLayouts(a), &error));
        QVERIFY(unfinished.appendDecodedBlock("unfinished",
            {makeValue("unfinished", 1, "P", "P", 0, 1)}, &error));
    }
    QVERIFY(!QFileInfo::exists(directory.filePath("unfinished_results.txt")));
    QCOMPARE(QDir(directory.path()).entryList(QDir::Files | QDir::Hidden).size(), 2);
}

void ResultWriterTests::emptyAndFailedStreamingTasks()
{
    QTemporaryDir directory;
    ResultWriter writer;
    QSignalSpy finished(&writer, &ResultWriter::taskOutputFinished);
    QSignalSpy failed(&writer, &ResultWriter::taskOutputFailed);
    TaskDefinition task = makeTask("empty", directory.path());
    task.parameter_groups = {makeGroup(1, {"P"})};
    QString error, path;
    QVERIFY(writer.startStreamingTask(task, makeLayouts(task), &error));
    QVERIFY(writer.finishTask(task.task_id, &path, &error));
    QCOMPARE(readOutputLines(path), QStringList({"timestamp_seconds\tP"}));
    QCOMPARE(finished.count(), 1);
    QCOMPARE(finished.first().at(2).toULongLong(), quint64(0));
    task.task_id = "failure";
    QVERIFY(writer.startStreamingTask(task, makeLayouts(task), &error));
    QVERIFY(writer.appendDecodedBlock(task.task_id, {makeValue(task.task_id, 1, "P", "P", 0, 1)}, &error));
    // 在打开临时文件后制造不可替换的目标目录，跨平台触发 commit 失败。
    const QString blocked = writer.outputPathForTask(task.task_id);
    QVERIFY(QDir().mkdir(blocked));
    QVERIFY(!writer.finishTask(task.task_id, nullptr, &error));
    QVERIFY(!error.isEmpty());
    QCOMPARE(failed.count(), 1);
    QCOMPARE(finished.count(), 1);
    QVERIFY(!writer.hasTask(task.task_id));
    QVERIFY(QFileInfo(blocked).isDir());
    QVERIFY(!writer.startStreamingTask(task, makeLayouts(task), &error));
    QVERIFY(!writer.hasTask(task.task_id));
}

void ResultWriterTests::concurrentStreamingTasksStayIndependent()
{
    QTemporaryDir directory;
    ResultWriter writer;
    QList<TaskDefinition> tasks;
    QList<ResultWriterOptions> options;
    QList<QList<ParameterValue>> all_values;
    QList<QFuture<QString>> futures;
    for (int index = 0; index < 3; ++index) {
        TaskDefinition task = makeTask(QStringLiteral("thread-%1").arg(index), directory.path());
        task.parameter_groups = {makeGroup(1, {"P"})};
        ResultWriterOptions option;
        option.include_raw_values = index % 2 == 0;
        option.legacy_time_format = index % 2 == 1;
        writer.setOptions(option);
        QString error;
        QVERIFY2(writer.startStreamingTask(task, makeLayouts(task), &error), qPrintable(error));
        tasks.append(task);
        options.append(option);
        QList<ParameterValue> values;
        for (int row = 0; row < 2048; ++row) {
            ParameterValue value = makeValue(task.task_id, 1, "P", "P", double(row) / 64, row + index * 10000);
            value.raw_value = QVariant::fromValue(quint64(row) + 9007199254740993ULL);
            values.append(value);
        }
        all_values.append(values);
    }
    for (int index = 0; index < tasks.size(); ++index) {
        const TaskDefinition task = tasks.at(index);
        const QList<ParameterValue> values = all_values.at(index);
        futures.append(QtConcurrent::run([&writer, task, values, index]() -> QString {
            QString error;
            for (int offset = 0; offset < values.size(); offset += 17) {
                if (!writer.appendDecodedBlock(task.task_id, values.mid(offset, 17), &error)) {
                    return error.isEmpty() ? QStringLiteral("append failed") : error;
                }
                if (index == 0 && offset >= 1000) {
                    return writer.abortTask(task.task_id) ? QString() : QStringLiteral("abort failed");
                }
            }
            return QString();
        }));
    }
    QStringList errors;
    // 所有 worker 结束后再断言，失败分支也不会让任务持有已销毁的 writer。
    for (QFuture<QString> &future : futures) {
        errors.append(future.result());
    }
    for (const QString &error : errors) {
        QVERIFY2(error.isEmpty(), qPrintable(error));
    }
    QVERIFY(!writer.hasTask(tasks.first().task_id));
    QVERIFY(!QFileInfo::exists(directory.filePath("thread-0_results.txt")));
    for (int index = 1; index < tasks.size(); ++index) {
        QString path, error;
        QVERIFY2(writer.finishTask(tasks.at(index).task_id, &path, &error), qPrintable(error));
        ResultWriter reference;
        ResultWriterOptions option = options.at(index);
        option.output_file_path = directory.filePath(QStringLiteral("expected-%1.txt").arg(index));
        reference.setOptions(option);
        QVERIFY2(reference.writeTaskResult(tasks.at(index), all_values.at(index), nullptr, &error), qPrintable(error));
        QCOMPARE(readOutputBytes(path), readOutputBytes(option.output_file_path));
    }
}

QTEST_MAIN(ResultWriterTests)

#include "result_writer_tests.moc"
