#include "logger.h"

#include <QDir>
#include <QFile>
#include <QTemporaryDir>
#include <QtTest>

class LoggerTests : public QObject
{
    Q_OBJECT

private slots:
    void writesUtf8FlushesErrorsAndRotates();
    void degradesSafelyWhenLogDirectoryCannotBeCreated();
};

namespace {

QString makeBlockingFile(const QString &directory)
{
    const QString path = QDir(directory).filePath(QStringLiteral("not-a-directory"));
    QFile file(path);
    if (file.open(QIODevice::WriteOnly)) {
        file.write("block");
    }
    return path;
}

} // namespace

void LoggerTests::writesUtf8FlushesErrorsAndRotates()
{
    QTemporaryDir temp_dir;
    QVERIFY(temp_dir.isValid());

    Logger &logger = Logger::instance();
    logger.configurePersistence(temp_dir.path(), 240, 3);
    for (int index = 0; index < 12; ++index) {
        logger.log(LogLevel::Info,
                   QStringLiteral("日志测试"),
                   QStringLiteral("任务-%1").arg(index),
                   QStringLiteral("温度"),
                   QStringLiteral("中文持久化消息 %1 abcdefghijklmnopqrstuvwxyz").arg(index));
    }
    logger.log(LogLevel::Error,
               QStringLiteral("日志测试"),
               QStringLiteral("flush-task"),
               QString(),
               QStringLiteral("立即刷新错误"));

    const QString active_path = logger.logFilePath();
    QVERIFY(!active_path.isEmpty());
    QFile active_file(active_path);
    QVERIFY(active_file.open(QIODevice::ReadOnly));
    const QString active_text = QString::fromUtf8(active_file.readAll());
    QVERIFY(active_text.contains(QStringLiteral("立即刷新错误")));

    const QFileInfoList files = QDir(temp_dir.path()).entryInfoList(
        {QStringLiteral("pcm_process_*.log")}, QDir::Files, QDir::Name);
    QVERIFY(files.size() >= 2);
    QVERIFY(files.size() <= 3);

    bool found_utf8_text = false;
    for (const QFileInfo &file_info : files) {
        QFile file(file_info.absoluteFilePath());
        QVERIFY(file.open(QIODevice::ReadOnly));
        if (QString::fromUtf8(file.readAll()).contains(QStringLiteral("中文持久化消息"))) {
            found_utf8_text = true;
        }
    }
    QVERIFY(found_utf8_text);

    logger.configurePersistence(makeBlockingFile(temp_dir.path()), 240, 3);
}

void LoggerTests::degradesSafelyWhenLogDirectoryCannotBeCreated()
{
    QTemporaryDir temp_dir;
    QVERIFY(temp_dir.isValid());

    Logger &logger = Logger::instance();
    logger.configurePersistence(makeBlockingFile(temp_dir.path()), 1024, 5);
    QCOMPARE(logger.logFilePath(), QString());
    logger.log(LogLevel::Fatal,
               QStringLiteral("日志测试"),
               QStringLiteral("write-failure"),
               QString(),
               QStringLiteral("该错误仍应通过 Qt 日志输出且不崩溃"));
    QCOMPARE(logger.logFilePath(), QString());
}

QTEST_MAIN(LoggerTests)

#include "logger_tests.moc"
