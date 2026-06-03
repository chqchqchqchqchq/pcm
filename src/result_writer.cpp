#include "result_writer.h"

#include <QDate>
#include <QDateTime>
#include <QDir>
#include <QFileInfo>
#include <QLocale>
#include <QMutexLocker>
#include <QSaveFile>
#include <QTextStream>
#include <QTime>
#include <QtGlobal>

#if QT_VERSION >= QT_VERSION_CHECK(6, 0, 0)
#include <QStringConverter>
#endif

#include <algorithm>

namespace {

constexpr QChar kColumnKeySeparator(0x1f);

/**
 * @brief Orders decoded values by timestamp while keeping equal timestamps stable for async duplicate resolution.
 */
bool resultValueLess(const ParameterValue &left, const ParameterValue &right)
{
    if (left.timestamp_seconds != right.timestamp_seconds) {
        return left.timestamp_seconds < right.timestamp_seconds;
    }

    if (left.stream_index != right.stream_index) {
        return left.stream_index < right.stream_index;
    }

    if (left.parameter_id != right.parameter_id) {
        return left.parameter_id < right.parameter_id;
    }

    return left.parameter_name < right.parameter_name;
}

} // namespace

ResultWriter::ResultWriter(QObject *parent)
    : QObject(parent)
{
    qRegisterMetaType<ResultWriterOptions>("ResultWriterOptions");
}

void ResultWriter::setOptions(const ResultWriterOptions &options)
{
    ResultWriterOptions normalized_options = options;
    normalized_options.timestamp_precision = qBound(0, normalized_options.timestamp_precision, 15);
    if (normalized_options.file_name_suffix.isEmpty()) {
        normalized_options.file_name_suffix = QStringLiteral("_results.txt");
    }

    QMutexLocker locker(&mutex_);
    options_ = normalized_options;
}

ResultWriterOptions ResultWriter::options() const
{
    QMutexLocker locker(&mutex_);
    return options_;
}

bool ResultWriter::startTask(const TaskDefinition &definition, QString *error_message)
{
    if (definition.task_id.trimmed().isEmpty()) {
        setError(error_message, QStringLiteral("TaskDefinition.task_id is empty"));
        return false;
    }

    PendingTask task;
    task.definition = definition;

    {
        QMutexLocker locker(&mutex_);
        if (pending_tasks_.contains(definition.task_id)) {
            setError(error_message,
                     QStringLiteral("Result output for task '%1' has already started").arg(definition.task_id));
            return false;
        }

        task.output_path = makeOutputPath(definition, options_);
        pending_tasks_.insert(definition.task_id, task);
    }

    log(LogLevel::Info,
        definition.task_id,
        QString(),
        QStringLiteral("Result output started at '%1'").arg(task.output_path));
    emit taskOutputStarted(definition.task_id, task.output_path);
    return true;
}

bool ResultWriter::appendValue(const ParameterValue &value, QString *error_message)
{
    if (value.task_id.trimmed().isEmpty()) {
        setError(error_message, QStringLiteral("ParameterValue.task_id is empty"));
        return false;
    }

    if (makeColumnKey(value).isEmpty()) {
        setError(error_message,
                 QStringLiteral("ParameterValue for task '%1' has neither parameter_id nor parameter_name")
                     .arg(value.task_id));
        return false;
    }

    QMutexLocker locker(&mutex_);
    auto iterator = pending_tasks_.find(value.task_id);
    if (iterator == pending_tasks_.end()) {
        setError(error_message,
                 QStringLiteral("Result output for task '%1' has not been started").arg(value.task_id));
        return false;
    }

    iterator.value().values.append(value);
    return true;
}

