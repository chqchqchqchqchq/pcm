#include "traditional_processing_controller.h"

#include "config_file_document.h"
#include "config_parser.h"
#include "fast_fixed_pcm_processor.h"
#include "logger.h"

#include <QDate>
#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QMap>
#include <QMutexLocker>
#include <QRegularExpression>
#include <QThread>
#include <QTimer>
#include <QtGlobal>

#include <algorithm>
#include <limits>

namespace {

enum class ConfigSection
{
    None,
    StreamFormat,
    ParameterDescription
};

void setError(QString *error_message, const QString &message)
{
    if (error_message != nullptr) {
        *error_message = message;
    }
}

QString textFromBytes(const QByteArray &content_bytes)
{
    QString content = QString::fromUtf8(content_bytes);
    if (content.contains(QChar(QChar::ReplacementCharacter))) {
        content = QString::fromLocal8Bit(content_bytes);
    }

    return content;
}

QString stripTemplateComment(const QString &line)
{
    QString normalized_line = line.trimmed();
    const int ascii_comment_index = normalized_line.indexOf(QLatin1Char('('));
    const int chinese_comment_index = normalized_line.indexOf(QChar(0xff08));
    int comment_index = -1;
    if (ascii_comment_index >= 0 && chinese_comment_index >= 0) {
        comment_index = qMin(ascii_comment_index, chinese_comment_index);
    } else {
        comment_index = qMax(ascii_comment_index, chinese_comment_index);
    }

    if (comment_index >= 0) {
        normalized_line = normalized_line.left(comment_index).trimmed();
    }

    return normalized_line;
}

QString normalizedConfigLine(QString line)
{
    line.remove(QChar(0xfeff));
    line.replace(QChar(0xff1a), QLatin1Char(':'));
    return stripTemplateComment(line);
}

QString normalizedSectionTitle(QString line)
{
    line = normalizedConfigLine(line);
    if (line.endsWith(QLatin1Char(':'))) {
        line.chop(1);
    }

    return line.trimmed();
}

bool parseStreamFormatSectionLine(const QString &line, int *stream_index)
{
    const QRegularExpression expression(QStringLiteral("^The format description of stream (\\d+)$"));
    const QRegularExpressionMatch match = expression.match(normalizedSectionTitle(line));
    if (!match.hasMatch()) {
        return false;
    }

    bool ok = false;
    const int parsed_stream_index = match.captured(1).toInt(&ok);
    if (!ok || parsed_stream_index <= 0) {
        return false;
    }

    if (stream_index != nullptr) {
        *stream_index = parsed_stream_index;
    }
    return true;
}

bool parseParameterSectionLine(const QString &line, int *stream_index)
{
    const QRegularExpression expression(QStringLiteral("^the_parameter_description_of_stream_(\\d+)$"));
    const QRegularExpressionMatch match = expression.match(normalizedSectionTitle(line));
    if (!match.hasMatch()) {
        return false;
    }

    bool ok = false;
    const int parsed_stream_index = match.captured(1).toInt(&ok);
    if (!ok || parsed_stream_index <= 0) {
        return false;
    }

    if (stream_index != nullptr) {
        *stream_index = parsed_stream_index;
    }
    return true;
}

bool splitConfigKeyValue(const QString &line, QString *key, QString *value)
{
    const QString normalized_line = normalizedConfigLine(line);
    const int separator_index = normalized_line.indexOf(QLatin1Char(':'));
    if (separator_index < 0) {
        return false;
    }

    const QString parsed_key = normalized_line.left(separator_index).trimmed();
    if (parsed_key.isEmpty()) {
        return false;
    }

    if (key != nullptr) {
        *key = parsed_key;
    }
    if (value != nullptr) {
        *value = normalized_line.mid(separator_index + 1).trimmed();
    }
    return true;
}

bool readTextFile(const QString &file_path, QString *content, QString *error_message)
{
    QFile file(file_path);
    if (!file.open(QIODevice::ReadOnly | QIODevice::Text)) {
        setError(error_message, QStringLiteral("无法打开配置文件 '%1'：%2").arg(file_path, file.errorString()));
        return false;
    }

    if (content != nullptr) {
        *content = textFromBytes(file.readAll());
    }
    return true;
}

QMap<int, QMap<QString, QString>> parseStreamFormatFields(const QString &content)
{
    QString normalized_content = content;
    normalized_content.replace(QStringLiteral("\r\n"), QStringLiteral("\n"));
    normalized_content.replace(QLatin1Char('\r'), QLatin1Char('\n'));
    const QStringList lines = normalized_content.split(QLatin1Char('\n'));

    QMap<int, QMap<QString, QString>> fields_by_stream;
    ConfigSection current_section = ConfigSection::None;
    int current_stream_index = 0;

    for (const QString &line : lines) {
        int parsed_stream_index = 0;
        if (parseStreamFormatSectionLine(line, &parsed_stream_index)) {
            current_section = ConfigSection::StreamFormat;
            current_stream_index = parsed_stream_index;
            fields_by_stream.insert(current_stream_index, QMap<QString, QString>());
            continue;
        }

        if (parseParameterSectionLine(line, &parsed_stream_index) ||
            normalizedSectionTitle(line) == ConfigTemplateKeys::headerSection()) {
            current_section = ConfigSection::None;
            current_stream_index = 0;
            continue;
        }

        if (current_section != ConfigSection::StreamFormat || current_stream_index <= 0) {
            continue;
        }

        QString key;
        QString value;
        if (splitConfigKeyValue(line, &key, &value) && ConfigTemplateKeys::isStreamKey(key)) {
            fields_by_stream[current_stream_index].insert(key, value);
        }
    }

    return fields_by_stream;
}

bool parseInt(const QString &text, int *value)
{
    bool ok = false;
    const int parsed_value = text.trimmed().toInt(&ok, 0);
    if (!ok) {
        return false;
    }

    if (value != nullptr) {
        *value = parsed_value;
    }
    return true;
}

bool parseDouble(const QString &text, double *value)
{
    bool ok = false;
    const double parsed_value = text.trimmed().toDouble(&ok);
    if (!ok) {
        return false;
    }

    if (value != nullptr) {
        *value = parsed_value;
    }
    return true;
}

bool readIntField(const QMap<QString, QString> &fields,
                  const QString &key,
                  int default_value,
                  bool required,
                  int *value,
                  QString *error_message)
{
    const QString text = fields.value(key).trimmed();
    if (text.isEmpty()) {
        if (required) {
            setError(error_message, QStringLiteral("配置流缺少字段 '%1'").arg(key));
            return false;
        }
        if (value != nullptr) {
            *value = default_value;
        }
        return true;
    }

    int parsed_value = 0;
    if (!parseInt(text, &parsed_value)) {
        setError(error_message, QStringLiteral("字段 '%1' 不是有效整数：%2").arg(key, text));
        return false;
    }

    if (value != nullptr) {
        *value = parsed_value;
    }
    return true;
}

bool readDoubleField(const QMap<QString, QString> &fields,
                     const QString &key,
                     double default_value,
                     double *value,
                     QString *error_message)
{
    const QString text = fields.value(key).trimmed();
    if (text.isEmpty()) {
        if (value != nullptr) {
            *value = default_value;
        }
        return true;
    }

    double parsed_value = 0.0;
    if (!parseDouble(text, &parsed_value)) {
        setError(error_message, QStringLiteral("字段 '%1' 不是有效数字：%2").arg(key, text));
        return false;
    }

    if (value != nullptr) {
        *value = parsed_value;
    }
    return true;
}

QString unquote(QString text)
{
    text = text.trimmed();
    if (text.size() >= 2 &&
        ((text.startsWith(QLatin1Char('"')) && text.endsWith(QLatin1Char('"'))) ||
         (text.startsWith(QLatin1Char('\'')) && text.endsWith(QLatin1Char('\''))))) {
        text = text.mid(1, text.size() - 2).trimmed();
    }
    return text;
}

bool isHexText(const QString &text)
{
    if (text.isEmpty()) {
        return false;
    }

    for (const QChar ch : text) {
        if (!ch.isDigit() &&
            (ch.toLower() < QLatin1Char('a') || ch.toLower() > QLatin1Char('f'))) {
            return false;
        }
    }
    return true;
}

bool containsHexLetter(const QString &text)
{
    for (const QChar ch : text) {
        const QChar lower = ch.toLower();
        if (lower >= QLatin1Char('a') && lower <= QLatin1Char('f')) {
            return true;
        }
    }
    return false;
}

QByteArray unsignedToBigEndianBytes(quint64 value, int byte_count)
{
    QByteArray bytes;
    for (int index = byte_count - 1; index >= 0; --index) {
        bytes.append(static_cast<char>((value >> (index * 8)) & 0xff));
    }
    return bytes;
}

int minimumByteCount(quint64 value)
{
    int byte_count = 1;
    while (byte_count < 8 && (value >> (byte_count * 8)) != 0) {
        ++byte_count;
    }
    return byte_count;
}

bool parseByteToken(const QString &token, char *byte, QString *error_message)
{
    QString normalized = unquote(token);
    bool base_is_hex = normalized.startsWith(QStringLiteral("0x"), Qt::CaseInsensitive);
    if (base_is_hex) {
        normalized = normalized.mid(2);
    } else {
        for (const QChar ch : normalized) {
            if (ch.isLetter()) {
                base_is_hex = true;
                break;
            }
        }
    }

    bool ok = false;
    const uint parsed_value = normalized.toUInt(&ok, base_is_hex ? 16 : 10);
    if (!ok || parsed_value > 0xff) {
        setError(error_message, QStringLiteral("同步字节无效：%1").arg(token));
        return false;
    }

    if (byte != nullptr) {
        *byte = static_cast<char>(parsed_value);
    }
    return true;
}

bool reorderSyncBytesForRawWordOrder(const QByteArray &logical_bytes,
                                     int word_length_bits,
                                     QByteArray *raw_bytes,
                                     QString *error_message)
{
    if (word_length_bits <= 0) {
        setError(error_message, QStringLiteral("word_length 必须大于 0，才能解析同步字字节序"));
        return false;
    }
    if ((word_length_bits % 8) != 0) {
        setError(error_message, QStringLiteral("当前同步字字节序转换仅支持字节对齐的 word_length"));
        return false;
    }

    const int word_byte_count = word_length_bits / 8;
    if (word_byte_count <= 1) {
        if (raw_bytes != nullptr) {
            *raw_bytes = logical_bytes;
        }
        return true;
    }

    QByteArray reordered;
    reordered.reserve(logical_bytes.size());
    for (int offset = 0; offset < logical_bytes.size(); offset += word_byte_count) {
        const int group_size = qMin(word_byte_count, logical_bytes.size() - offset);
        for (int index = group_size - 1; index >= 0; --index) {
            reordered.append(logical_bytes.at(offset + index));
        }
    }

    if (raw_bytes != nullptr) {
        *raw_bytes = reordered;
    }
    return true;
}

bool parseSyncPattern(const QString &text,
                      int sync_word_length_bits,
                      int word_length_bits,
                      QByteArray *sync_word,
                      QString *error_message)
{
    QString normalized = unquote(text);
    if (normalized.isEmpty()) {
        setError(error_message, QStringLiteral("sync_pattern 不能为空"));
        return false;
    }

    int expected_byte_count = 0;
    if (sync_word_length_bits > 0) {
        if (sync_word_length_bits % 8 != 0) {
            setError(error_message, QStringLiteral("当前最小链路仅支持字节对齐同步字"));
            return false;
        }
        expected_byte_count = sync_word_length_bits / 8;
    }

    QString tokenized = normalized;
    tokenized.replace(QLatin1Char(','), QLatin1Char(' '));
    tokenized.replace(QLatin1Char(';'), QLatin1Char(' '));
    const QStringList tokens = tokenized.split(QRegularExpression(QStringLiteral("\\s+")), Qt::SkipEmptyParts);

    QByteArray parsed_bytes;
    if (tokens.size() > 1) {
        for (const QString &token : tokens) {
            char parsed_byte = 0;
            if (!parseByteToken(token, &parsed_byte, error_message)) {
                return false;
            }
            parsed_bytes.append(parsed_byte);
        }
    } else {
        QString compact = tokens.isEmpty() ? normalized : tokens.first();
        bool has_hex_prefix = compact.startsWith(QStringLiteral("0x"), Qt::CaseInsensitive);
        if (has_hex_prefix) {
            compact = compact.mid(2);
        }

        const bool parse_as_hex = isHexText(compact) && (has_hex_prefix || containsHexLetter(compact));
        if (parse_as_hex) {
            if ((compact.size() % 2) != 0) {
                compact.prepend(QLatin1Char('0'));
            }
            parsed_bytes = QByteArray::fromHex(compact.toLatin1());
        } else {
            bool ok = false;
            const quint64 value = unquote(tokens.isEmpty() ? normalized : tokens.first()).toULongLong(&ok, 0);
            if (!ok) {
                setError(error_message, QStringLiteral("sync_pattern 无法解析：%1").arg(text));
                return false;
            }

            const int byte_count = expected_byte_count > 0 ? expected_byte_count : minimumByteCount(value);
            parsed_bytes = unsignedToBigEndianBytes(value, byte_count);
        }
    }

    if (parsed_bytes.isEmpty()) {
        setError(error_message, QStringLiteral("sync_pattern 未产生有效字节"));
        return false;
    }

    if (expected_byte_count > 0 && parsed_bytes.size() != expected_byte_count) {
        setError(error_message,
                 QStringLiteral("sync_pattern 字节数为 %1，但 sync_word_length 要求 %2 字节")
                     .arg(parsed_bytes.size())
                     .arg(expected_byte_count));
        return false;
    }

    QByteArray raw_order_bytes;
    if (!reorderSyncBytesForRawWordOrder(parsed_bytes, word_length_bits, &raw_order_bytes, error_message)) {
        return false;
    }

    if (sync_word != nullptr) {
        *sync_word = raw_order_bytes;
    }
    return true;
}

QStringList splitValueList(const QString &text)
{
    return text.split(QRegularExpression(QStringLiteral("[,;\\s]+")), Qt::SkipEmptyParts);
}

QList<int> parseIntList(const QString &text)
{
    QList<int> values;
    for (const QString &token : splitValueList(text)) {
        int value = 0;
        if (parseInt(token, &value)) {
            values.append(value);
        }
    }
    return values;
}

int valueAtOrZero(const QList<int> &values, int index)
{
    return index >= 0 && index < values.size() ? values.at(index) : 0;
}

bool parseBoolToken(const QString &text)
{
    const QString normalized = text.trimmed().toLower();
    return normalized == QStringLiteral("1") ||
        normalized == QStringLiteral("true") ||
        normalized == QStringLiteral("yes") ||
        normalized == QStringLiteral("y");
}

ParameterDescription parameterFromRecord(const ConfigParameterRecord &record)
{
    ParameterDescription parameter;
    parameter.serial_no = record.serial_no;
    parameter.id_no = record.idNo();
    parameter.name = record.name();
    parameter.unit = record.value(QStringLiteral("unit"));
    parameter.code_type_token = record.value(QStringLiteral("code_type"));
    parameter.code_type = parameterCodeTypeFromToken(parameter.code_type_token);
    parameter.calibration.type_token = record.value(QStringLiteral("cal_type"));
    parameter.calibration.type = calibrationTypeFromToken(parameter.calibration.type_token);
    parameter.calibration.cal_date = QDate::fromString(record.value(QStringLiteral("cal_date")), Qt::ISODate);
    parameter.calibration.cal_number = record.value(QStringLiteral("cal_number"));
    parseDouble(record.value(QStringLiteral("rate")), &parameter.rate_hz);
    parseInt(record.value(QStringLiteral("word_interval")), &parameter.word_interval);
    parseInt(record.value(QStringLiteral("sign_bit")), &parameter.sign_bit);
    parameter.inverse = parseBoolToken(record.value(QStringLiteral("inverse")));
    parseInt(record.value(QStringLiteral("pcm_words")), &parameter.pcm_words);
    if (parameter.pcm_words <= 0) {
        parameter.pcm_words = 1;
    }
    parameter.description = record.value(QStringLiteral("description"));

    const QList<int> frame_numbers = parseIntList(record.value(QStringLiteral("frame_no")));
    const QList<int> word_numbers = parseIntList(record.value(QStringLiteral("word_no")));
    const QList<int> source_bits = parseIntList(record.value(QStringLiteral("source_bit")));
    const QList<int> bit_lengths = parseIntList(record.value(QStringLiteral("bit_length")));
    const QList<int> object_bits = parseIntList(record.value(QStringLiteral("object_bit")));
    const int segment_count = qMax(kDefaultParameterWordColumns,
                                   qMax(frame_numbers.size(),
                                        qMax(word_numbers.size(),
                                             qMax(source_bits.size(),
                                                  qMax(bit_lengths.size(), object_bits.size())))));

    for (int index = 0; index < segment_count; ++index) {
        ParameterWordSegment segment;
        segment.frame_no = valueAtOrZero(frame_numbers, index);
        segment.word_no = valueAtOrZero(word_numbers, index);
        segment.source_bit = valueAtOrZero(source_bits, index);
        segment.bit_length = valueAtOrZero(bit_lengths, index);
        segment.object_bit = valueAtOrZero(object_bits, index);
        parameter.word_segments.append(segment);
    }

    return parameter;
}

QString streamIdForIndex(int stream_index)
{
    return QStringLiteral("stream_%1").arg(stream_index);
}

bool matchesRequestedParameter(const QString &requested, const StreamLayoutEntry &entry)
{
    return requested.compare(entry.parameter_id, Qt::CaseInsensitive) == 0 ||
        requested.compare(entry.parameter_name, Qt::CaseInsensitive) == 0;
}

QString normalizedRequestedParameter(QString parameter_name)
{
    parameter_name = unquote(parameter_name);
    parameter_name.remove(QChar(0xfeff));
    return parameter_name.trimmed();
}

} // namespace

