#include "config_text_codec.h"
#include "config_file_document.h"

#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QRegularExpression>
#include <QSaveFile>
#include <QSet>
#include <QTextStream>

#include <algorithm>

namespace {

QString textFromBytes(const QByteArray &content_bytes)
{
    return decodeConfigText(content_bytes);
}

void setError(QString *error_message, const QString &message)
{
    if (error_message != nullptr) {
        *error_message = message;
    }
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
    const QRegularExpression expression(QStringLiteral("^the_parameter_description_of_stream[_ ](\\d+)$"));
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
    QString normalized_line = normalizedConfigLine(line);
    const QString trimmed = line.trimmed();
    const int description_separator = trimmed.indexOf(QLatin1Char(':'));
    if (description_separator >= 0 &&
        trimmed.left(description_separator).trimmed().compare(QStringLiteral("Description"),
                                                              Qt::CaseInsensitive) == 0) {
        normalized_line = trimmed;
    }
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

QString stripParameterToken(const QString &token)
{
    QString normalized = token.trimmed();
    normalized.remove(QChar(0xfeff));
    if (normalized.size() >= 2 &&
        ((normalized.startsWith(QLatin1Char('"')) && normalized.endsWith(QLatin1Char('"'))) ||
         (normalized.startsWith(QLatin1Char('\'')) && normalized.endsWith(QLatin1Char('\''))))) {
        normalized = normalized.mid(1, normalized.size() - 2).trimmed();
    }

    return normalized;
}

bool isIgnorableLine(const QString &line)
{
    QString trimmed = line.trimmed();
    trimmed.remove(QChar(0xfeff));
    return trimmed.isEmpty() || trimmed.startsWith(QLatin1Char('#'));
}

QString keyValueLine(const QString &key, const QString &value)
{
    return QStringLiteral("%1: %2").arg(key, value);
}

/** @brief 按最长字段名补最少前导空格以对齐冒号；冒号紧跟字段名，流格式、参数描述段标题及字段值保留原样。 */
QString alignedConfigText(QStringList lines, const QString &newline)
{
    int key_width = 0;
    auto is_field = [](const QString &line) {
        const QString trimmed = line.trimmed();
        return line.contains(QLatin1Char(':')) && !trimmed.startsWith(QLatin1Char('#')) &&
            !trimmed.startsWith(QStringLiteral("The format description of stream")) &&
            !trimmed.startsWith(QStringLiteral("the_parameter_description_of_stream"));
    };
    for (const QString &line : lines) {
        if (is_field(line)) {
            const QString key = line.left(line.indexOf(QLatin1Char(':'))).trimmed();
            const int syntax_space = key == QStringLiteral("bits_order") ? 1 : 0;
            key_width = qMax(key_width, key.size() + syntax_space);
        }
    }
    for (QString &line : lines) {
        if (!is_field(line)) {
            continue;
        }
        const int separator = line.indexOf(QLatin1Char(':'));
        QString key = line.left(separator).trimmed();
        if (key == QStringLiteral("bits_order")) {
            key += QLatin1Char(' '); // 旧版固定使用 fscanf("%s :%s") 读取此字段。
        }
        const QString value = line.mid(separator + 1);
        line = key.rightJustified(key_width, QLatin1Char(' ')) + QLatin1Char(':') + value;
    }
    return lines.join(newline);
}

QString canonicalPathForCompare(const QString &file_path)
{
    const QFileInfo file_info(file_path);
    if (file_info.exists()) {
        return QDir::cleanPath(file_info.canonicalFilePath());
    }

    return QDir::cleanPath(file_info.absoluteFilePath());
}

} // namespace

QString ConfigParameterRecord::value(const QString &key) const
{
    return fields.value(key).trimmed();
}

void ConfigParameterRecord::setValue(const QString &key, const QString &value)
{
    fields.insert(key, value.trimmed());
    if (key == QStringLiteral("serial_no.")) {
        bool ok = false;
        const int parsed_serial = value.trimmed().toInt(&ok);
        serial_no = ok ? parsed_serial : 0;
    }
}

QString ConfigParameterRecord::idNo() const
{
    return value(QStringLiteral("id_no."));
}

QString ConfigParameterRecord::name() const
{
    return value(QStringLiteral("name"));
}

QString ConfigParameterRecord::searchableText() const
{
    return QStringList{
        QString::number(stream_index),
        value(QStringLiteral("serial_no.")),
        idNo(),
        name(),
        value(QStringLiteral("unit")),
        value(QStringLiteral("cal_type")),
        value(QStringLiteral("code_type")),
        value(QStringLiteral("rate")),
        value(QStringLiteral("Description")),
    }.join(QLatin1Char(' '));
}

bool ConfigFileDocument::loadFromFile(const QString &file_path, QString *error_message)
{
    QFile file(file_path);
    if (!file.open(QIODevice::ReadOnly | QIODevice::Text)) {
        setError(error_message, QStringLiteral("无法打开配置文件：%1").arg(file.errorString()));
        return false;
    }

    return loadFromText(textFromBytes(file.readAll()), file_path, error_message);
}

bool ConfigFileDocument::loadFromText(const QString &content,
                                      const QString &source_path,
                                      QString *error_message)
{
    QString normalized_content = content;
    newline_ = normalized_content.contains(QStringLiteral("\r\n"))
        ? QStringLiteral("\r\n")
        : QStringLiteral("\n");
    normalized_content.replace(QStringLiteral("\r\n"), QStringLiteral("\n"));
    normalized_content.replace(QLatin1Char('\r'), QLatin1Char('\n'));

    source_path_ = source_path;
    lines_ = normalized_content.split(QLatin1Char('\n'));
    sections_.clear();
    parameters_.clear();
    stream_indexes_.clear();
    has_content_ = true;
    dirty_ = false;

    if (!parse(error_message)) {
        has_content_ = false;
        lines_.clear();
        sections_.clear();
        parameters_.clear();
        stream_indexes_.clear();
        return false;
    }

    return true;
}

bool ConfigFileDocument::saveAs(const QString &file_path, QString *error_message)
{
    if (!hasContent()) {
        setError(error_message, QStringLiteral("没有已加载的配置文件。"));
        return false;
    }

    if (!source_path_.trimmed().isEmpty() &&
        canonicalPathForCompare(file_path) == canonicalPathForCompare(source_path_)) {
        setError(error_message, QStringLiteral("导出路径不能覆盖当前导入的原配置文件。"));
        return false;
    }

    QString local_error;
    const QString content = toText(&local_error);
    if (!local_error.isEmpty()) {
        setError(error_message, local_error);
        return false;
    }

    QSaveFile file(file_path);
    if (!file.open(QIODevice::WriteOnly | QIODevice::Text)) {
        setError(error_message, QStringLiteral("无法创建导出文件：%1").arg(file.errorString()));
        return false;
    }

    QTextStream stream(&file);
#if QT_VERSION < QT_VERSION_CHECK(6, 0, 0)
    stream.setCodec("UTF-8");
#endif
    stream << content;
    if (!file.commit()) {
        setError(error_message, QStringLiteral("无法写入导出文件：%1").arg(file.errorString()));
        return false;
    }

    if (!loadFromText(content, file_path, error_message)) {
        return false;
    }
    dirty_ = false;
    return true;
}

QString ConfigFileDocument::toText(QString *error_message) const
{
    QString local_error;
    if (!validateAll(&local_error)) {
        setError(error_message, local_error);
        return QString();
    }

    QMap<int, SectionRange> section_by_title_line;
    for (const SectionRange &section : sections_) {
        section_by_title_line.insert(section.title_line, section);
    }

    QStringList output_lines;
    int current_stream_index = 0;
    int current_stream_end = -1;
    for (int line_index = 0; line_index < lines_.size(); ++line_index) {
        if (line_index >= current_stream_end) {
            current_stream_index = 0;
            current_stream_end = -1;
        }

        const auto section_iterator = section_by_title_line.constFind(line_index);
        if (section_iterator != section_by_title_line.constEnd()) {
            const SectionRange section = section_iterator.value();
            output_lines.append(section.type == SectionRange::Type::ParameterDescription
                ? ConfigTemplateKeys::parameterSectionTitle(section.stream_index) + QLatin1Char(':')
                : lines_.at(line_index));
            if (section.type == SectionRange::Type::ParameterDescription) {
                output_lines.append(serializedParameterLines(section.stream_index));
                line_index = section.content_end - 1;
                continue;
            }

            current_stream_index = section.stream_index;
            current_stream_end = section.content_end;
            continue;
        }

        QString key;
        QString value;
        if (current_stream_index > 0 &&
            splitConfigKeyValue(lines_.at(line_index), &key, &value) &&
            key == QStringLiteral("parameter_number")) {
            output_lines.append(keyValueLine(key, QString::number(parameterCountForStream(current_stream_index))));
            continue;
        }

        output_lines.append(lines_.at(line_index));
    }

    if (error_message != nullptr) {
        error_message->clear();
    }
    return alignedConfigText(output_lines, newline_);
}

bool ConfigFileDocument::hasContent() const
{
    return has_content_;
}

bool ConfigFileDocument::isDirty() const
{
    return dirty_;
}

QString ConfigFileDocument::sourcePath() const
{
    return source_path_;
}

QList<ConfigParameterRecord> ConfigFileDocument::parameters() const
{
    return parameters_;
}

QStringList ConfigFileDocument::parameterNames() const
{
    QStringList names;
    for (const ConfigParameterRecord &parameter : parameters_) {
        const QString parameter_name = parameter.name();
        if (parameter_name.isEmpty()) {
            continue;
        }

        bool exists = false;
        for (const QString &existing_name : names) {
            if (existing_name.compare(parameter_name, Qt::CaseInsensitive) == 0) {
                exists = true;
                break;
            }
        }
        if (!exists) {
            names.append(parameter_name);
        }
    }

    return names;
}

QList<int> ConfigFileDocument::streamIndexes() const
{
    return stream_indexes_;
}

int ConfigFileDocument::nextSerialNo(int stream_index) const
{
    int max_serial_no = 0;
    for (const ConfigParameterRecord &parameter : parameters_) {
        if (parameter.stream_index == stream_index) {
            max_serial_no = qMax(max_serial_no, parameter.serial_no);
        }
    }

    return max_serial_no + 1;
}

bool ConfigFileDocument::addParameter(const ConfigParameterRecord &record, QString *error_message)
{
    ConfigParameterRecord normalized = normalizedRecord(record);
    if (normalized.serial_no <= 0) {
        normalized.serial_no = nextSerialNo(normalized.stream_index);
        normalized.setValue(QStringLiteral("serial_no."), QString::number(normalized.serial_no));
    }

    if (!validateRecord(normalized, -1, error_message)) {
        return false;
    }

    int insert_index = parameters_.size();
    for (int index = parameters_.size() - 1; index >= 0; --index) {
        if (parameters_.at(index).stream_index == normalized.stream_index) {
            insert_index = index + 1;
            break;
        }
    }

    parameters_.insert(insert_index, normalized);
    dirty_ = true;
    return true;
}

bool ConfigFileDocument::updateParameter(int parameter_index,
                                         const ConfigParameterRecord &record,
                                         QString *error_message)
{
    if (parameter_index < 0 || parameter_index >= parameters_.size()) {
        setError(error_message, QStringLiteral("参数索引无效。"));
        return false;
    }

    ConfigParameterRecord normalized = normalizedRecord(record);
    if (normalized.stream_index != parameters_.at(parameter_index).stream_index) {
        setError(error_message, QStringLiteral("编辑参数时不能修改所属流号。"));
        return false;
    }

    if (!validateRecord(normalized, parameter_index, error_message)) {
        return false;
    }

    parameters_[parameter_index] = normalized;
    dirty_ = true;
    return true;
}

bool ConfigFileDocument::removeParameter(int parameter_index, QString *error_message)
{
    if (parameter_index < 0 || parameter_index >= parameters_.size()) {
        setError(error_message, QStringLiteral("参数索引无效。"));
        return false;
    }

    parameters_.removeAt(parameter_index);
    dirty_ = true;
    return true;
}

bool ConfigFileDocument::parse(QString *error_message)
{
    struct OpenSection
    {
        bool active = false;
        SectionRange range;
    };

    OpenSection open_section;
    auto close_section = [this, &open_section](int content_end) {
        if (!open_section.active) {
            return;
        }

        open_section.range.content_end = content_end;
        sections_.append(open_section.range);
        open_section.active = false;
    };

    QSet<int> stream_index_set;
    for (int line_index = 0; line_index < lines_.size(); ++line_index) {
        const QString line = lines_.at(line_index).trimmed();
        const QString section_title = normalizedSectionTitle(line);
        if (section_title == ConfigTemplateKeys::fileSignature()) {
            continue;
        }

        if (section_title == ConfigTemplateKeys::headerSection()) {
            close_section(line_index);
            continue;
        }

        int parsed_stream_index = 0;
        if (parseStreamFormatSectionLine(line, &parsed_stream_index)) {
            close_section(line_index);
            stream_index_set.insert(parsed_stream_index);
            open_section.active = true;
            open_section.range.type = SectionRange::Type::StreamFormat;
            open_section.range.stream_index = parsed_stream_index;
            open_section.range.title_line = line_index;
            open_section.range.content_begin = line_index + 1;
            open_section.range.content_end = lines_.size();
            continue;
        }

        if (parseParameterSectionLine(line, &parsed_stream_index)) {
            close_section(line_index);
            stream_index_set.insert(parsed_stream_index);
            open_section.active = true;
            open_section.range.type = SectionRange::Type::ParameterDescription;
            open_section.range.stream_index = parsed_stream_index;
            open_section.range.title_line = line_index;
            open_section.range.content_begin = line_index + 1;
            open_section.range.content_end = lines_.size();
            continue;
        }
    }
    close_section(lines_.size());

    if (sections_.isEmpty()) {
        setError(error_message, QStringLiteral("未识别到流格式段或参数描述段。"));
        return false;
    }

    stream_indexes_ = stream_index_set.values();
    std::sort(stream_indexes_.begin(), stream_indexes_.end());

    for (const SectionRange &section : sections_) {
        if (section.type != SectionRange::Type::ParameterDescription) {
            continue;
        }

        if (!parseParameterSection(section, error_message)) {
            return false;
        }
    }

    return validateAll(error_message);
}

bool ConfigFileDocument::parseParameterSection(const SectionRange &section, QString *error_message)
{
    QMap<QString, QString> fields;

    auto flush_parameter = [this, &fields, &section]() {
        if (fields.isEmpty()) {
            return;
        }

        ConfigParameterRecord record;
        record.stream_index = section.stream_index;
        record.fields = fields;
        bool ok = false;
        const int parsed_serial = record.value(QStringLiteral("serial_no.")).toInt(&ok);
        record.serial_no = ok ? parsed_serial : 0;
        parameters_.append(normalizedRecord(record));
        fields.clear();
    };

    for (int line_index = section.content_begin; line_index < section.content_end; ++line_index) {
        const QString line = lines_.at(line_index);
        if (isIgnorableLine(line)) {
            continue;
        }

        QString key;
        QString value;
        if (!splitConfigKeyValue(line, &key, &value)) {
            setError(error_message,
                     QStringLiteral("配置文件第 %1 行不是合法的 key: value 格式。").arg(line_index + 1));
            return false;
        }

        if (key == QStringLiteral("description")) {
            key = QStringLiteral("Description");
        }
        if (!ConfigTemplateKeys::isParameterKey(key)) {
            setError(error_message,
                     QStringLiteral("配置文件第 %1 行包含非法参数字段 '%2'。").arg(line_index + 1).arg(key));
            return false;
        }

        if (key == QStringLiteral("serial_no.") && !fields.isEmpty()) {
            flush_parameter();
        }

        if (key == QStringLiteral("code,phy") && fields.contains(key)) {
            fields[key] += QStringLiteral(";") + stripParameterToken(value);
        } else {
            if (fields.contains(key)) {
                setError(error_message, QStringLiteral("参数字段重复：%1（第 %2 行）").arg(key).arg(line_index + 1));
                return false;
            }
            fields.insert(key, stripParameterToken(value));
        }
    }
    flush_parameter();

    return true;
}

bool ConfigFileDocument::validateRecord(const ConfigParameterRecord &record,
                                        int ignored_parameter_index,
                                        QString *error_message) const
{
    if (record.stream_index <= 0) {
        setError(error_message, QStringLiteral("参数所属流号必须大于 0。"));
        return false;
    }

    if (!stream_indexes_.contains(record.stream_index)) {
        setError(error_message, QStringLiteral("流 %1 不存在。").arg(record.stream_index));
        return false;
    }

    if (!hasParameterSection(record.stream_index)) {
        setError(error_message, QStringLiteral("流 %1 没有参数描述段，无法增删改参数。").arg(record.stream_index));
        return false;
    }

    if (record.serial_no <= 0) {
        setError(error_message, QStringLiteral("serial_no. 必须是大于 0 的整数。"));
        return false;
    }

    if (record.idNo().isEmpty()) {
        setError(error_message, QStringLiteral("id_no. 不能为空。"));
        return false;
    }

    if (record.name().isEmpty()) {
        setError(error_message, QStringLiteral("name 不能为空。"));
        return false;
    }

    for (int index = 0; index < parameters_.size(); ++index) {
        if (index == ignored_parameter_index) {
            continue;
        }

        const ConfigParameterRecord &existing = parameters_.at(index);
        if (existing.stream_index == record.stream_index &&
            existing.idNo().compare(record.idNo(), Qt::CaseInsensitive) == 0) {
            setError(error_message,
                     QStringLiteral("流 %1 中 id_no. '%2' 重复。").arg(record.stream_index).arg(record.idNo()));
            return false;
        }
    }

    return true;
}

bool ConfigFileDocument::validateAll(QString *error_message) const
{
    for (int index = 0; index < parameters_.size(); ++index) {
        if (!validateRecord(parameters_.at(index), index, error_message)) {
            return false;
        }
    }

    return true;
}

ConfigParameterRecord ConfigFileDocument::normalizedRecord(const ConfigParameterRecord &record) const
{
    ConfigParameterRecord normalized = record;
    const QMap<QString, QString> defaults = {
        {QStringLiteral("cal_type"), QStringLiteral("NO")},
        {QStringLiteral("code_type"), QStringLiteral("uint")},
        {QStringLiteral("cal_date"), QString()},
        {QStringLiteral("cal_number"), QString()},
        {QStringLiteral("rate"), QStringLiteral("0")},
        {QStringLiteral("word_interval"), QStringLiteral("0")},
        {QStringLiteral("sign_bit"), QStringLiteral("0")},
        {QStringLiteral("inverse"), QStringLiteral("0")},
        {QStringLiteral("pcm_words"), QStringLiteral("1")},
        {QStringLiteral("frame_no"), QStringLiteral("0,0,0,0,0,0")},
        {QStringLiteral("word_no"), QStringLiteral("0,0,0,0,0,0")},
        {QStringLiteral("source_bit"), QStringLiteral("0,0,0,0,0,0")},
        {QStringLiteral("bit_length"), QStringLiteral("0,0,0,0,0,0")},
        {QStringLiteral("object_bit"), QStringLiteral("0,0,0,0,0,0")},
        {QStringLiteral("Description"), QString()},
    };

    for (const QString &key : ConfigTemplateKeys::parameterKeys()) {
        if (!normalized.fields.contains(key)) {
            normalized.fields.insert(key, defaults.value(key));
        } else {
            normalized.fields.insert(key, normalized.fields.value(key).trimmed());
        }
    }

    bool ok = false;
    const int parsed_serial = normalized.value(QStringLiteral("serial_no.")).toInt(&ok);
    if (ok) {
        normalized.serial_no = parsed_serial;
    } else if (normalized.serial_no > 0) {
        normalized.fields.insert(QStringLiteral("serial_no."), QString::number(normalized.serial_no));
    }

    return normalized;
}

bool ConfigFileDocument::hasParameterSection(int stream_index) const
{
    for (const SectionRange &section : sections_) {
        if (section.type == SectionRange::Type::ParameterDescription &&
            section.stream_index == stream_index) {
            return true;
        }
    }

    return false;
}

int ConfigFileDocument::parameterCountForStream(int stream_index) const
{
    int count = 0;
    for (const ConfigParameterRecord &parameter : parameters_) {
        if (parameter.stream_index == stream_index) {
            ++count;
        }
    }

    return count;
}

QStringList ConfigFileDocument::serializedParameterLines(int stream_index) const
{
    QStringList serialized_lines;
    bool first_record = true;
    for (const ConfigParameterRecord &parameter : parameters_) {
        if (parameter.stream_index != stream_index) {
            continue;
        }

        if (!first_record) {
            serialized_lines.append(QString());
        }
        first_record = false;

        const ConfigParameterRecord normalized = normalizedRecord(parameter);
        for (const QString &key : ConfigTemplateKeys::parameterKeys()) {
            if (key != QStringLiteral("Description")) {
                serialized_lines.append(keyValueLine(key, normalized.value(key)));
            }
        }
        for (const QString &key : ConfigTemplateKeys::calibrationKeys()) {
            if (!normalized.value(key).isEmpty()) {
                if (key == QStringLiteral("code,phy")) {
                    for (const QString &point : normalized.value(key).split(QLatin1Char(';'))) {
                        serialized_lines.append(keyValueLine(key, point.trimmed()));
                    }
                } else {
                    serialized_lines.append(keyValueLine(key, normalized.value(key)));
                }
            }
        }
        serialized_lines.append(keyValueLine(QStringLiteral("Description"),
                                              normalized.value(QStringLiteral("Description"))));
    }

    return serialized_lines;
}
