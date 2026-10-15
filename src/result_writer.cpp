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
#include <cmath>
#include <cstring>
#include <limits>

namespace {

constexpr QChar kColumnKeySeparator(0x1f);
constexpr int kStreamingBufferSize = 256 * 1024;

/** @brief 十进制整数最多 20 位加符号；无区域设置、堆分配或旧 MinGW 不支持的 charconv 依赖。 */
int formatInteger(char (&buffer)[32], quint64 magnitude, bool negative)
{
    char *const end = buffer + sizeof(buffer);
    char *cursor = end;
    do {
        *--cursor = char('0' + magnitude % 10);
        magnitude /= 10;
    } while (magnitude != 0);
    if (negative) {
        *--cursor = '-';
    }
    const int size = int(end - cursor);
    std::memmove(buffer, cursor, size_t(size));
    return size;
}

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

ResultWriter::~ResultWriter()
{
    QMutexLocker locker(&mutex_);
    for (auto iterator = pending_tasks_.begin(); iterator != pending_tasks_.end(); ++iterator) {
        destroyStreamingOutput(&iterator.value());
    }
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

        task.writer_options = options_;
        task.output_path = makeOutputPath(definition, task.writer_options);
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
    QHash<QString, int> value_count_by_task;
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

        value_count_by_task[value.task_id] = value_count_by_task.value(value.task_id, 0) + 1;
    }

    QMutexLocker locker(&mutex_);
    for (auto iterator = value_count_by_task.constBegin(); iterator != value_count_by_task.constEnd(); ++iterator) {
        auto task_iterator = pending_tasks_.find(iterator.key());
        if (task_iterator == pending_tasks_.end()) {
            setError(error_message,
                     QStringLiteral("Result output for task '%1' has not been started").arg(iterator.key()));
            return false;
        }

        task_iterator.value().values.reserve(task_iterator.value().values.size() + iterator.value());
    }

    for (const ParameterValue &value : values) {
        pending_tasks_[value.task_id].values.append(value);
    }

    return true;
}

bool ResultWriter::startStreamingTask(const TaskDefinition &definition,
                                      const QList<StreamLayoutTable> &layout_tables,
                                      QString *error_message)
{
    if (definition.task_id.trimmed().isEmpty()) {
        setError(error_message, QStringLiteral("TaskDefinition.task_id is empty"));
        return false;
    }

    PendingTask task;
    task.definition = definition;
    task.streaming = true;

    {
        QMutexLocker locker(&mutex_);
        if (pending_tasks_.contains(definition.task_id)) {
            setError(error_message,
                     QStringLiteral("Result output for task '%1' has already started").arg(definition.task_id));
            return false;
        }

        task.writer_options = options_;
        task.output_path = makeOutputPath(definition, task.writer_options);
        task.streaming_columns = buildColumnsFromLayouts(definition, layout_tables);
        if (task.streaming_columns.isEmpty()) {
            setError(error_message,
                     QStringLiteral("Streaming output for task '%1' has no result columns")
                         .arg(definition.task_id));
            return false;
        }

        QString local_error;
        if (!initializeStreamingOutput(&task, options_, &local_error)) {
            setError(error_message, local_error);
            destroyStreamingOutput(&task);
            return false;
        }

        pending_tasks_.insert(definition.task_id, task);
    }

    log(LogLevel::Info,
        definition.task_id,
        QString(),
        QStringLiteral("Streaming result output started at '%1'").arg(task.output_path));
    emit taskOutputStarted(definition.task_id, task.output_path);
    return true;
}

