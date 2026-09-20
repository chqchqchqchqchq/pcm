#include "raw_time_range_scanner.h"

#include <QCommandLineParser>
#include <QCoreApplication>
#include <QElapsedTimer>
#include <QJsonDocument>
#include <QJsonObject>
#include <QTextStream>

/** @brief 使用生产扫描器读取文件范围，输出可审阅的时间、I/O 和耗时报告。 */
int main(int argc, char **argv)
{
    QCoreApplication application(argc, argv);
    QCommandLineParser parser;
    parser.addHelpOption();
    parser.addOption(QCommandLineOption("config", QStringLiteral("配置文件"), "path"));
    parser.addOption(QCommandLineOption("raw", QStringLiteral("原始数据文件"), "path"));
    parser.process(application);
    if (!parser.isSet("config") || !parser.isSet("raw")) {
        parser.showHelp(2);
    }
    QElapsedTimer timer;
    timer.start();
    const auto result = RawTimeRangeScanner::scan(parser.value("config"), parser.value("raw"));
    QJsonObject json;
    json.insert("success", result.success);
    json.insert("error", result.error);
    json.insert("start_seconds", result.start_seconds);
    json.insert("end_seconds", result.end_seconds);
    json.insert("original_start_seconds", result.original_start_seconds);
    json.insert("skipped_startup", result.skipped_startup);
    json.insert("skipped_seconds", result.skipped_seconds);
    json.insert("start_byte_offset", double(result.start_byte_offset));
    json.insert("file_bytes", double(result.file_bytes));
    json.insert("bytes_read", double(result.bytes_read));
    json.insert("probed_frames", result.probed_frames);
    json.insert("elapsed_ms", double(timer.elapsed()));
    QTextStream output(stdout);
#if QT_VERSION < QT_VERSION_CHECK(6, 0, 0)
    output.setCodec("UTF-8");
#endif
    output << QJsonDocument(json).toJson(QJsonDocument::Indented);
    return result.success ? 0 : 1;
}