struct TraditionalProcessingController::ActiveJob
{
    TaskDefinition definition;
    QList<StreamDefinition> streams;
    QList<StreamLayoutTable> layout_tables;
    QHash<QString, QSet<QString>> selected_parameter_ids_by_stream;
    FrameProcessor *processor = nullptr;
    FastFixedPcmProcessor *fast_processor = nullptr;
    QThread *fast_thread = nullptr;
    bool fast_path = false;
    bool reader_finished = false;
    bool processor_finished = false;
    bool failed = false;
    bool failure_emitted = false;
    bool stopped = false;
    bool finalized = false;
    quint64 produced_frame_count = 0;
    quint64 processed_frame_count = 0;
    quint64 produced_value_count = 0;
    int last_progress_percent = 0;
    QString failure_reason;
};

TraditionalProcessingController::TraditionalProcessingController(QObject *parent)
    : QObject(parent)
{
    qRegisterMetaType<TraditionalProcessingOptions>("TraditionalProcessingOptions");
    qRegisterMetaType<ParameterValue>("ParameterValue");
    qRegisterMetaType<QList<ParameterValue>>("QList<ParameterValue>");
    scheduler_.setMaxActiveTasks(1);
    scheduler_.setFrameQueueMaxSize(256);

    connect(&scheduler_,
            &TaskScheduler::taskQueuesReady,
            this,
            &TraditionalProcessingController::handleTaskQueuesReady);
    connect(&scheduler_, &TaskScheduler::taskStarted, this, [this](const QString &task_id) {
        log(LogLevel::Info, task_id, QStringLiteral("传统处理任务开始读取原始数据"));
        emit taskStarted(publicTaskId(task_id));
    });
    connect(&scheduler_,
            &TaskScheduler::taskProgressChanged,
            this,
            &TraditionalProcessingController::handleSchedulerTaskProgress);
    connect(&scheduler_,
            &TaskScheduler::taskFailed,
            this,
            &TraditionalProcessingController::handleSchedulerTaskFailed);
    connect(&scheduler_,
            &TaskScheduler::taskStopped,
            this,
            &TraditionalProcessingController::handleSchedulerTaskStopped);
    connect(&scheduler_,
            &TaskScheduler::taskFinished,
            this,
            &TraditionalProcessingController::handleSchedulerTaskFinished);
}

