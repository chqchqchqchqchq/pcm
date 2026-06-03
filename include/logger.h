#pragma once

#include <QDateTime>
#include <QMutex>
#include <QObject>
#include <QString>

/**
 * @brief Runtime log severity used by all modules.
 */
enum class LogLevel
{
    Info,
    Warning,
    Error,
    Fatal
};

/**
 * @brief Structured log record with task and parameter context.
 */
struct LogRecord
{
    QDateTime timestamp;
    LogLevel level = LogLevel::Info;
    QString module;
    QString task_id;
    QString parameter_name;
    QString message;
};

Q_DECLARE_METATYPE(LogRecord)

/**
 * @brief Minimal thread-safe logger facade.
 *
 * Logger emits structured records for GUI/persistent sinks and also forwards text to Qt's message handler. Modules can
 * use it without owning a sink; a full log persistence component can later connect to messageLogged().
 */
class Logger : public QObject
{
    Q_OBJECT

public:
    /**
     * @brief Returns the process-wide logger facade.
     */
    static Logger &instance();

    /**
     * @brief Emits one structured log record.
     */
    void log(LogLevel level,
             const QString &module,
             const QString &task_id,
             const QString &parameter_name,
             const QString &message);

    /**
     * @brief Converts a log level to display text.
     */
    static QString levelToString(LogLevel level);

signals:
    /**
     * @brief Raised for GUI display or persistent log sinks.
     */
    void messageLogged(const LogRecord &record);

private:
    explicit Logger(QObject *parent = nullptr);

    QMutex mutex_;
};
