#pragma once

#include "frame_processor.h"

#include <QVector>
#include <array>

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
    /** @brief 编译四个时间字地址；缺省毫秒地址为 -1，保留 UMA2000 连续三字规则。 */
    static bool timeWordAddresses(const StreamLayoutTable &layout, std::array<qint64, 4> *addresses,
                                  QString *error_message = nullptr);
    /** @brief 从一个完整长周读取时间参数，返回旧版采集器时间。 */
    static bool decodeFrameTime(const FrameTask &frame, const StreamLayoutTable &layout,
                                double *seconds, QString *error_message = nullptr);
    /** @brief 解算一个完整长周；时间窗口使用实际时间码，用户采样率不改变时间轴。 */
    static bool decode(const FrameTask &frame, const StreamLayoutTable &layout,
                       const QSet<QString> &selected_ids, QList<ParameterValue> *values,
                       QString *error_message = nullptr);
};

/**
 * @brief 每路流独占的解算计划；启动时校验并编译地址，工作线程重复使用。
 *
 * 只缓存各参数的段描述和最近一次采样值，不展开“参数数 × 出现次数”矩阵。
 * 每个长周仍检查时间码和数据长度，低频补点只在同一长周内复用。
 */
class LegacyPcmDecodePlan
{
public:
    /** @brief 编译固定布局、采样步长和时间字地址；失败后不能继续解算。 */
    bool prepare(const StreamLayoutTable &layout, double output_rate, const QSet<QString> &selected_ids,
                 bool include_raw_values, QString *error_message = nullptr);
    /** @brief 使用预先校验的地址读取当前长周时间，仍校验实际时间码。 */
    bool decodeFrameTime(const FrameTask &frame, double *seconds, QString *error_message = nullptr) const;
    /** @brief 使用已经解出的时间生成本长周结果；仅当前工作线程调用。 */
    bool decode(const FrameTask &frame, double base_time, QList<ParameterValue> *values,
                QString *error_message = nullptr);

private:
    /** @brief 一个 PCM 字的固定字节地址、源位掩码及目标移位。 */
    struct Segment
    {
        qint64 byte_offset = 0;
        quint64 mask = 0;
        int source_shift = 0;
        int target_shift = 0;
    };
    /** @brief 参数的固定转换信息和长周内补点缓存。 */
    struct Entry
    {
        StreamLayoutEntry definition;
        StreamLayoutEntry raw_definition;
        QVector<Segment> segments;
        qint64 occurrence_stride_bytes = 0;
        int occurrences_per_subframe = 0;
        int hold_slots = 1;
        int width = 0;
        int last_occurrence = -1;
        ParameterValue cached_value;
    };

    QVector<Entry> entries_;
    std::array<qint64, 4> time_addresses_{{-1, -1, -1, -1}};
    QString equipment_;
    qint64 frame_bytes_ = 0;
    qint64 subframe_bytes_ = 0;
    int frame_stride_ = 1;
    int slot_count_ = 1;
    int slot_step_ = 1;
    double slots_per_second_ = 1;
    bool include_raw_values_ = false;
    bool prepared_ = false;
};
