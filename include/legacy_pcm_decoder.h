#pragma once

#include "frame_processor.h"

/** @brief 旧版长周解算：时间字、rate 出现次数、低频补点与用户采样步长。 */
class LegacyPcmDecoder
{
public:
    /** @brief 在启动线程前校验时间字、出现次数和采样参数。 */
    static bool validate(const StreamLayoutTable &layout, double output_rate,
                         const QSet<QString> &selected_ids, QString *error_message = nullptr);
    /** @brief 按旧采集器位布局从原始时间字计算当天秒数。 */
    static bool decodeTimeWords(const QString &equipment, const QList<quint16> &words,
                                double *seconds, QString *error_message = nullptr);
    /** @brief 从一个完整长周读取时间参数，返回旧版采集器时间。 */
    static bool decodeFrameTime(const FrameTask &frame, const StreamLayoutTable &layout,
                                double *seconds, QString *error_message = nullptr);
    /** @brief 解算一个完整长周；时间窗口使用实际时间码，用户采样率不改变时间轴。 */
    static bool decode(const FrameTask &frame, const StreamLayoutTable &layout,
                       const QSet<QString> &selected_ids, QList<ParameterValue> *values,
                       QString *error_message = nullptr);
};