bool ResultWriter::appendDecodedBlock(const QString &task_id,
                                      const QList<ParameterValue> &values,
                                      QString *error_message)
{
    if (values.isEmpty()) {
        return true;
    }

    ResultWriterOptions writer_options;
    {
        QMutexLocker locker(&mutex_);
        auto iterator = pending_tasks_.find(task_id);
        if (iterator == pending_tasks_.end()) {
            setError(error_message,
                     QStringLiteral("Result output for task '%1' has not been started").arg(task_id));
            return false;
        }
        if (!iterator.value().streaming) {
            setError(error_message,
                     QStringLiteral("Result output for task '%1' is not in streaming mode").arg(task_id));
            return false;
        }

        writer_options = iterator.value().writer_options;
        return appendStreamingValues(&iterator.value(), values, writer_options, error_message);
    }
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
        writer_options = task.writer_options;
        pending_tasks_.erase(iterator);
    }

    quint64 row_count = 0;
    QString local_error;
    const bool write_ok = task.streaming
        ? finishStreamingOutput(&task, &row_count, &local_error)
        : writeMergedFile(task, writer_options, &row_count, &local_error);
    if (!write_ok) {
        setError(error_message, local_error);
        log(LogLevel::Error, task_id, QString(), local_error);
        emit taskOutputFailed(task_id, local_error);
        destroyStreamingOutput(&task);
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
    auto iterator = pending_tasks_.find(task_id);
    if (iterator == pending_tasks_.end()) {
        return false;
    }

    PendingTask task = iterator.value();
    pending_tasks_.erase(iterator);
    destroyStreamingOutput(&task);
    return true;
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

QList<ResultWriter::ColumnDefinition> ResultWriter::buildColumnsFromLayouts(
    const TaskDefinition &definition,
    const QList<StreamLayoutTable> &layout_tables) const
{
    QList<ColumnDefinition> columns;
    QHash<QString, int> column_index_by_key;
    QHash<int, const StreamLayoutTable *> layout_by_stream_index;
    for (const StreamLayoutTable &layout_table : layout_tables) {
        layout_by_stream_index.insert(layout_table.stream_index, &layout_table);
    }

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
        const StreamLayoutTable *layout_table = layout_by_stream_index.value(group.stream_index, nullptr);
        for (const QString &parameter_id : group.parameter_ids) {
            if (layout_table == nullptr) {
                append_or_update_column(group.stream_index, parameter_id, QString(), QString());
                continue;
            }

            const StreamLayoutEntry *entry = layout_table->findById(parameter_id);
            if (entry == nullptr) {
                entry = layout_table->findByName(parameter_id);
            }

            if (entry == nullptr) {
                append_or_update_column(group.stream_index, parameter_id, QString(), QString());
                continue;
            }

            append_or_update_column(group.stream_index, entry->parameter_id, entry->parameter_name, entry->unit);
        }
    }

    return columns;
}

bool ResultWriter::initializeStreamingOutput(PendingTask *task,
                                             const ResultWriterOptions &options,
                                             QString *error_message) const
{
    if (task == nullptr) {
        setError(error_message, QStringLiteral("streaming task pointer is null"));
        return false;
    }

    QFileInfo output_info(task->output_path);
    QDir output_dir(output_info.absolutePath());
    if (!output_dir.exists() && !output_dir.mkpath(QStringLiteral("."))) {
        setError(error_message,
                 QStringLiteral("Failed to create result output directory '%1'").arg(output_dir.absolutePath()));
        return false;
    }

    task->stream_file = new QSaveFile(task->output_path);
    if (!task->stream_file->open(QIODevice::WriteOnly | QIODevice::Text)) {
        setError(error_message,
                 QStringLiteral("Failed to open result file '%1': %2")
                     .arg(task->output_path, task->stream_file->errorString()));
        return false;
    }

    // 仍由 Text 模式完成平台换行转换，UTF-8 不带 BOM，与原 QTextStream 保持一致。
    task->output_buffer.reserve(kStreamingBufferSize);
    task->missing_cell = options.missing_value_text.toUtf8();

    task->streaming_column_index_by_stream.clear();
    for (int index = 0; index < task->streaming_columns.size(); ++index) {
        const ColumnDefinition &column = task->streaming_columns.at(index);
        const QString id = column.parameter_id.trimmed();
        const QString key = id.isEmpty() ? column.parameter_name.trimmed() : id;
        task->streaming_column_index_by_stream[column.stream_index].insert(key, index);
    }

    task->output_buffer.append(options.legacy_time_format ? "TIME" : "timestamp_seconds");
    for (const ColumnDefinition &column : task->streaming_columns) {
        const QByteArray title = makeColumnTitle(column).toUtf8();
        if (options.include_raw_values) {
            task->output_buffer.append('\t').append(title).append("_raw");
        }
        task->output_buffer.append('\t').append(title);
    }
    task->output_buffer.append('\n');
    resetStreamingRow(task, options);
    return task->output_buffer.size() < kStreamingBufferSize || flushStreamingBuffer(task, error_message);
}

