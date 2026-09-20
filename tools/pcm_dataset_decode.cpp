#include "traditional_processing_controller.h"

#include <QCommandLineParser>
#include <QCoreApplication>
#include <QEventLoop>
#include <QElapsedTimer>
#include <QFile>
#include <QFileInfo>
#include <QRegularExpression>
#include <QTextStream>
#include <QTimer>
#include <cmath>

/** @brief 测试数据命令行入口；与 GUI 共用控制器、文件读取、解算及结果提交路径。 */
int main(int argc, char *argv[])
{
    QCoreApplication application(argc, argv);
    QCommandLineParser parser;
    parser.setApplicationDescription(QStringLiteral("使用实际控制器解算生成的 PCM 测试数据"));
    parser.addHelpOption();
    for (const QString &name : {"config", "raw", "parameters", "output", "sample-rate", "start", "end"}) {
        parser.addOption(QCommandLineOption(name, name, QStringLiteral("value")));
    }
    parser.addOption(QCommandLineOption(QStringLiteral("engineering-only"),
        QStringLiteral("仅输出工程值，与关闭原码输出的 GUI 设置一致")));
    parser.addOption(QCommandLineOption(QStringLiteral("timeout-seconds"),
        QStringLiteral("解算超时秒数"), QStringLiteral("seconds"), QStringLiteral("90")));
    parser.process(application);
    QTextStream error_stream(stderr);
#if QT_VERSION < QT_VERSION_CHECK(6, 0, 0)
    error_stream.setCodec("UTF-8");
#endif
    for (const QString &name : {"config", "raw", "parameters", "output", "sample-rate"}) {
        if (!parser.isSet(name)) {
            error_stream << "Missing --" << name << '\n';
            return 2;
        }
    }
    TraditionalProcessingOptions options;
    options.task_definition.task_id = QStringLiteral("generated-dataset");
    options.task_definition.config_file_path = QFileInfo(parser.value("config")).absoluteFilePath();
    options.task_definition.raw_file_path = QFileInfo(parser.value("raw")).absoluteFilePath();
    options.output_file_path = QFileInfo(parser.value("output")).absoluteFilePath();
    options.task_definition.output_directory = QFileInfo(options.output_file_path).absolutePath();
    options.include_raw_values = !parser.isSet(QStringLiteral("engineering-only"));
    bool valid_rate = false, valid_start = true, valid_end = true;
    options.task_definition.user_sample_rate_hz = parser.value("sample-rate").toDouble(&valid_rate);
    if (parser.isSet("start")) {
        options.task_definition.time_range.start_seconds = parser.value("start").toDouble(&valid_start);
    }
    if (parser.isSet("end")) {
        options.task_definition.time_range.end_seconds = parser.value("end").toDouble(&valid_end);
    }
    if (!valid_rate || !valid_start || !valid_end
        || !std::isfinite(options.task_definition.time_range.start_seconds)
        || !std::isfinite(options.task_definition.time_range.end_seconds)) {
        error_stream << "Invalid numeric argument\n";
        return 2;
    }
    options.time_ranges.append(options.task_definition.time_range);
    QFile groups(parser.value("parameters"));
    if (!groups.open(QIODevice::ReadOnly)) {
        error_stream << groups.errorString() << '\n';
        return 2;
    }
    const QByteArray bytes = groups.readAll();
    if (groups.error() != QFileDevice::NoError) {
        error_stream << groups.errorString() << '\n';
        return 2;
    }
    options.parameter_names = QString::fromUtf8(bytes).remove(QChar(0xfeff))
        .split(QRegularExpression(QStringLiteral("[\\s,;]+")), Qt::SkipEmptyParts);
    bool valid_timeout = false;
    const int timeout_seconds = parser.value(QStringLiteral("timeout-seconds")).toInt(&valid_timeout);
    if (!valid_timeout || timeout_seconds <= 0 || timeout_seconds > 86400) {
        error_stream << "Invalid timeout seconds\n";
        return 2;
    }
    QElapsedTimer elapsed;
    elapsed.start();
    TraditionalProcessingController controller;
    QObject::connect(&Logger::instance(), &Logger::messageLogged, &application, [&](const LogRecord &record) {
        error_stream << record.message << '\n';
    });
    QEventLoop event_loop;
    QTimer timeout;
    timeout.setSingleShot(true);
    bool completed = false;
    int exit_code = 1;
    QObject::connect(&controller, &TraditionalProcessingController::processingFinished, &event_loop,
                     [&](const QString &, const QStringList &) {
        completed = true;
        exit_code = 0;
        event_loop.quit();
    });
    QObject::connect(&controller, &TraditionalProcessingController::taskFailed, &event_loop,
                     [&](const QString &, const QString &reason) {
        error_stream << reason << '\n';
        completed = true;
        event_loop.quit();
    });
    QObject::connect(&controller, &TraditionalProcessingController::taskStopped, &event_loop,
                     [&](const QString &) {
        error_stream << "Stopped\n";
        completed = true;
        event_loop.quit();
    });
    QObject::connect(&timeout, &QTimer::timeout, &event_loop, [&]() {
        error_stream << "Timed out after " << timeout_seconds << " seconds\n";
        controller.requestStop();
        event_loop.quit();
    });
    timeout.start(timeout_seconds * 1000);
    controller.startProcessing(options);
    if (!completed) {
        event_loop.exec();
    }
    error_stream << "Elapsed ms: " << elapsed.elapsed() << '\n';
    return exit_code;
}
