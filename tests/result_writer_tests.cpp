#include "result_writer.h"

#include <QFile>
#include <QSignalSpy>
#include <QTemporaryDir>
#include <QtTest>

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

QTEST_MAIN(ResultWriterTests)

#include "result_writer_tests.moc"