bool ResultWriter::appendValues(const QList<ParameterValue> &values, QString *error_message)
{
    for (const ParameterValue &value : values) {
        if (value.task_id.trimmed().isEmpty()) {
            setError(error_message, QStringLiteral("ParameterValue.task_id is empty"));
            return false;
        }

        if (makeColumnKey(value).isEmpty()) {
            setError(error_message,
                     QStringLiteral("ParameterValue for task '%1' has neither parameter_id nor parameter_name")
                         .arg(value.task_id));
            return false;
        }
    }

    QMutexLocker locker(&mutex_);
    for (const ParameterValue &value : values) {
        if (!pending_tasks_.contains(value.task_id)) {
            setError(error_message,
                     QStringLiteral("Result output for task '%1' has not been started").arg(value.task_id));
            return false;
        }
    }

    for (const ParameterValue &value : values) {
        pending_tasks_[value.task_id].values.append(value);
    }

    return true;
}

bool ResultWriter::writeTaskResult(const TaskDefinition &definition,
                                   const QList<ParameterValue> &values,
                                   QString *output_path,
                                   QString *error_message)
{
    if (!startTask(definition, error_message)) {
        return false;
    }

    if (!appendValues(values, error_message)) {
        abortTask(definition.task_id);
        return false;
    }

    return finishTask(definition.task_id, output_path, error_message);
}

bool ResultWriter::finishTask(const QString &task_id, QString *output_path, QString *error_message)
{
    PendingTask task;
    ResultWriterOptions writer_options;
    {
        QMutexLocker locker(&mutex_);
        auto iterator = pending_tasks_.find(task_id);
        if (iterator == pending_tasks_.end()) {
            setError(error_message,
                     QStringLiteral("Result output for task '%1' has not been started").arg(task_id));
            return false;
        }

        task = iterator.value();
        writer_options = options_;
        pending_tasks_.erase(iterator);
    }

    quint64 row_count = 0;
    QString local_error;
    if (!writeMergedFile(task, writer_options, &row_count, &local_error)) {
        setError(error_message, local_error);
        log(LogLevel::Error, task_id, QString(), local_error);
        emit taskOutputFailed(task_id, local_error);
        return false;
    }

    if (output_path != nullptr) {
        *output_path = task.output_path;
    }

    log(LogLevel::Info,
        task_id,
        QString(),
        QStringLiteral("Result output finished, rows=%1, file='%2'").arg(row_count).arg(task.output_path));
    emit taskOutputFinished(task_id, task.output_path, row_count);
    return true;
}

bool ResultWriter::abortTask(const QString &task_id)
{
    QMutexLocker locker(&mutex_);
    return pending_tasks_.remove(task_id) > 0;
}

bool ResultWriter::hasTask(const QString &task_id) const
{
    QMutexLocker locker(&mutex_);
    return pending_tasks_.contains(task_id);
}

QString ResultWriter::outputPathForTask(const QString &task_id) const
{
    QMutexLocker locker(&mutex_);
    const auto iterator = pending_tasks_.constFind(task_id);
    if (iterator == pending_tasks_.constEnd()) {
        return QString();
    }

    return iterator.value().output_path;
}

QList<ResultWriter::ColumnDefinition> ResultWriter::buildColumns(const TaskDefinition &definition,
                                                                 const QList<ParameterValue> &sorted_values) const
{
    QList<ColumnDefinition> columns;
    QHash<QString, int> column_index_by_key;

    auto append_or_update_column = [&](int stream_index,
                                       const QString &parameter_id,
                                       const QString &parameter_name,
                                       const QString &unit) {
        const QString key = makeColumnKey(stream_index, parameter_id, parameter_name);
        if (key.isEmpty()) {
            return;
        }

        const auto iterator = column_index_by_key.constFind(key);
        if (iterator != column_index_by_key.constEnd()) {
            ColumnDefinition &column = columns[iterator.value()];
            if (column.parameter_id.isEmpty()) {
                column.parameter_id = parameter_id.trimmed();
            }
            if (column.parameter_name.isEmpty()) {
                column.parameter_name = parameter_name.trimmed();
            }
            if (column.unit.isEmpty()) {
                column.unit = unit.trimmed();
            }
            return;
        }

        ColumnDefinition column;
        column.stream_index = stream_index;
        column.parameter_id = parameter_id.trimmed();
        column.parameter_name = parameter_name.trimmed();
        column.unit = unit.trimmed();
        column.key = key;
        column_index_by_key.insert(key, columns.size());
        columns.append(column);
    };

    for (const ParameterGroupSelection &group : definition.parameter_groups) {
        for (const QString &parameter_id : group.parameter_ids) {
            append_or_update_column(group.stream_index, parameter_id, QString(), QString());
        }
    }

    for (const ParameterValue &value : sorted_values) {
        append_or_update_column(value.stream_index, value.parameter_id, value.parameter_name, value.unit);
    }

    return columns;
}