TraditionalProcessingController::~TraditionalProcessingController()
{
    scheduler_.requestStopAll();

    const QList<QString> task_ids = active_jobs_.keys();
    for (const QString &task_id : task_ids) {
        ActiveJob *job = active_jobs_.take(task_id);
        if (job == nullptr) {
            continue;
        }

        writer_.abortTask(task_id);
        if (job->processor != nullptr) {
            job->processor->requestStop();
            job->processor->waitForFinished();
            delete job->processor;
            job->processor = nullptr;
        }
        if (job->fast_processor != nullptr) {
            job->fast_processor->requestStop();
            job->fast_processor->deleteLater();
            job->fast_processor = nullptr;
        }
        if (job->fast_thread != nullptr) {
            job->fast_thread->quit();
            job->fast_thread->wait();
            delete job->fast_thread;
            job->fast_thread = nullptr;
        }
        scheduler_.unregisterConfigStreams(job->definition.config_file_path);
        delete job;
    }

    current_task_id_.clear();
}

bool TraditionalProcessingController::validateOptions(const TraditionalProcessingOptions &options,
                                                      QString *error_message) const
{
    const QFileInfo config_info(options.task_definition.config_file_path);
    if (!config_info.exists() || !config_info.isFile() || !config_info.isReadable()) {
        setError(error_message,
                 QStringLiteral("配置文件不存在或不可读：%1")
                     .arg(options.task_definition.config_file_path));
        return false;
    }

    const QFileInfo raw_info(options.task_definition.raw_file_path);
    if (!raw_info.exists() || !raw_info.isFile() || !raw_info.isReadable()) {
        setError(error_message,
                 QStringLiteral("原始数据文件不存在或不可读：%1")
                     .arg(options.task_definition.raw_file_path));
        return false;
    }

    Config config;
    if (!loadConfig(options.task_definition.config_file_path, &config, error_message)) {
        return false;
    }

    QList<StreamDefinition> streams;
    if (!buildStreamDefinitions(config, &streams, error_message)) {
        return false;
    }

    TaskDefinition definition = options.task_definition;
    QHash<QString, QSet<QString>> selected_parameter_ids_by_stream;
    if (!applyParameterSelection(options,
                                 config,
                                 &definition,
                                 &selected_parameter_ids_by_stream,
                                 error_message)) {
        return false;
    }
    for (const StreamDefinition &stream : streams) {
        if (!selected_parameter_ids_by_stream.contains(stream.stream_id)) {
            continue;
        }
        const QString path = rawFilePathForStream(definition.raw_file_path, stream.stream_index);
        QFile source(path);
        if (!source.open(QIODevice::ReadOnly)) {
            setError(error_message,
                     QStringLiteral("流 %1 的原始文件不可读：%2；%3")
                         .arg(stream.stream_index).arg(path, source.errorString()));
            return false;
        }
    }

    return true;
}

