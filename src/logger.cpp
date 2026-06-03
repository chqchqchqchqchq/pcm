#include "logger.h"

#include <QDebug>
#include <QMutexLocker>

Logger &Logger::instance()
{
    static Logger logger;
    return logger;
}

Logger::Logger(QObject *parent)
    : QObject(parent)
{
    qRegisterMetaType<LogRecord>("LogRecord");
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
    }

    emit messageLogged(record);
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
