#include "logger.h"

#include <QDebug>
#include <QDir>
#include <QFileInfoList>
#include <QMutexLocker>
#include <QStandardPaths>

Logger &Logger::instance()
{
    static Logger logger;
    return logger;
}

Logger::Logger(QObject *parent)
    : QObject(parent)
{
    qRegisterMetaType<LogRecord>("LogRecord");
    log_directory_ = QDir(QStandardPaths::writableLocation(QStandardPaths::AppLocalDataLocation))
                         .filePath(QStringLiteral("logs"));
    if (log_directory_.isEmpty()) {
        log_directory_ = QDir::current().filePath(QStringLiteral("logs"));
    }
    run_file_stem_ = QStringLiteral("pcm_process_%1")
                         .arg(QDateTime::currentDateTime().toString(QStringLiteral("yyyyMMdd_HHmmss_zzz")));
    openLogFile();
}

Logger::~Logger()
{
    QMutexLocker locker(&mutex_);
    if (log_file_.isOpen()) {
        log_file_.flush();
        log_file_.close();
    }
}

void Logger::log(LogLevel level,
                 const QString &module,
                 const QString &task_id,
                 const QString &parameter_name,
                 const QString &message)
{
    LogRecord record;
    record.timestamp = QDateTime::currentDateTime();
    record.level = level;
    record.module = module;
    record.task_id = task_id;
    record.parameter_name = parameter_name;
    record.message = message;

    const QString text = QStringLiteral("[%1] [%2] [%3] [task=%4] [param=%5] %6")
                             .arg(record.timestamp.toString(Qt::ISODateWithMs),
                                  levelToString(level),
                                  module,
                                  task_id.isEmpty() ? QStringLiteral("-") : task_id,
                                  parameter_name.isEmpty() ? QStringLiteral("-") : parameter_name,
                                  message);

    {
        QMutexLocker locker(&mutex_);
        switch (level) {
        case LogLevel::Info:
            qInfo().noquote() << text;
            break;
        case LogLevel::Warning:
            qWarning().noquote() << text;
            break;
        case LogLevel::Error:
        case LogLevel::Fatal:
            qCritical().noquote() << text;
            break;
        }

        QByteArray encoded_line = text.toUtf8();
        encoded_line.append('\n');
        writePersistentLine(encoded_line, level == LogLevel::Error || level == LogLevel::Fatal);
    }

    emit messageLogged(record);
}

void Logger::configurePersistence(const QString &log_directory,
                                  qint64 max_file_size_bytes,
                                  int retained_file_count)
{
    QMutexLocker locker(&mutex_);
    if (log_file_.isOpen()) {
        log_file_.flush();
        log_file_.close();
    }

    log_directory_ = QDir::cleanPath(log_directory);
    max_file_size_bytes_ = qMax<qint64>(1, max_file_size_bytes);
    retained_file_count_ = qMax(1, retained_file_count);
    rotation_index_ = 0;
    persistence_error_reported_ = false;
    run_file_stem_ = QStringLiteral("pcm_process_%1")
                         .arg(QDateTime::currentDateTime().toString(QStringLiteral("yyyyMMdd_HHmmss_zzz")));
    openLogFile();
}

QString Logger::logFilePath() const
{
    QMutexLocker locker(&mutex_);
    return log_file_.isOpen() ? current_log_path_ : QString();
}

bool Logger::openLogFile()
{
    if (log_directory_.isEmpty() || !QDir().mkpath(log_directory_)) {
        current_log_path_.clear();
        return false;
    }

    const QString part_suffix = rotation_index_ == 0
        ? QString()
        : QStringLiteral("_part%1").arg(rotation_index_, 3, 10, QLatin1Char('0'));
    current_log_path_ = QDir(log_directory_)
                            .filePath(QStringLiteral("%1%2.log").arg(run_file_stem_, part_suffix));
    log_file_.setFileName(current_log_path_);
    if (!log_file_.open(QIODevice::WriteOnly | QIODevice::Append)) {
        current_log_path_.clear();
        return false;
    }

    pruneOldLogFiles();
    return true;
}

void Logger::rotateIfNeeded(qint64 incoming_bytes)
{
    if (!log_file_.isOpen() || log_file_.size() + incoming_bytes <= max_file_size_bytes_) {
        return;
    }

    log_file_.flush();
    log_file_.close();
    ++rotation_index_;
    openLogFile();
}

void Logger::pruneOldLogFiles()
{
    const QFileInfoList log_files = QDir(log_directory_)
                                         .entryInfoList({QStringLiteral("pcm_process_*.log")},
                                                        QDir::Files,
                                                        QDir::Time);
    for (int index = retained_file_count_; index < log_files.size(); ++index) {
        const QString path = log_files.at(index).absoluteFilePath();
        if (path != current_log_path_) {
            QFile::remove(path);
        }
    }
}

void Logger::writePersistentLine(const QByteArray &line, bool flush_immediately)
{
    if (!log_file_.isOpen() && !openLogFile()) {
        if (!persistence_error_reported_) {
            persistence_error_reported_ = true;
            qWarning().noquote() << QStringLiteral("无法创建持久化日志文件：%1").arg(log_directory_);
        }
        return;
    }

    rotateIfNeeded(line.size());
    if (!log_file_.isOpen() || log_file_.write(line) != line.size()) {
        if (!persistence_error_reported_) {
            persistence_error_reported_ = true;
            qWarning().noquote() << QStringLiteral("持久化日志写入失败：%1").arg(current_log_path_);
        }
        return;
    }

    if (flush_immediately) {
        log_file_.flush();
    }
}

QString Logger::levelToString(LogLevel level)
{
    switch (level) {
    case LogLevel::Info:
        return QStringLiteral("INFO");
    case LogLevel::Warning:
        return QStringLiteral("WARNING");
    case LogLevel::Error:
        return QStringLiteral("ERROR");
    case LogLevel::Fatal:
        return QStringLiteral("FATAL");
    }

    return QStringLiteral("UNKNOWN");
}
