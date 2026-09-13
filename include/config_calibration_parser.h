#pragma once
#include "config_file_document.h"

/** @brief 将 CFTE 校准字段转换为类型化系数/点表，拒绝缺失、重复和非有限数值。 */
class ConfigCalibrationParser
{
public:
    /** @brief 读取一个参数的校准数据；失败时返回含参数名的诊断。 */
    static bool parse(const ConfigParameterRecord &record, CalibrationDefinition *calibration,
                      QString *error_message = nullptr);
};