bool ResultWriter::appendStreamingValues(PendingTask *task,
                                         const QList<ParameterValue> &values,
                                         const ResultWriterOptions &options,
                                         QString *error_message) const
{
    if (task == nullptr || task->stream_file == nullptr) {
        setError(error_message, QStringLiteral("streaming output is not open"));
        return false;
    }

    if (!task->stream_error.isEmpty()) {
        setError(error_message, task->stream_error);
        return false;
    }

    for (const ParameterValue &value : values) {
        if (value.task_id != task->definition.task_id) {
            setError(error_message,
                     QStringLiteral("Streaming block for task '%1' contains value from task '%2'")
                         .arg(task->definition.task_id, value.task_id));
            return false;
        }

        // 同一采样行的数百/数千个值共享时间；仅时间改变时计算分行键与显示文本。
        if (task->current_timestamp_key.isEmpty() || value.timestamp_seconds != task->last_timestamp_seconds) {
            const QString timestamp_key = timestampKey(value.timestamp_seconds, options);
            if (task->current_timestamp_key.isEmpty() || timestamp_key != task->current_timestamp_key) {
                if (!flushStreamingRow(task, error_message)) {
                    return false;
                }
                resetStreamingRow(task, options);
                task->current_timestamp_key = timestamp_key;
                task->current_timestamp_text = formatTimestamp(value.timestamp_seconds, options);
            }
            task->last_timestamp_seconds = value.timestamp_seconds;
        }

        // 生产解算通常按固定列顺序返回；验证身份后复用顺序位置，乱序仍按流和 ID 查找。
        int base_column_index = -1;
        if (task->next_column_hint < task->streaming_columns.size()) {
            const ColumnDefinition &hint = task->streaming_columns.at(task->next_column_hint);
            const bool same_parameter = !hint.parameter_id.isEmpty()
                ? hint.parameter_id == value.parameter_id
                : value.parameter_id.isEmpty() && hint.parameter_name == value.parameter_name;
            if (hint.stream_index == value.stream_index && same_parameter) {
                base_column_index = task->next_column_hint;
            }
        }
        if (base_column_index < 0) {
            const auto stream_columns = task->streaming_column_index_by_stream.constFind(value.stream_index);
            const QString id = value.parameter_id.trimmed();
            const QString key = id.isEmpty() ? value.parameter_name.trimmed() : id;
            base_column_index = stream_columns == task->streaming_column_index_by_stream.cend()
                ? -1 : stream_columns->value(key, -1);
        }
        if (base_column_index >= 0) {
            task->next_column_hint = (base_column_index + 1) % task->streaming_columns.size();
        }
        const int value_column_index = options.include_raw_values ? (base_column_index * 2) + 1 : base_column_index;
        if (base_column_index < 0 || value_column_index >= task->current_row_cells.size()) {
            log(LogLevel::Warning,
                value.task_id,
                value.parameter_name,
                QStringLiteral("Skipping streamed result value without an output column"));
            continue;
        }

        if (options.include_raw_values) {
            formatStreamingCell(&task->current_row_cells[base_column_index * 2], value.raw_value);
        }
        formatStreamingCell(&task->current_row_cells[value_column_index], value.value);
    }

    return true;
}

bool ResultWriter::finishStreamingOutput(PendingTask *task,
                                         quint64 *row_count,
                                         QString *error_message) const
{
    if (task == nullptr || task->stream_file == nullptr) {
        setError(error_message, QStringLiteral("streaming output is not open"));
        return false;
    }

    if (!flushStreamingRow(task, error_message) || !flushStreamingBuffer(task, error_message)) {
        return false;
    }

    if (!task->stream_file->commit()) {
        setError(error_message,
                 QStringLiteral("Failed to commit result file '%1': %2")
                     .arg(task->output_path, task->stream_file->errorString()));
        delete task->stream_file;
        task->stream_file = nullptr;
        return false;
    }

    delete task->stream_file;
    task->stream_file = nullptr;
    if (row_count != nullptr) {
        *row_count = task->streamed_row_count;
    }
    return true;
}

void ResultWriter::resetStreamingRow(PendingTask *task, const ResultWriterOptions &options)
{
    if (task == nullptr) {
        return;
    }

    const int physical_column_count = task->streaming_columns.size() * (options.include_raw_values ? 2 : 1);
    task->current_row_cells.resize(physical_column_count);
    for (StreamingCell &cell : task->current_row_cells) {
        cell.present = false;
    }
}

