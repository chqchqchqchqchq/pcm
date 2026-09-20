#include "time_range_parser.h"

#include <QFile>
#include <QRegularExpression>

#include <cmath>

namespace {

void setError(QString *error_message, const QString &message)
{
    if (error_message != nullptr) {
        *error_message = message;
    }
}

bool rangesOverlap(const TaskTimeRange &left, const TaskTimeRange &right)
{
    return left.start_seconds < right.end_seconds && right.start_seconds < left.end_seconds;
}

} // namespace

bool TimeRangeParser::parseFile(const QString &file_path,
                                QList<TaskTimeRange> *ranges,
                                QStringList *warnings,
                                QString *error_message)
{
    QFile file(file_path);
    if (!file.open(QIODevice::ReadOnly | QIODevice::Text)) {
        setError(error_message,
                 QStringLiteral("无法打开时间段文件 '%1'：%2").arg(file_path, file.errorString()));
        return false;
    }

    return parseText(QString::fromUtf8(file.readAll()), ranges, warnings, error_message);
}

bool TimeRangeParser::parseText(const QString &content,
                                QList<TaskTimeRange> *ranges,
                                QStringList *warnings,
                                QString *error_message)
{
    if (ranges == nullptr) {
        setError(error_message, QStringLiteral("时间段输出指针为空"));
        return false;
    }

    ranges->clear();
    if (warnings != nullptr) {
        warnings->clear();
    }

    QString normalized_content = content;
    if (normalized_content.startsWith(QChar(0xfeff))) {
        normalized_content.remove(0, 1);
    }
    normalized_content.replace(QStringLiteral("\r\n"), QStringLiteral("\n"));
    normalized_content.replace(QLatin1Char('\r'), QLatin1Char('\n'));
    const QStringList lines = normalized_content.split(QLatin1Char('\n'));
    const QRegularExpression separator_expression(QStringLiteral("[,\\s]+"));

    for (int line_index = 0; line_index < lines.size(); ++line_index) {
        QString line = lines.at(line_index);
        const int comment_index = line.indexOf(QLatin1Char('#'));
        if (comment_index >= 0) {
            line = line.left(comment_index);
        }
        line = line.trimmed();
        if (line.isEmpty()) {
            continue;
        }

        const QStringList columns = line.split(separator_expression, Qt::SkipEmptyParts);
        if (columns.size() == 2 &&
            columns.at(0).compare(QStringLiteral("start"), Qt::CaseInsensitive) == 0 &&
            columns.at(1).compare(QStringLiteral("end"), Qt::CaseInsensitive) == 0) {
            continue;
        }
        if (columns.size() != 2) {
            setError(error_message,
                     QStringLiteral("时间段文件第 %1 行必须包含起始和结束两列：%2")
                         .arg(line_index + 1)
                         .arg(line));
            return false;
        }

        TaskTimeRange range;
        QString local_error;
        if (!parseTimeValue(columns.at(0), &range.start_seconds, &local_error)) {
            setError(error_message,
                     QStringLiteral("时间段文件第 %1 行起始时间无效：%2")
                         .arg(line_index + 1)
                         .arg(local_error));
            return false;
        }
        if (!parseTimeValue(columns.at(1), &range.end_seconds, &local_error)) {
            setError(error_message,
                     QStringLiteral("时间段文件第 %1 行结束时间无效：%2")
                         .arg(line_index + 1)
                         .arg(local_error));
            return false;
        }
        if (range.start_seconds < 0.0 || range.end_seconds <= range.start_seconds) {
            setError(error_message,
                     QStringLiteral("时间段文件第 %1 行要求 0 <= 起始时间 < 结束时间")
                         .arg(line_index + 1));
            return false;
        }

        for (int existing_index = 0; existing_index < ranges->size(); ++existing_index) {
            if (rangesOverlap(ranges->at(existing_index), range) && warnings != nullptr) {
                warnings->append(
                    QStringLiteral("第 %1 个时间段与第 %2 个时间段重叠，将分别输出重复区间数据。")
                        .arg(existing_index + 1)
                        .arg(ranges->size() + 1));
            }
        }
        ranges->append(range);
    }

    if (ranges->isEmpty()) {
        setError(error_message, QStringLiteral("时间段文件中没有有效时间段"));
        return false;
    }
    return true;
}

bool TimeRangeParser::parseTimeValue(const QString &text, double *seconds, QString *error_message)
{
    if (seconds == nullptr) {
        setError(error_message, QStringLiteral("时间值输出指针为空"));
        return false;
    }

    const QString normalized = text.trimmed();
    bool numeric_ok = false;
    const double numeric_seconds = normalized.toDouble(&numeric_ok);
    if (numeric_ok && std::isfinite(numeric_seconds)) {
        *seconds = numeric_seconds;
        return true;
    }

    const QRegularExpression expression(
        QStringLiteral("^(\\d+):(\\d{2}):(\\d{2})(?:[.:](\\d{1,3}))?$"));
    const QRegularExpressionMatch match = expression.match(normalized);
    if (!match.hasMatch()) {
        setError(error_message,
                 QStringLiteral("'%1' 不是秒数、HH:mm:ss[.zzz] 或旧版 HH:mm:ss:zzz").arg(normalized));
        return false;
    }

    bool hours_ok = false;
    bool minutes_ok = false;
    bool seconds_ok = false;
    const qint64 hours = match.captured(1).toLongLong(&hours_ok);
    const int minutes = match.captured(2).toInt(&minutes_ok);
    const int whole_seconds = match.captured(3).toInt(&seconds_ok);
    if (!hours_ok || !minutes_ok || !seconds_ok || minutes >= 60 || whole_seconds >= 60) {
        setError(error_message, QStringLiteral("'%1' 的分或秒超出有效范围").arg(normalized));
        return false;
    }

    double fractional_seconds = 0.0;
    if (!match.captured(4).isEmpty()) {
        QString milliseconds_text = match.captured(4);
        while (milliseconds_text.size() < 3) {
            milliseconds_text.append(QLatin1Char('0'));
        }
        fractional_seconds = static_cast<double>(milliseconds_text.toInt()) / 1000.0;
    }

    *seconds = static_cast<double>(hours) * 3600.0 +
        static_cast<double>(minutes) * 60.0 +
        static_cast<double>(whole_seconds) + fractional_seconds;
    return true;
}