void TraditionalProcessingController::startProcessing(const TraditionalProcessingOptions &options)
{
    if (batch_active_ || !current_task_id_.isEmpty()) {
        emit taskFailed(options.task_definition.task_id, QStringLiteral("已有传统处理任务正在运行"));
        return;
    }

    batch_options_ = options;
    batch_ranges_ = options.time_ranges;
    if (batch_ranges_.isEmpty()) {
        batch_ranges_.append(TaskTimeRange());
    }
    for (const TaskTimeRange &range : batch_ranges_) {
        if (range.start_seconds < 0.0 || (range.hasEnd() && range.end_seconds <= range.start_seconds)) {
            emit taskFailed(options.task_definition.task_id, QStringLiteral("处理时间范围无效"));
            resetBatch();
            return;
        }
    }

    batch_request_id_ = options.task_definition.task_id;
    completed_output_paths_.clear();
    current_range_index_ = -1;
    batch_stop_requested_ = false;
    batch_active_ = true;
    startNextRange();
}

void TraditionalProcessingController::startNextRange()
{
    if (!batch_active_) {
        return;
    }
    if (batch_stop_requested_) {
        const QString request_id = batch_request_id_;
        resetBatch();
        emit taskStopped(request_id);
        return;
    }

    ++current_range_index_;
    if (current_range_index_ >= batch_ranges_.size()) {
        const QString request_id = batch_request_id_;
        const QStringList output_paths = completed_output_paths_;
        resetBatch();
        emit processingFinished(request_id, output_paths);
        return;
    }

    TraditionalProcessingOptions range_options = batch_options_;
    const int range_count = batch_ranges_.size();
    range_options.time_ranges = {batch_ranges_.at(current_range_index_)};
    range_options.task_definition.time_range = range_options.time_ranges.first();
    if (range_count > 1) {
        range_options.task_definition.task_id = QStringLiteral("%1_range_%2")
            .arg(batch_request_id_)
            .arg(current_range_index_ + 1, 3, 10, QLatin1Char('0'));
        const QFileInfo output_info(batch_options_.output_file_path);
        const QString suffix = output_info.suffix().isEmpty() ? QStringLiteral("txt") : output_info.suffix();
        range_options.output_file_path = QDir(output_info.absolutePath())
            .filePath(QStringLiteral("%1_range%2.%3")
                          .arg(output_info.completeBaseName())
                          .arg(current_range_index_ + 1, 3, 10, QLatin1Char('0'))
                          .arg(suffix));
    }

    startSingleProcessing(range_options);
    if (current_task_id_.isEmpty() && batch_active_) {
        resetBatch();
    }
}

