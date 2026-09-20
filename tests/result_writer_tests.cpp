#include "result_writer.h"

#include <QFile>
#include <QSignalSpy>
#include <QTemporaryDir>
#include <QtTest>
#include <limits>

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

QTEST_MAIN(ResultWriterTests)

#include "result_writer_tests.moc"
