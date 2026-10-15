#pragma once

#include "traditional_processing_types.h"

/** @brief 单任务和多任务共用的字段转换、参数组解析及基础校验。 */
class ProcessingOptionsSupport
{
public:
    /** @brief 规范化绝对路径；空字段保持为空。 */
    static QString absolutePath(const QString &path);
    /** @brief 小型配置/辅助文件的内容指纹，不用于大体积原始数据。 */
    static QString fingerprint(const QString &path);
    /** @brief 从参数快照重建通用五元组，保留 TIME 首列语义。 */
    static void normalize(TraditionalProcessingOptions *options);
    /** @brief 检查参数、采样率及已解析时间范围，不触碰输出文件。 */
    static bool validateFields(const TraditionalProcessingOptions &options, QString *error = nullptr);
    /** @brief 按现有 UTF-8/GB18030 规则读取参数组。 */
    static bool readParameterGroup(const QString &path, QStringList *names, QString *error = nullptr);
};