bool ResultWriter::writeMergedFile(const PendingTask &task,
                                   const ResultWriterOptions &options,
                                   quint64 *row_count,
                                   QString *error_message) const
{
    QList<ParameterValue> sorted_values = task.values;
    std::stable_sort(sorted_values.begin(), sorted_values.end(), resultValueLess);

    const QList<ColumnDefinition> columns = buildColumns(task.definition, sorted_values);
    QHash<QString, int> column_index_by_key;
    for (int index = 0; index < columns.size(); ++index) {
        column_index_by_key.insert(columns.at(index).key, index);
    }

    QFileInfo output_info(task.output_path);
    QDir output_dir(output_info.absolutePath());
    if (!output_dir.exists() && !output_dir.mkpath(QStringLiteral("."))) {
        setError(error_message,
                 QStringLiteral("Failed to create result output directory '%1'").arg(output_dir.absolutePath()));
        return false;
    }

    QSaveFile file(task.output_path);
    if (!file.open(QIODevice::WriteOnly | QIODevice::Text)) {
        setError(error_message,
                 QStringLiteral("Failed to open result file '%1': %2").arg(task.output_path, file.errorString()));
        return false;
    }

    QTextStream stream(&file);
#if QT_VERSION >= QT_VERSION_CHECK(6, 0, 0)
    stream.setEncoding(QStringConverter::Utf8);
#else
    stream.setCodec("UTF-8");
#endif

    stream << QStringLiteral("timestamp_seconds");
    for (const ColumnDefinition &column : columns) {
        stream << '\t' << makeColumnTitle(column);
    }
    stream << '\n';

    QString current_timestamp_key;
    QString current_timestamp_text;
    QStringList row_values;
    quint64 emitted_rows = 0;

    auto reset_row = [&]() {
        row_values.clear();
        row_values.reserve(columns.size());
        for (int index = 0; index < columns.size(); ++index) {
            row_values.append(options.missing_value_text);
        }
    };

    auto flush_row = [&]() {
        if (current_timestamp_key.isEmpty()) {
            return;
        }

        stream << current_timestamp_text;
        for (const QString &cell : row_values) {
            stream << '\t' << cell;
        }
        stream << '\n';
        ++emitted_rows;
    };

    reset_row();
    for (const ParameterValue &value : sorted_values) {
        const QString timestamp_text = formatTimestamp(value.timestamp_seconds, options);
        if (current_timestamp_key.isEmpty()) {
            current_timestamp_key = timestamp_text;
            current_timestamp_text = timestamp_text;
        } else if (timestamp_text != current_timestamp_key) {
            flush_row();
            reset_row();
            current_timestamp_key = timestamp_text;
            current_timestamp_text = timestamp_text;
        }

        const int column_index = column_index_by_key.value(makeColumnKey(value), -1);
        if (column_index < 0 || column_index >= row_values.size()) {
            log(LogLevel::Warning,
                value.task_id,
                value.parameter_name,
                QStringLiteral("Skipping result value without an output column"));
            continue;
        }

        row_values[column_index] = variantToText(value.value);
    }

    flush_row();
    stream.flush();

    if (stream.status() != QTextStream::Ok) {
        setError(error_message,
                 QStringLiteral("Failed while writing result file '%1'").arg(task.output_path));
        return false;
    }

    if (!file.commit()) {
        setError(error_message,
                 QStringLiteral("Failed to commit result file '%1': %2").arg(task.output_path, file.errorString()));
        return false;
    }

    if (row_count != nullptr) {
        *row_count = emitted_rows;
    }
    return true;
}

QString ResultWriter::formatTimestamp(double timestamp_seconds, const ResultWriterOptions &options)
{
    return QString::number(timestamp_seconds, 'f', options.timestamp_precision);
}

