#include "processing_options_support.h"
#include "config_text_codec.h"

#include <QCryptographicHash>
#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QRegularExpression>
#include <cmath>

namespace {
/** @brief 返回带错误说明的失败。 */
bool fail(QString *error, const QString &message)
{
    if (error) { *error = message; }
    return false;
}
}

QString ProcessingOptionsSupport::absolutePath(const QString &path)
{
    return path.trimmed().isEmpty() ? QString() : QDir::cleanPath(QFileInfo(path.trimmed()).absoluteFilePath());
}

QString ProcessingOptionsSupport::fingerprint(const QString &path)
{
    QFile file(path);
    if (!file.open(QIODevice::ReadOnly)) { return {}; }
    QCryptographicHash hash(QCryptographicHash::Sha256);
    return hash.addData(&file) ? QString::fromLatin1(hash.result().toHex()) : QString();
}

void ProcessingOptionsSupport::normalize(TraditionalProcessingOptions *options)
{
    auto &definition = options->task_definition;
    definition.config_file_path = absolutePath(definition.config_file_path);
    definition.raw_file_path = absolutePath(definition.raw_file_path);
    options->parameter_group_file_path = absolutePath(options->parameter_group_file_path);
    definition.time_range = options->time_ranges.isEmpty() ? TaskTimeRange() : options->time_ranges.first();
    ParameterGroupSelection group;
    group.group_id = QStringLiteral("snapshot_parameters");
    group.display_name = QFileInfo(options->parameter_group_file_path).fileName();
    if (group.display_name.isEmpty()) { group.display_name = QStringLiteral("界面选择参数"); }
    for (const auto &name : options->parameter_names) {
        if (name.compare(QStringLiteral("TIME"), Qt::CaseInsensitive) != 0) { group.parameter_ids.append(name); }
    }
    definition.parameter_groups = {group};
}

bool ProcessingOptionsSupport::validateFields(const TraditionalProcessingOptions &options, QString *error)
{
    const double rate = options.task_definition.user_sample_rate_hz;
    if (!std::isfinite(rate) || rate < 1.0 || rate > 1000000000.0 || std::floor(rate) != rate) {
        return fail(error, QStringLiteral("处理采样率必须为 1～1000000000 Hz 的整数。"));
    }
    if (options.parameter_names.isEmpty() ||
        options.parameter_names.first().compare(QStringLiteral("TIME"), Qt::CaseInsensitive) != 0) {
        return fail(error, QStringLiteral("参数列表的第一项必须是 TIME。"));
    }
    if (options.parameter_names.size() < 2) {
        return fail(error, QStringLiteral("请选择至少一个需要处理的参数。"));
    }
    for (const auto &range : options.time_ranges) {
        if (!std::isfinite(range.start_seconds) || !std::isfinite(range.end_seconds) ||
            range.start_seconds < 0 || (range.end_seconds != -1 && range.end_seconds <= range.start_seconds)) {
            return fail(error, QStringLiteral("时间段无效，结束时间必须大于起始时间。"));
        }
    }
    return true;
}

bool ProcessingOptionsSupport::readParameterGroup(const QString &path, QStringList *names, QString *error)
{
    QFile file(path);
    if (!file.open(QIODevice::ReadOnly)) {
        return fail(error, QStringLiteral("无法打开参数组文件：%1").arg(file.errorString()));
    }
    const QByteArray bytes = file.readAll();
    if (file.error() != QFileDevice::NoError) { return fail(error, file.errorString()); }
    QString content = decodeConfigText(bytes);
    content.remove(QChar(0xfeff));
    const QRegularExpression separators(QStringLiteral("[\\s,;，；]+"),
                                        QRegularExpression::UseUnicodePropertiesOption);
    const auto parsed = content.split(separators, Qt::SkipEmptyParts);
    if (parsed.isEmpty() || parsed.first().compare(QStringLiteral("TIME"), Qt::CaseInsensitive) != 0) {
        return fail(error, QStringLiteral("参数组文件的第一个参数必须是 TIME。"));
    }
    *names = parsed;
    return true;
}