void TraditionalProcessingController::startSingleProcessing(const TraditionalProcessingOptions &options)
{
    if (!current_task_id_.isEmpty()) {
        emit taskFailed(publicTaskId(options.task_definition.task_id), rangeDetail(QStringLiteral("已有传统处理任务正在运行")));
        return;
    }

    Config config;
    QString error_message;
    if (!loadConfig(options.task_definition.config_file_path, &config, &error_message)) {
        emit taskFailed(publicTaskId(options.task_definition.task_id), failureDetail(error_message));
        return;
    }

    QList<StreamDefinition> streams;
    if (!buildStreamDefinitions(config, &streams, &error_message)) {
        emit taskFailed(publicTaskId(options.task_definition.task_id), failureDetail(error_message));
        return;
    }

    TaskDefinition definition = options.task_definition;
    QHash<QString, QSet<QString>> selected_parameter_ids_by_stream;
    if (!applyParameterSelection(options, config, &definition, &selected_parameter_ids_by_stream, &error_message)) {
        emit taskFailed(publicTaskId(options.task_definition.task_id), failureDetail(error_message));
        return;
    }

    ResultWriterOptions writer_options;
    writer_options.output_file_path = options.output_file_path;
    writer_options.include_raw_values = options.include_raw_values;

    ActiveJob *job = new ActiveJob;
    job->definition = definition;
    job->streams = streams;
    job->layout_tables = config.layout_tables;
    job->selected_parameter_ids_by_stream = selected_parameter_ids_by_stream;
    active_jobs_.insert(definition.task_id, job);
    current_task_id_ = definition.task_id;

    QString fast_path_reason;
    if (FastFixedPcmProcessor::canUseFastPath(definition,
                                             streams,
                                             config.layout_tables,
                                             selected_parameter_ids_by_stream,
                                             &fast_path_reason)) {
        FastFixedPcmTask fast_task;
        fast_task.definition = definition;
        fast_task.streams = streams;
        fast_task.layout_tables = config.layout_tables;
        fast_task.selected_parameter_ids_by_stream = selected_parameter_ids_by_stream;
        fast_task.writer_options = writer_options;

        FastFixedPcmProcessorOptions fast_options;
        fast_options.worker_count = qMax(1, QThread::idealThreadCount() - 1);
        fast_options.block_size_bytes = 256 * 1024 * 1024;
        fast_options.resync_window_bytes = 1024 * 1024;

        job->fast_path = true;
        job->fast_thread = new QThread;
        job->fast_processor = new FastFixedPcmProcessor(fast_task, fast_options);
        job->fast_thread->setObjectName(QStringLiteral("FastFixedPcmProcessor-%1").arg(definition.task_id));
        job->fast_processor->moveToThread(job->fast_thread);

        connect(job->fast_thread, &QThread::started, job->fast_processor, &FastFixedPcmProcessor::process);
        connect(job->fast_processor,
                &FastFixedPcmProcessor::started,
                this,
                &TraditionalProcessingController::handleFastTaskStarted);
        connect(job->fast_processor,
                &FastFixedPcmProcessor::progressChanged,
                this,
                &TraditionalProcessingController::handleFastTaskProgress);
        connect(job->fast_processor,
                &FastFixedPcmProcessor::failed,
                this,
                &TraditionalProcessingController::handleFastTaskFailed);
        connect(job->fast_processor,
                &FastFixedPcmProcessor::stopped,
                this,
                &TraditionalProcessingController::handleFastTaskStopped);
        connect(job->fast_processor,
                &FastFixedPcmProcessor::finished,
                this,
                &TraditionalProcessingController::handleFastTaskFinished);

        job->fast_thread->start();
        emit taskProgressChanged(publicTaskId(definition.task_id),
                                 aggregateProgress(0),
                                 rangeDetail(QStringLiteral("固定帧高速任务已启动")));
        log(LogLevel::Info, definition.task_id, QStringLiteral("固定帧高速路径已启用"));
        return;
    }

    log(LogLevel::Info,
        definition.task_id,
        QStringLiteral("固定帧高速路径未启用，使用通用扫描链路：%1").arg(fast_path_reason));

    writer_.setOptions(writer_options);
    if (!writer_.startTask(definition, &error_message)) {
        cleanupJob(definition.task_id);
        emit taskFailed(publicTaskId(definition.task_id), failureDetail(error_message));
        return;
    }

    scheduler_.registerConfigStreams(definition.config_file_path, streams);
    if (!scheduler_.submitTask(definition, &error_message)) {
        writer_.abortTask(definition.task_id);
        cleanupJob(definition.task_id);
        emit taskFailed(publicTaskId(definition.task_id), failureDetail(error_message));
        return;
    }

    emit taskProgressChanged(publicTaskId(definition.task_id),
                             aggregateProgress(0),
                             rangeDetail(QStringLiteral("任务已提交")));
    log(LogLevel::Info, definition.task_id, QStringLiteral("传统处理任务已提交到后端链路"));
}

void TraditionalProcessingController::requestStop()
{
    batch_stop_requested_ = true;
    const QString task_id = current_task_id_;
    if (task_id.isEmpty()) {
        return;
    }

    ActiveJob *job = active_jobs_.value(task_id, nullptr);
    if (job != nullptr) {
        job->stopped = true;
    }

    log(LogLevel::Warning, task_id, QStringLiteral("收到用户中断请求"));
    scheduler_.requestStop(task_id);
    if (job != nullptr && job->processor != nullptr) {
        job->processor->requestStop();
    }
    if (job != nullptr && job->fast_processor != nullptr) {
        job->fast_processor->requestStop();
    }
    const int local_progress = job == nullptr ? 0 : job->last_progress_percent;
    emit taskProgressChanged(publicTaskId(task_id), aggregateProgress(local_progress), rangeDetail(QStringLiteral("正在中断")));
    finishJobIfReady(task_id);
}

bool TraditionalProcessingController::loadConfig(const QString &file_path,
                                                 Config *config,
                                                 QString *error_message) const
{
    if (config == nullptr) {
        setError(error_message, QStringLiteral("config 输出指针为空"));
        return false;
    }

    ConfigFileDocument document;
    if (!document.loadFromFile(file_path, error_message)) {
        return false;
    }

    QString content;
    if (!readTextFile(file_path, &content, error_message)) {
        return false;
    }

    const QMap<int, QMap<QString, QString>> stream_fields_by_index = parseStreamFormatFields(content);
    Config parsed_config;
    parsed_config.source_path = file_path;
    parsed_config.loaded_at = QDateTime::currentDateTime();

    const QList<ConfigParameterRecord> records = document.parameters();
    const QList<int> stream_indexes = document.streamIndexes();
    for (int stream_index : stream_indexes) {
        const QMap<QString, QString> fields = stream_fields_by_index.value(stream_index);
        if (fields.isEmpty()) {
            setError(error_message, QStringLiteral("配置文件缺少流 %1 的格式描述段").arg(stream_index));
            return false;
        }

        StreamFormatDescription stream;
        stream.stream_index = stream_index;
        stream.stream_id = streamIdForIndex(stream_index);
        stream.format_section_title = ConfigTemplateKeys::streamSectionTitle(stream_index);
        stream.parameter_section_title = ConfigTemplateKeys::parameterSectionTitle(stream_index);
        stream.aircraft_name = fields.value(QStringLiteral("aircraft_name"));
        stream.test_name = fields.value(QStringLiteral("test_name"));
        if (!readIntField(fields, QStringLiteral("parameter_number"), 0, false,
                          &stream.parameter_number, error_message) ||
            !readDoubleField(fields, QStringLiteral("bit_rate"), 0.0,
                             &stream.bit_rate_bps, error_message) ||
            !readDoubleField(fields, QStringLiteral("loop_width"), 0.1,
                             &stream.loop_width, error_message) ||
            !readIntField(fields, QStringLiteral("word_length"), 0, true,
                          &stream.word_length_bits, error_message) ||
            !readIntField(fields, QStringLiteral("frame_length"), 0, true,
                          &stream.frame_length_words, error_message) ||
            !readIntField(fields, QStringLiteral("subframe_deepth"), 1, false,
                          &stream.subframe_depth, error_message) ||
            !readDoubleField(fields, QStringLiteral("major_frame_rate"), 0.0,
                             &stream.major_frame_rate_hz, error_message) ||
            !readIntField(fields, QStringLiteral("sync_word_length"), 0, true,
                          &stream.sync_word_length_bits, error_message)) {
            return false;
        }

        stream.pcm_code = fields.value(QStringLiteral("pcm_code"), stream.pcm_code);
        stream.parity = fields.value(QStringLiteral("parity"), stream.parity);
        stream.sync_pattern_text = fields.value(QStringLiteral("sync_pattern"));
        if (!parseSyncPattern(stream.sync_pattern_text,
                              stream.sync_word_length_bits,
                              stream.word_length_bits,
                              &stream.sync_pattern_bytes,
                              error_message)) {
            return false;
        }

        for (const ConfigParameterRecord &record : records) {
            if (record.stream_index == stream_index) {
                stream.parameters.append(parameterFromRecord(record));
            }
        }

        parsed_config.streams.append(stream);
    }

    if (!ConfigParser::rebuildLayoutTables(&parsed_config, error_message)) {
        return false;
    }

    *config = parsed_config;
    return true;
}