bool ResultWriter::flushStreamingRow(PendingTask *task, QString *error_message)
{
    if (task->current_timestamp_key.isEmpty()) {
        return true;
    }
    const QByteArray timestamp = task->current_timestamp_text.toUtf8();
    // 一次扩容后直接填充字节，避免每列 QString 分配、QStringList::join 和 UTF-16 中转。
    qint64 row_size = timestamp.size() + 1;
    for (const StreamingCell &cell : qAsConst(task->current_row_cells)) {
        row_size += 1 + (!cell.present ? task->missing_cell.size()
            : cell.number_size >= 0 ? cell.number_size : cell.text.size());
    }
    if (row_size > std::numeric_limits<int>::max() - task->output_buffer.size()) {
        task->stream_error = QStringLiteral("Result row exceeds supported buffer size: '%1'").arg(task->output_path);
        setError(error_message, task->stream_error);
        return false;
    }
    const int offset = task->output_buffer.size();
    task->output_buffer.resize(offset + int(row_size));
    char *cursor = task->output_buffer.data() + offset;
    std::memcpy(cursor, timestamp.constData(), size_t(timestamp.size()));
    cursor += timestamp.size();
    for (const StreamingCell &cell : qAsConst(task->current_row_cells)) {
        *cursor++ = '\t';
        const char *data = !cell.present ? task->missing_cell.constData()
            : cell.number_size >= 0 ? cell.number : cell.text.constData();
        const int size = !cell.present ? task->missing_cell.size()
            : cell.number_size >= 0 ? cell.number_size : cell.text.size();
        std::memcpy(cursor, data, size_t(size));
        cursor += size;
    }
    *cursor = '\n';
    task->current_timestamp_key.clear();
    ++task->streamed_row_count;
    return task->output_buffer.size() < kStreamingBufferSize || flushStreamingBuffer(task, error_message);
}

bool ResultWriter::flushStreamingBuffer(PendingTask *task, QString *error_message)
{
    if (task->stream_error.isEmpty() && !task->output_buffer.isEmpty()) {
        const qint64 size = task->output_buffer.size();
        if (task->stream_file->write(task->output_buffer.constData(), size) != size) {
            task->stream_error = QStringLiteral("Failed while writing result file '%1': %2")
                .arg(task->output_path, task->stream_file->errorString());
            task->stream_file->cancelWriting();
        } else {
            // resize(0) 保留已分配容量；clear() 在部分 Qt 版本会释放缓冲。
            task->output_buffer.resize(0);
        }
    }
    if (!task->stream_error.isEmpty()) {
        setError(error_message, task->stream_error);
        return false;
    }
    return true;
}

void ResultWriter::formatStreamingCell(StreamingCell *cell, const QVariant &value)
{
    cell->present = true;
    cell->number_size = 0;
    if (!value.isValid() || value.isNull()) {
        return;
    }
    const int type = value.userType();
    if (type == QMetaType::Int || type == QMetaType::LongLong) {
        const qlonglong number = value.toLongLong();
        // 在无符号域求绝对值，LLONG_MIN 也不会触发有符号溢出。
        cell->number_size = formatInteger(cell->number,
            number < 0 ? quint64(0) - quint64(number) : quint64(number), number < 0);
    } else if (type == QMetaType::UInt || type == QMetaType::ULongLong) {
        cell->number_size = formatInteger(cell->number, value.toULongLong(), false);
    } else if (type == QMetaType::Bool) {
        const bool flag = value.toBool();
        cell->number_size = flag ? 4 : 5;
        std::memcpy(cell->number, flag ? "true" : "false", size_t(cell->number_size));
    } else {
        cell->number_size = -1;
        if (type == QMetaType::Float || type == QMetaType::Double) {
            const double number = value.toDouble();
            // g17 在十进制指数 0..16 且无小数时输出精确整数。先限制范围再转换，
            // 避免 NaN/Inf/越界转换；零（含负零）和科学计数边界仍交给 Qt。
            if (number > -1e17 && number < 1e17 && number != 0) {
                const qlonglong integer = static_cast<qlonglong>(number);
                if (number == double(integer)) {
                    cell->number_size = formatInteger(cell->number,
                        integer < 0 ? quint64(0) - quint64(integer) : quint64(integer), integer < 0);
                    return;
                }
            }
            cell->text = QByteArray::number(number, 'g', 17);
        } else {
            cell->text = variantToText(value).toUtf8();
        }
    }
}

