#include "config_calibration_parser.h"

#include <QRegularExpression>
#include <algorithm>
#include <cmath>

namespace {
/** @brief 解析逗号分隔的有限实数，空项和非法项均返回错误。 */
bool numbers(const QString &text, QList<double> *values)
{
    values->clear();
    if (text.trimmed().isEmpty()) {
        return false;
    }
    for (const QString &token : text.split(QLatin1Char(','))) {
        bool ok = false;
        const double value = token.trimmed().toDouble(&ok);
        if (!ok || !std::isfinite(value)) {
            return false;
        }
        values->append(value);
    }
    return true;
}
}

bool ConfigCalibrationParser::parse(const ConfigParameterRecord &record, CalibrationDefinition *calibration,
                                    QString *error_message)
{
    auto fail = [&](const QString &reason) {
        if (error_message != nullptr) {
            *error_message = QStringLiteral("参数 %1：%2").arg(record.name(), reason);
        }
        return false;
    };
    if (calibration == nullptr) {
        return fail(QStringLiteral("校准输出对象为空"));
    }
    CalibrationDefinition parsed;
    parsed.type_token = record.value(QStringLiteral("cal_type"));
    parsed.type = calibrationTypeFromToken(parsed.type_token);
    parsed.cal_date = QDate::fromString(record.value(QStringLiteral("cal_date")), Qt::ISODate);
    parsed.cal_number = record.value(QStringLiteral("cal_number"));
    // 旧版 float/double 是原码解释方式，不是工程校准。
    if ((parsed.type_token.compare("float", Qt::CaseInsensitive) == 0 ||
                                   parsed.type_token.compare("double", Qt::CaseInsensitive) == 0)) {
        parsed.type = CalibrationType::None;
    }
    switch (parsed.type) {
    case CalibrationType::Unknown:
        return fail(QStringLiteral("不支持的校准类型：%1").arg(parsed.type_token));
    case CalibrationType::Linear:
    case CalibrationType::Polynomial:
        if (!numbers(record.value(QStringLiteral("coefficient")), &parsed.coefficients)) {
            return fail(QStringLiteral("coefficient 缺失或包含非法数值"));
        }
        // 旧版 line 与 poly 使用相同的六项多项式。
        if (parsed.type == CalibrationType::Linear) {
            parsed.type = CalibrationType::Polynomial;
        }
        break;
    case CalibrationType::Segmented: {
        bool ok = false;
        const int count = record.value(QStringLiteral("segment_number")).toInt(&ok);
        const QStringList rows = record.value(QStringLiteral("code,phy")).split(QLatin1Char(';'));
        if (!ok || count < 2 || count != rows.size()) {
            return fail(QStringLiteral("segment_number 必须等于 code,phy 点数且至少为 2"));
        }
        for (const QString &row : rows) {
            QList<double> pair;
            if (!numbers(row, &pair) || pair.size() != 2) {
                return fail(QStringLiteral("code,phy 必须为有限数值对"));
            }
            CalibrationPoint point;
            point.raw_value = pair.at(0);
            point.engineering_value = pair.at(1);
            parsed.points.append(point);
        }
        std::sort(parsed.points.begin(), parsed.points.end(), [](const CalibrationPoint &a, const CalibrationPoint &b) {
            return a.raw_value < b.raw_value;
        });
        for (int index = 1; index < parsed.points.size(); ++index) {
            if (parsed.points.at(index).raw_value == parsed.points.at(index - 1).raw_value) {
                return fail(QStringLiteral("code,phy 存在重复码值"));
            }
        }
        break;
    }
    case CalibrationType::Hyperbolic: {
        bool ok = false;
        parsed.segment_point = record.value(QStringLiteral("segment_point")).toDouble(&ok);
        QList<double> first, second;
        if (!ok || !std::isfinite(parsed.segment_point) ||
            !numbers(record.value(QStringLiteral("first_segment")), &first) || first.size() != 4 ||
            !numbers(record.value(QStringLiteral("second_segment")), &second) || second.size() != 4) {
            return fail(QStringLiteral("hyper 需要 segment_point 和两组四项系数"));
        }
        parsed.coefficients = first + second;
        break;
    }
    case CalibrationType::None:
        break;
    }
    *calibration = parsed;
    return true;
}