bool TraditionalProcessingController::buildStreamDefinitions(const Config &config,
                                                             QList<StreamDefinition> *streams,
                                                             QString *error_message) const
{
    if (streams == nullptr) {
        setError(error_message, QStringLiteral("streams 输出指针为空"));
        return false;
    }

    QList<StreamDefinition> generated_streams;
    for (const StreamFormatDescription &stream : config.streams) {
        const qint64 frame_bits = stream.wordsPerMajorFrame() * stream.word_length_bits;
        if (frame_bits <= 0) {
            setError(error_message, QStringLiteral("流 %1 的帧长度无效").arg(stream.stream_index));
            return false;
        }

        if (frame_bits % 8 != 0) {
            setError(error_message,
                     QStringLiteral("流 %1 的帧长度不是字节对齐，当前最小链路无法读取")
                         .arg(stream.stream_index));
            return false;
        }

        StreamDefinition definition;
        definition.stream_index = stream.stream_index;
        definition.stream_id = stream.stream_id;
        definition.sync_word = stream.sync_pattern_bytes;
        definition.frame_length_bytes = static_cast<qsizetype>(frame_bits / 8);
        definition.samples_per_frame = static_cast<quint64>(qMax<qint64>(1, stream.wordsPerMajorFrame()));

        if (definition.sync_word.isEmpty()) {
            setError(error_message, QStringLiteral("流 %1 的同步字为空").arg(stream.stream_index));
            return false;
        }

        if (definition.frame_length_bytes < definition.sync_word.size()) {
            setError(error_message,
                     QStringLiteral("流 %1 的帧长度短于同步字长度").arg(stream.stream_index));
            return false;
        }

        generated_streams.append(definition);
    }

    if (generated_streams.isEmpty()) {
        setError(error_message, QStringLiteral("配置文件没有可用数据流"));
        return false;
    }

    *streams = generated_streams;
    return true;
}

bool TraditionalProcessingController::applyParameterSelection(
    const TraditionalProcessingOptions &options,
    const Config &config,
    TaskDefinition *definition,
    QHash<QString, QSet<QString>> *selected_parameter_ids_by_stream,
    QString *error_message) const
{
    if (definition == nullptr || selected_parameter_ids_by_stream == nullptr) {
        setError(error_message, QStringLiteral("参数选择输出指针为空"));
        return false;
    }

    QStringList requested_parameters;
    for (const QString &parameter_name : options.parameter_names) {
        const QString normalized = normalizedRequestedParameter(parameter_name);
        if (normalized.isEmpty() || normalized.compare(QStringLiteral("TIME"), Qt::CaseInsensitive) == 0) {
            continue;
        }
        requested_parameters.append(normalized);
    }

    if (requested_parameters.isEmpty()) {
        setError(error_message, QStringLiteral("参数组中没有除 TIME 以外的可处理参数"));
        return false;
    }

    QSet<QString> matched_requested_parameters;
    QMap<int, ParameterGroupSelection> groups_by_stream;
    QHash<QString, QSet<QString>> selected_by_stream;

    for (const StreamLayoutTable &layout_table : config.layout_tables) {
        QSet<QString> selected_ids;
        for (const StreamLayoutEntry &entry : layout_table.entries) {
            for (const QString &requested : requested_parameters) {
                if (!matchesRequestedParameter(requested, entry)) {
                    continue;
                }

                selected_ids.insert(entry.parameter_id);
                matched_requested_parameters.insert(requested.toLower());
            }
        }

        if (selected_ids.isEmpty()) {
            continue;
        }

        ParameterGroupSelection group;
        group.stream_index = layout_table.stream_index;
        group.group_id = QStringLiteral("traditional_stream_%1").arg(layout_table.stream_index);
        group.display_name = QStringLiteral("stream_%1").arg(layout_table.stream_index);
        QList<QString> sorted_ids = selected_ids.values();
        std::sort(sorted_ids.begin(), sorted_ids.end());
        group.parameter_ids = sorted_ids;
        groups_by_stream.insert(layout_table.stream_index, group);
        selected_by_stream.insert(layout_table.stream_id, selected_ids);
    }

    QStringList missing_parameters;
    for (const QString &requested : requested_parameters) {
        if (!matched_requested_parameters.contains(requested.toLower())) {
            missing_parameters.append(requested);
        }
    }

    if (!missing_parameters.isEmpty()) {
        setError(error_message,
                 QStringLiteral("配置文件中未找到参数：%1").arg(missing_parameters.join(QStringLiteral(", "))));
        return false;
    }

    if (groups_by_stream.isEmpty()) {
        setError(error_message, QStringLiteral("未生成任何后端参数组"));
        return false;
    }

    definition->parameter_groups = groups_by_stream.values();
    *selected_parameter_ids_by_stream = selected_by_stream;
    return true;
}

void TraditionalProcessingController::handleTaskQueuesReady(const TaskDefinition &definition,
                                                            const QList<StreamDefinition> &streams,
                                                            const StreamFrameQueues &frame_queues)
{
    Q_UNUSED(streams)

    ActiveJob *job = active_jobs_.value(definition.task_id, nullptr);
    if (job == nullptr) {
        return;
    }

    FrameProcessorTask processor_task;
    processor_task.task_id = definition.task_id;
    QSet<QString> active_stream_ids;
    for (const StreamDefinition &stream : streams) {
        active_stream_ids.insert(stream.stream_id);
    }
    for (const StreamLayoutTable &layout_table : job->layout_tables) {
        if (active_stream_ids.contains(layout_table.stream_id)) {
            processor_task.layout_tables.append(layout_table);
        }
    }
    processor_task.frame_queues = frame_queues;
    processor_task.selected_parameter_ids_by_stream = job->selected_parameter_ids_by_stream;

    const int stream_count = qMax(1, processor_task.layout_tables.size());
    const int worker_budget = qMax(1, QThread::idealThreadCount() - 1);
    FrameProcessorOptions processor_options;
    processor_options.consumer_count_per_stream = qBound(1, worker_budget / stream_count, 4);
    processor_options.wait_timeout_ms = 20;
    processor_options.value_batch_size = 1024;
    processor_options.progress_interval_frames = 4096;
    processor_options.enable_result_queues = false;
    processor_options.emit_individual_values = false;

    FrameProcessor *processor = new FrameProcessor(processor_task, processor_options, this);
    job->processor = processor;
    connect(processor,
            &FrameProcessor::parameterBatchReady,
            this,
            &TraditionalProcessingController::handleParameterBatchReady,
            Qt::BlockingQueuedConnection);
    connect(processor,
            &FrameProcessor::failed,
            this,
            &TraditionalProcessingController::handleProcessorFailed);
    connect(processor,
            &FrameProcessor::progressChanged,
            this,
            &TraditionalProcessingController::handleProcessorProgress);
    connect(processor,
            &FrameProcessor::finished,
            this,
            &TraditionalProcessingController::handleProcessorFinished);

    processor->start();
    log(LogLevel::Info, definition.task_id, QStringLiteral("帧解算器已连接到任务队列"));
}

