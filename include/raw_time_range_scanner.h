#pragma once

#include "raw_file_reader.h"

#include <QAtomicInteger>
#include <QSharedPointer>

/** @brief 可协作取消的后台扫描标记；不依赖 GUI 对象生命周期。 */
using RawTimeScanCancellation = QSharedPointer<QAtomicInteger<int>>;

/** @brief 文件时间范围及起始异常识别结果；结束时间为最后完整长周的右开边界。 */
struct RawTimeRangeResult
{
    bool success = false;
    bool cancelled = false;
    bool skipped_startup = false;
    int stream_index = 1;
    double original_start_seconds = 0;
    double start_seconds = 0;
    double last_frame_seconds = 0;
    double end_seconds = 0;
    double skipped_seconds = 0;
    qint64 start_byte_offset = 0;
    qint64 file_bytes = 0;
    qint64 bytes_read = 0;
    int probed_frames = 0;
    QString error;
};

/**
 * @brief 仅读取同步字、子帧 ID 和时间字的有界快速扫描器。
 *
 * 首尾探测后，仅在起点位于零点后一小时内时，对文件前半小时做指数探测；
 * 连续时间中出现超过五秒的持续向前跳变时，二分定位首个正常长周。
 * 不按“零点时间”直接剔除数据；正常从零点开始且无跳变的文件保留原起点。
 */
class RawTimeRangeScanner
{
public:
    /** @brief 共用生产配置导入规则，扫描所选原始文件对应的第一路流。 */
    static RawTimeRangeResult scan(const QString &config_path, const QString &raw_path,
                                   const RawTimeScanCancellation &cancellation = {});
    /** @brief 使用已解析布局扫描单文件，便于配置复用及独立边界测试。 */
    static RawTimeRangeResult scanFile(const QString &raw_path, const StreamDefinition &stream,
                                       const StreamLayoutTable &layout,
                                       const RawTimeScanCancellation &cancellation = {});
    /** @brief 将秒数显示为毫秒时间，跨日时显式标注天数。 */
    static QString displayTime(double seconds);
};
