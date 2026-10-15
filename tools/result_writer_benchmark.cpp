#include "result_writer.h"

#include <QCommandLineParser>
#include <QCoreApplication>
#include <QCryptographicHash>
#include <QElapsedTimer>
#include <QFile>
#include <QFileInfo>
#include <QJsonDocument>
#include <QJsonObject>
#include <QTextStream>

/** @brief 无原始文件依赖的宽表基准；数据生成和 SHA-256 均不计入输出器耗时。 */
int main(int argc, char **argv)
{
    QCoreApplication app(argc, argv);
    QCommandLineParser parser;
    parser.addHelpOption();
    parser.addOptions({
        {"output", "新建结果路径（禁止覆盖）", "path"},
        {"rows", "数据行数", "count", "81920"},
        {"parameters", "参数列数，不包含时间列", "count", "1004"},
        {"block-rows", "每批生成行数", "count", "64"},
        {"profile", "integer / double / mixed", "name", "mixed"},
        {"raw", "同时输出原始码值"},
        {"abort", "写完数据后取消，测量临时文件清理时间，不提交结果"}
    });
    parser.process(app);
    bool rows_ok = false, columns_ok = false, block_ok = false;
    const int rows = parser.value("rows").toInt(&rows_ok);
    const int columns = parser.value("parameters").toInt(&columns_ok);
    const int block_rows = parser.value("block-rows").toInt(&block_ok);
    const QString profile = parser.value("profile");
    const QString output = parser.value("output");
    if (!rows_ok || !columns_ok || !block_ok || rows < 1 || columns < 1 || block_rows < 1
        || columns > 100000 || block_rows > 1000000 / columns || output.isEmpty()
        || QFileInfo::exists(output) || !QStringList{"integer", "double", "mixed"}.contains(profile)) {
        QTextStream(stderr) << "Invalid arguments or output already exists\n";
        return 2;
    }

    TaskDefinition task;
    task.task_id = QStringLiteral("writer-benchmark");
    StreamLayoutTable layout;
    layout.stream_index = 1;
    ParameterGroupSelection group;
    group.stream_index = 1;
    QList<ParameterValue> prototypes;
    for (int column = 0; column < columns; ++column) {
        StreamLayoutEntry entry;
        entry.parameter_id = QStringLiteral("P%1").arg(column, 4, 10, QLatin1Char('0'));
        entry.parameter_name = entry.parameter_id;
        layout.entries.append(entry);
        group.parameter_ids.append(entry.parameter_id);
        ParameterValue value;
        value.task_id = task.task_id;
        value.stream_index = 1;
        value.parameter_id = entry.parameter_id;
        value.parameter_name = entry.parameter_name;
        prototypes.append(value);
    }
    layout.rebuildIndexes();
    task.parameter_groups.append(group);
    ResultWriter writer;
    ResultWriterOptions options;
    options.output_file_path = output;
    options.legacy_time_format = true;
    options.include_raw_values = parser.isSet("raw");
    writer.setOptions(options);
    QString error;
    QElapsedTimer clock;
    clock.start();
    if (!writer.startStreamingTask(task, {layout}, &error)) {
        QTextStream(stderr) << error << '\n';
        return 1;
    }
    const qint64 start_ns = clock.nsecsElapsed();
    qint64 append_ns = 0, generation_ns = 0;
    QList<ParameterValue> values;
    values.reserve(block_rows * columns);
    for (int first = 0; first < rows; first += block_rows) {
        clock.restart();
        values.clear();
        for (int row = first; row < qMin(rows, first + block_rows); ++row) {
            for (int column = 0; column < columns; ++column) {
                ParameterValue value = prototypes.at(column);
                value.timestamp_seconds = row;
                // 固定公式，每行变化；混合负数、64 位大整数和非整浮点，避免重复值缓存偏置。
                const quint64 raw = quint64(row) * 1009 + quint64(column) * 7919;
                value.raw_value = QVariant::fromValue(raw);
                if (profile == "double" || (profile == "mixed" && column % 8 == 0)) {
                    value.value = double(raw) * 0.0012345678901234567 - 12345.6789;
                } else if (column % 8 == 1) {
                    value.value = QVariant::fromValue(-qlonglong(raw));
                } else {
                    value.value = QVariant::fromValue(raw + (column % 8 == 2 ? 9007199254740993ULL : 0));
                }
                values.append(value);
            }
        }
        generation_ns += clock.nsecsElapsed();
        clock.restart();
        if (!writer.appendDecodedBlock(task.task_id, values, &error)) {
            QTextStream(stderr) << error << '\n';
            writer.abortTask(task.task_id);
            return 1;
        }
        append_ns += clock.nsecsElapsed();
    }
    clock.restart();
    const bool aborted = parser.isSet("abort");
    const bool success = aborted ? writer.abortTask(task.task_id) : writer.finishTask(task.task_id, nullptr, &error);
    const qint64 finish_ns = clock.nsecsElapsed();
    if (!success || (aborted && QFileInfo::exists(output))) {
        QTextStream(stderr) << "Finish/abort failed: " << error << '\n';
        return 1;
    }
    QJsonObject result{
        {"rows", rows}, {"columns", columns * (options.include_raw_values ? 2 : 1) + 1},
        {"block_rows", block_rows}, {"profile", profile}, {"raw", options.include_raw_values},
        {"qt_version", QT_VERSION_STR}, {"start_ms", start_ns / 1e6}, {"append_ms", append_ns / 1e6},
        {"finish_or_abort_ms", finish_ns / 1e6}, {"aborted", aborted},
        {"writer_ms", (start_ns + append_ns + finish_ns) / 1e6}, {"generation_ms", generation_ns / 1e6}
    };
    if (!aborted) {
        QFile file(output);
        QCryptographicHash hash(QCryptographicHash::Sha256);
        if (!file.open(QIODevice::ReadOnly) || !hash.addData(&file)) {
            QTextStream(stderr) << "Hash failed\n";
            return 1;
        }
        result.insert("bytes", double(file.size()));
        result.insert("sha256", QString::fromLatin1(hash.result().toHex()));
    }
    QTextStream(stdout) << QJsonDocument(result).toJson(QJsonDocument::Compact) << '\n';
    return 0;
}