void TraditionalProcessingController::handleParameterReady(const ParameterValue &value)
{
    ActiveJob *job = active_jobs_.value(value.task_id, nullptr);
    if (job == nullptr || job->failed || job->stopped) {
        return;
    }

    QString error_message;
    if (!writer_.appendValue(value, &error_message)) {
        failJob(value.task_id, error_message);
    }
}

void TraditionalProcessingController::handleParameterBatchReady(const QList<ParameterValue> &values)
{
    if (values.isEmpty()) {
        return;
    }

    const QString task_id = values.first().task_id;
    ActiveJob *job = active_jobs_.value(task_id, nullptr);
    if (job == nullptr || job->failed || job->stopped) {
        return;
    }

    QString error_message;
    if (!writer_.appendValues(values, &error_message)) {
        failJob(task_id, error_message);
    }
}

void TraditionalProcessingController::handleSchedulerTaskProgress(const QString &task_id,
                                                                  qint64 processed_bytes,
                                                                  qint64 total_bytes)
{
    ActiveJob *job = active_jobs_.value(task_id, nullptr);
    if (job == nullptr || total_bytes < 0) {
        return;
    }

    int progress_percent = 0;
    if (total_bytes > 0) {
        progress_percent = static_cast<int>((processed_bytes * 90) / total_bytes);
    }
    progress_percent = qBound(0, progress_percent, 90);
    job->last_progress_percent = qMax(job->last_progress_percent, progress_percent);

    emit taskProgressChanged(
        publicTaskId(task_id),
        aggregateProgress(job->last_progress_percent),
        rangeDetail(QStringLiteral("已读取 %1 / %2 字节").arg(processed_bytes).arg(total_bytes)));
}

void TraditionalProcessingController::handleSchedulerTaskFailed(const QString &task_id, const QString &reason)
{
    failJob(task_id, reason, false);
}

void TraditionalProcessingController::handleSchedulerTaskStopped(const QString &task_id)
{
    ActiveJob *job = active_jobs_.value(task_id, nullptr);
    if (job == nullptr) {
        return;
    }

    job->stopped = true;
    finishJobIfReady(task_id);
}

void TraditionalProcessingController::handleSchedulerTaskFinished(const QString &task_id,
                                                                  quint64 produced_frame_count)
{
    ActiveJob *job = active_jobs_.value(task_id, nullptr);
    if (job == nullptr) {
        return;
    }

    job->reader_finished = true;
    job->produced_frame_count = produced_frame_count;
    if (!job->failed && !job->stopped) {
        job->last_progress_percent = qMax(job->last_progress_percent, 90);
        emit taskProgressChanged(publicTaskId(task_id),
                                 aggregateProgress(job->last_progress_percent),
                                 rangeDetail(QStringLiteral("原始帧读取完成")));
    }
    finishJobIfReady(task_id);
}

void TraditionalProcessingController::handleProcessorFailed(const QString &task_id, const QString &reason)
{
    failJob(task_id, reason);
}

void TraditionalProcessingController::handleProcessorProgress(const QString &task_id,
                                                              int stream_index,
                                                              const QString &stream_id,
                                                              quint64 processed_frame_count,
                                                              quint64 produced_value_count)
{
    Q_UNUSED(stream_id)

    ActiveJob *job = active_jobs_.value(task_id, nullptr);
    if (job == nullptr || job->failed || job->stopped) {
        return;
    }

    const int progress_percent = qMax(job->last_progress_percent, 90);
    job->last_progress_percent = qMin(progress_percent, 95);
    emit taskProgressChanged(
        publicTaskId(task_id),
        aggregateProgress(job->last_progress_percent),
        rangeDetail(QStringLiteral("流 %1 已解算 %2 帧，输出 %3 个值")
            .arg(stream_index)
            .arg(processed_frame_count)
            .arg(produced_value_count)));
}

void TraditionalProcessingController::handleProcessorFinished(const QString &task_id,
                                                              quint64 processed_frame_count,
                                                              quint64 produced_value_count)
{
    ActiveJob *job = active_jobs_.value(task_id, nullptr);
    if (job == nullptr) {
        return;
    }

    job->processor_finished = true;
    job->processed_frame_count = processed_frame_count;
    job->produced_value_count = produced_value_count;
    if (!job->failed && !job->stopped) {
        job->last_progress_percent = qMax(job->last_progress_percent, 95);
        emit taskProgressChanged(publicTaskId(task_id),
                                 aggregateProgress(job->last_progress_percent),
                                 rangeDetail(QStringLiteral("帧解算完成")));
    }
    finishJobIfReady(task_id);
}

void TraditionalProcessingController::handleFastTaskStarted(const QString &task_id)
{
    log(LogLevel::Info, task_id, QStringLiteral("固定帧高速任务开始读取原始数据"));
    emit taskStarted(publicTaskId(task_id));
}

void TraditionalProcessingController::handleFastTaskProgress(const QString &task_id,
                                                             qint64 processed_bytes,
                                                             qint64 total_bytes)
{
    ActiveJob *job = active_jobs_.value(task_id, nullptr);
    if (job == nullptr || total_bytes <= 0) {
        return;
    }

    const int progress_percent = qBound(0, static_cast<int>((processed_bytes * 98) / total_bytes), 98);
    job->last_progress_percent = qMax(job->last_progress_percent, progress_percent);
    emit taskProgressChanged(
        publicTaskId(task_id),
        aggregateProgress(job->last_progress_percent),
        rangeDetail(QStringLiteral("固定帧高速处理 %1 / %2 字节").arg(processed_bytes).arg(total_bytes)));
}

void TraditionalProcessingController::handleFastTaskFailed(const QString &task_id, const QString &reason)
{
    ActiveJob *job = active_jobs_.value(task_id, nullptr);
    if (job != nullptr) {
        job->failed = true;
        job->failure_reason = reason;
    }

    const QString request_id = publicTaskId(task_id);
    QString public_reason = rangeDetail(reason);
    if (!completed_output_paths_.isEmpty()) {
        public_reason += QStringLiteral("\n已生成文件：\n%1").arg(completed_output_paths_.join(QLatin1Char('\n')));
    }
    log(LogLevel::Error, task_id, reason);
    emit taskFailed(request_id, public_reason);
    cleanupJob(task_id);
    resetBatch();
}