void ResultWriter::log(LogLevel level,
                       const QString &task_id,
                       const QString &parameter_name,
                       const QString &message) const
{
    Logger::instance().log(level, QStringLiteral("ResultWriter"), task_id, parameter_name, message);
}

QString ResultWriter::makeColumnKey(int stream_index, const QString &parameter_id, const QString &parameter_name)
{
    const QString normalized_id = parameter_id.trimmed();
    const QString normalized_name = parameter_name.trimmed();
    const QString parameter_key = normalized_id.isEmpty() ? normalized_name : normalized_id;
    if (parameter_key.isEmpty()) {
        return QString();
    }

    return QString::number(stream_index) + kColumnKeySeparator + parameter_key;
}

QString ResultWriter::makeColumnKey(const ParameterValue &value)
{
    return makeColumnKey(value.stream_index, value.parameter_id, value.parameter_name);
}

QString ResultWriter::makeColumnTitle(const ColumnDefinition &column)
{
    const QString parameter_label = column.parameter_id.isEmpty() ? column.parameter_name : column.parameter_id;
    QString title = QStringLiteral("stream_%1.%2").arg(column.stream_index).arg(parameter_label);

    if (!column.parameter_name.isEmpty() && column.parameter_name != parameter_label) {
        title += QStringLiteral(":") + column.parameter_name;
    }
    if (!column.unit.isEmpty()) {
        title += QStringLiteral("[") + column.unit + QStringLiteral("]");
    }

    return sanitizeCell(title);
}

QString ResultWriter::makeOutputPath(const TaskDefinition &definition, const ResultWriterOptions &options)
{
    if (!options.output_file_path.trimmed().isEmpty()) {
        return QFileInfo(options.output_file_path).absoluteFilePath();
    }

    const QString output_directory = definition.output_directory.trimmed().isEmpty()
        ? QDir::currentPath()
        : definition.output_directory;
    const QString output_name = safeFileName(definition.task_id) + options.file_name_suffix;
    return QDir(output_directory).absoluteFilePath(output_name);
}

QString ResultWriter::safeFileName(const QString &file_name)
{
    QString result = file_name.trimmed();
    static const QString invalid_chars = QStringLiteral("<>:\"/\\|?*");

    for (int index = 0; index < result.size(); ++index) {
        const QChar ch = result.at(index);
        if (invalid_chars.contains(ch) || ch.unicode() < 0x20) {
            result[index] = QChar('_');
        }
    }

    if (result.isEmpty()) {
        return QStringLiteral("task");
    }

    return result;
}

QString ResultWriter::sanitizeCell(QString text)
{
    text.replace('\t', ' ');
    text.replace('\r', ' ');
    text.replace('\n', ' ');
    return text;
}

QString ResultWriter::variantToText(const QVariant &value)
{
    if (!value.isValid() || value.isNull()) {
        return QString();
    }

    const QLocale c_locale = QLocale::c();
    const int type = value.userType();
    QString text;

    if (type == QMetaType::Float || type == QMetaType::Double) {
        text = c_locale.toString(value.toDouble(), 'g', 17);
    } else if (type == QMetaType::Int || type == QMetaType::UInt ||
               type == QMetaType::LongLong || type == QMetaType::ULongLong) {
        text = value.toString();
    } else if (type == QMetaType::Bool) {
        text = value.toBool() ? QStringLiteral("true") : QStringLiteral("false");
    } else if (type == QMetaType::QDateTime) {
        text = value.toDateTime().toString(Qt::ISODateWithMs);
    } else if (type == QMetaType::QDate) {
        text = value.toDate().toString(Qt::ISODate);
    } else if (type == QMetaType::QTime) {
        text = value.toTime().toString(Qt::ISODateWithMs);
    } else if (type == QMetaType::QByteArray) {
        text = QString::fromLatin1(value.toByteArray().toHex());
    } else {
        text = value.toString();
    }

    return sanitizeCell(text);
}

void ResultWriter::setError(QString *error_message, const QString &message)
{
    if (error_message != nullptr) {
        *error_message = message;
    }
}
