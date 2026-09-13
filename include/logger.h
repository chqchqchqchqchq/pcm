#pragma once

#include <QDateTime>
#include <QFile>
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
     * @brief Reopens persistence in a directory, primarily for controlled deployments and tests.
     */
    void configurePersistence(const QString &log_directory,
                              qint64 max_file_size_bytes = 10 * 1024 * 1024,
                              int retained_file_count = 5);

    /**
     * @brief Returns the currently active UTF-8 log file path, or an empty string when unavailable.
     */
    QString logFilePath() const;

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
    ~Logger() override;

    bool openLogFile();
    void rotateIfNeeded(qint64 incoming_bytes);
    void pruneOldLogFiles();
    void writePersistentLine(const QByteArray &line, bool flush_immediately);

    mutable QMutex mutex_;
    QFile log_file_;
    QString log_directory_;
    QString run_file_stem_;
    QString current_log_path_;
    qint64 max_file_size_bytes_ = 10 * 1024 * 1024;
    int retained_file_count_ = 5;
    int rotation_index_ = 0;
    bool persistence_error_reported_ = false;
};