void TraditionalProcessingController::handleFastTaskStopped(const QString &task_id)
{
    ActiveJob *job = active_jobs_.value(task_id, nullptr);
    if (job != nullptr) {
        job->stopped = true;
    }

    const QString request_id = publicTaskId(task_id);
    log(LogLevel::Warning, task_id, QStringLiteral("固定帧高速任务已中断"));
    emit taskStopped(request_id);
    cleanupJob(task_id);
    resetBatch();
}

void TraditionalProcessingController::handleFastTaskFinished(const QString &task_id,
                                                             const QString &output_path,
                                                             quint64 processed_frame_count,
                                                             quint64 produced_value_count)
{
    ActiveJob *job = active_jobs_.value(task_id, nullptr);
    if (job != nullptr) {
        job->reader_finished = true;
        job->processor_finished = true;
        job->processed_frame_count = processed_frame_count;
        job->produced_frame_count = processed_frame_count;
        job->produced_value_count = produced_value_count;
        job->last_progress_percent = 100;
    }

    emit taskProgressChanged(publicTaskId(task_id), aggregateProgress(100), rangeDetail(QStringLiteral("固定帧高速结果写出完成")));
    completeCurrentRange(task_id, output_path);
}

void TraditionalProcessingController::failJob(const QString &task_id, const QString &reason, bool stop_scheduler)
{
    ActiveJob *job = active_jobs_.value(task_id, nullptr);
    if (job == nullptr) {
        emit taskFailed(publicTaskId(task_id), rangeDetail(reason));
        resetBatch();
        return;
    }
    QString public_reason = rangeDetail(reason);
    if (!completed_output_paths_.isEmpty()) {
        public_reason += QStringLiteral("\n已生成文件：\n%1").arg(completed_output_paths_.join(QLatin1Char('\n')));
    }

    if (!job->failed) {
        job->failed = true;
        job->failure_reason = reason;
        log(LogLevel::Error, task_id, reason);
    }

    if (!job->failure_emitted) {
        job->failure_emitted = true;
        emit taskFailed(publicTaskId(task_id), public_reason);
    }

    if (stop_scheduler && scheduler_.hasTask(task_id)) {
        scheduler_.requestStop(task_id);
    }
    if (job->processor != nullptr) {
        job->processor->requestStop();
    }
    if (job->fast_processor != nullptr) {
        job->fast_processor->requestStop();
    }
    finishJobIfReady(task_id);
}

void TraditionalProcessingController::finishJobIfReady(const QString &task_id)
{
    ActiveJob *job = active_jobs_.value(task_id, nullptr);
    if (job == nullptr || job->finalized) {
        return;
    }

    const bool processor_done = job->processor == nullptr || job->processor_finished;
    if (!job->reader_finished || !processor_done) {
        return;
    }

    job->finalized = true;
    if (job->stopped && !job->failed) {
        const QString request_id = publicTaskId(task_id);
        writer_.abortTask(task_id);
        log(LogLevel::Warning, task_id, QStringLiteral("传统处理任务已中断"));
        emit taskStopped(request_id);
        cleanupJob(task_id);
        resetBatch();
        return;
    }

    if (job->failed) {
        writer_.abortTask(task_id);
        cleanupJob(task_id);
        resetBatch();
        return;
    }

    QString output_path;
    QString error_message;
    if (!writer_.finishTask(task_id, &output_path, &error_message)) {
        job->failed = true;
        if (!job->failure_emitted) {
            job->failure_emitted = true;
            QString public_reason = rangeDetail(error_message);
            if (!completed_output_paths_.isEmpty()) {
                public_reason += QStringLiteral("\n已生成文件：\n%1").arg(completed_output_paths_.join(QLatin1Char('\n')));
            }
            emit taskFailed(publicTaskId(task_id), public_reason);
        }
        cleanupJob(task_id);
        resetBatch();
        return;
    }

    emit taskProgressChanged(publicTaskId(task_id), aggregateProgress(100), rangeDetail(QStringLiteral("结果写出完成")));
    completeCurrentRange(task_id, output_path);
}

void TraditionalProcessingController::cleanupJob(const QString &task_id)
{
    ActiveJob *job = active_jobs_.take(task_id);
    if (job == nullptr) {
        return;
    }

    scheduler_.unregisterConfigStreams(job->definition.config_file_path);
    if (job->processor != nullptr) {
        job->processor->deleteLater();
        job->processor = nullptr;
    }
    if (job->fast_processor != nullptr) {
        job->fast_processor->deleteLater();
        job->fast_processor = nullptr;
    }
    if (job->fast_thread != nullptr) {
        job->fast_thread->quit();
        job->fast_thread->wait();
        delete job->fast_thread;
        job->fast_thread = nullptr;
    }
    if (current_task_id_ == task_id) {
        current_task_id_.clear();
    }
    delete job;
}

void TraditionalProcessingController::completeCurrentRange(const QString &task_id,
                                                           const QString &output_path)
{
    const QString request_id = publicTaskId(task_id);
    const int range_index = current_range_index_ + 1;
    const int range_count = batch_ranges_.size();
    completed_output_paths_.append(output_path);

    emit taskFinished(task_id, output_path);
    emit rangeFinished(request_id, range_index, range_count, output_path);
    cleanupJob(task_id);
    QTimer::singleShot(0, this, &TraditionalProcessingController::startNextRange);
}

void TraditionalProcessingController::resetBatch()
{
    batch_options_ = TraditionalProcessingOptions();
    batch_ranges_.clear();
    batch_request_id_.clear();
    completed_output_paths_.clear();
    current_range_index_ = -1;
    batch_active_ = false;
    batch_stop_requested_ = false;
}

QString TraditionalProcessingController::publicTaskId(const QString &task_id) const
{
    return batch_active_ && !batch_request_id_.isEmpty() ? batch_request_id_ : task_id;
}

int TraditionalProcessingController::aggregateProgress(int local_progress_percent) const
{
    if (!batch_active_ || batch_ranges_.isEmpty()) {
        return qBound(0, local_progress_percent, 100);
    }
    const int completed_ranges = qMax(0, current_range_index_);
    return qBound(0,
                  ((completed_ranges * 100) + qBound(0, local_progress_percent, 100)) /
                      batch_ranges_.size(),
                  100);
}

QString TraditionalProcessingController::rangeDetail(const QString &detail) const
{
    if (!batch_active_ || batch_ranges_.size() <= 1) {
        return detail;
    }
    return QStringLiteral("第 %1/%2 段：%3")
        .arg(current_range_index_ + 1)
        .arg(batch_ranges_.size())
        .arg(detail);
}

QString TraditionalProcessingController::failureDetail(const QString &detail) const
{
    QString message = rangeDetail(detail);
    if (!completed_output_paths_.isEmpty()) {
        message += QStringLiteral("\n已生成文件：\n%1")
                       .arg(completed_output_paths_.join(QLatin1Char('\n')));
    }
    return message;
}

void TraditionalProcessingController::log(LogLevel level,
                                          const QString &task_id,
                                          const QString &message) const
{
    Logger::instance().log(level,
                           QStringLiteral("TraditionalProcessing"),
                           task_id,
                           QString(),
                           message);
}