void ResultWriter::destroyStreamingOutput(PendingTask *task)
{
    if (task == nullptr) {
        return;
    }

    if (task->stream_file != nullptr) {
        task->stream_file->cancelWriting();
        delete task->stream_file;
        task->stream_file = nullptr;
    }
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

    stream << (options.legacy_time_format ? QStringLiteral("TIME") : QStringLiteral("timestamp_seconds"));
    for (const ColumnDefinition &column : columns) {
        const QString title = makeColumnTitle(column);
        if (options.include_raw_values) {
            stream << '\t' << title + QStringLiteral("_raw");
        }
        stream << '\t' << title;
    }
    stream << '\n';

    QString current_timestamp_key;
    QString current_timestamp_text;
    QStringList row_values;
    quint64 emitted_rows = 0;

    auto reset_row = [&]() {
        row_values.clear();
        const int physical_column_count = columns.size() * (options.include_raw_values ? 2 : 1);
        row_values.reserve(physical_column_count);
        for (int index = 0; index < physical_column_count; ++index) {
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
        const QString timestamp_key = timestampKey(value.timestamp_seconds, options);
        if (current_timestamp_key.isEmpty()) {
            current_timestamp_key = timestamp_key;
            current_timestamp_text = timestamp_text;
        } else if (timestamp_key != current_timestamp_key) {
            flush_row();
            reset_row();
            current_timestamp_key = timestamp_key;
            current_timestamp_text = timestamp_text;
        }

        const int base_column_index = column_index_by_key.value(makeColumnKey(value), -1);
        const int value_column_index = options.include_raw_values ? (base_column_index * 2) + 1 : base_column_index;
        if (base_column_index < 0 || value_column_index >= row_values.size()) {
            log(LogLevel::Warning,
                value.task_id,
                value.parameter_name,
                QStringLiteral("Skipping result value without an output column"));
            continue;
        }

        if (options.include_raw_values) {
            row_values[base_column_index * 2] = variantToText(value.raw_value);
        }
        row_values[value_column_index] = variantToText(value.value);
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

QString ResultWriter::timestampKey(double timestamp_seconds, const ResultWriterOptions &options)
{
    const int precision = options.legacy_time_format ? qMax(9, options.timestamp_precision)
                                                     : options.timestamp_precision;
    return QString::number(timestamp_seconds, 'f', precision);
}

QString ResultWriter::formatTimestamp(double timestamp_seconds, const ResultWriterOptions &options)
{
    if (!options.legacy_time_format || !std::isfinite(timestamp_seconds)) {
        return QString::number(timestamp_seconds, 'f', options.timestamp_precision);
    }
    double day_seconds = std::fmod(timestamp_seconds, 86400.0);
    if (day_seconds < 0) {
        day_seconds += 86400.0;
    }
    // 对齐内部分行的纳秒精度，消除整毫秒边界的浮点误差，再按旧版 Time2S 截断毫秒以下。
    const qint64 nanoseconds = qRound64(day_seconds * 1e9);
    const qint64 milliseconds = nanoseconds / 1000000 % 86400000;
    return QStringLiteral("%1:%2:%3:%4")
        .arg(milliseconds / 3600000, 2, 10, QLatin1Char('0'))
        .arg(milliseconds / 60000 % 60, 2, 10, QLatin1Char('0'))
        .arg(milliseconds / 1000 % 60, 2, 10, QLatin1Char('0'))
        .arg(milliseconds % 1000, 3, 10, QLatin1Char('0'));
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
    const QString parameter_name = column.parameter_name.trimmed();
    if (!parameter_name.isEmpty()) {
        return sanitizeCell(parameter_name);
    }

    return sanitizeCell(column.parameter_id.trimmed());
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

    const int type = value.userType();
    QString text;

    if (type == QMetaType::Float || type == QMetaType::Double) {
        return QString::number(value.toDouble(), 'g', 17);
    } else if (type == QMetaType::Int || type == QMetaType::LongLong) {
        return QString::number(value.toLongLong());
    } else if (type == QMetaType::UInt || type == QMetaType::ULongLong) {
        return QString::number(value.toULongLong());
    } else if (type == QMetaType::Bool) {
        return value.toBool() ? QStringLiteral("true") : QStringLiteral("false");
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
