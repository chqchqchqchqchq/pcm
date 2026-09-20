#include "raw_time_range_scanner.h"

#include "legacy_pcm_decoder.h"
#include "traditional_processing_controller.h"

#include <QFile>
#include <QMap>
#include <QTime>

#include <algorithm>
#include <array>
#include <cmath>

namespace {
constexpr qint64 kPageBytes = 64 * 1024;
constexpr qint64 kSyncSearchBytes = 1024 * 1024;
constexpr double kStartupSearchSeconds = 30 * 60;
constexpr double kDaySeconds = 86400;

/** @brief 一个已验证同步和时间码的长周位置。 */
struct TimeSample
{
    qint64 offset = 0;
    double seconds = 0;
};

/** @brief 扫描工作线程独占的文件、小窗口缓存与时间探测结果。 */
class TimeProbe
{
public:
    /** @brief 绑定只读布局及可取消标记，不持有任何 GUI 指针。 */
    TimeProbe(const QString &path, const StreamDefinition &stream, const StreamLayoutTable &layout,
              const RawTimeScanCancellation &cancellation)
        : file_(path), stream_(stream), layout_(layout), cancellation_(cancellation)
    {
        frame_bytes = qint64(layout.frame_length_words) * layout.subframe_depth * 2;
    }

    /** @brief 校验布局并打开文件，仅读取四个时间字，不解算业务参数。 */
    bool open()
    {
        if (!LegacyPcmDecoder::validate(layout_, layout_.major_frame_rate_hz, {}, &error) ||
            !LegacyPcmDecoder::timeWordAddresses(layout_, &time_addresses_, &error)) {
            return false;
        }
        if (stream_.sync_word.isEmpty() || stream_.sync_word.size() > layout_.frame_length_words * 2) {
            error = QStringLiteral("配置中的同步字无效");
            return false;
        }
        if (!file_.open(QIODevice::ReadOnly)) {
            error = file_.errorString();
            return false;
        }
        file_bytes = file_.size();
        if (file_bytes < frame_bytes) {
            error = QStringLiteral("原始数据不足一个完整长周");
            return false;
        }
        return true;
    }

    /** @brief 检查协作取消；调用频度与探测数量相关，不按全文件大小增长。 */
    bool cancelled() const
    {
        return cancellation_ && cancellation_->loadAcquire() != 0;
    }

    /** @brief 在给定位置附近寻找有效长周；反向用于找到文件尾部最后完整长周。 */
    bool find(qint64 offset, bool backward, TimeSample *sample)
    {
        if (readFrame(offset, sample)) {
            return true;
        }
        if (cancelled() || !error.isEmpty()) {
            return false;
        }
        const qint64 begin = backward ? qMax<qint64>(0, offset - kSyncSearchBytes) : offset;
        const qint64 end = qMin(file_bytes, backward ? offset + stream_.sync_word.size()
                                                    : offset + kSyncSearchBytes);
        QByteArray search;
        if (!readSpan(begin, end - begin, &search)) {
            return false;
        }
        int position = backward ? search.lastIndexOf(stream_.sync_word) : search.indexOf(stream_.sync_word);
        while (position >= 0 && !cancelled()) {
            if (readFrame(begin + position, sample)) {
                return true;
            }
            if (!error.isEmpty()) {
                return false;
            }
            if (backward && position == 0) {
                break;
            }
            position = backward ? search.lastIndexOf(stream_.sync_word, position - 1)
                                : search.indexOf(stream_.sync_word, position + 1);
        }
        return false;
    }

    /** @brief 按首帧对齐后的序号探测，可在少量失步字节后恢复同步。 */
    bool atIndex(qint64 index, TimeSample *sample)
    {
        return find(first_offset + index * frame_bytes, false, sample);
    }

    /** @brief 判断相邻多个长周时间是否连续，允许缺少毫秒字时的一秒量化误差。 */
    bool stable(const TimeSample &sample)
    {
        TimeSample previous = sample;
        int compared = 0;
        for (int index = 1; index <= 3; ++index) {
            if (sample.offset + index * frame_bytes > file_bytes - frame_bytes) {
                break;
            }
            TimeSample next;
            if (!readFrame(sample.offset + index * frame_bytes, &next)) {
                return false;
            }
            double delta = next.seconds - previous.seconds;
            if (delta < -kDaySeconds / 2) {
                delta += kDaySeconds;
            }
            if (delta < 0 || std::abs(delta - 1.0 / layout_.major_frame_rate_hz) >
                                 qMax(1.01, 2.0 / layout_.major_frame_rate_hz)) {
                return false;
            }
            previous = next;
            ++compared;
        }
        return compared >= 2;
    }

    /** @brief 根据长周字节位置换算采集时长，只用于检测时钟跳变和估计跨日。 */
    double elapsed(const TimeSample &left, const TimeSample &right) const
    {
        return double(right.offset - left.offset) / double(frame_bytes) / layout_.major_frame_rate_hz;
    }

    qint64 frame_bytes = 0;
    qint64 file_bytes = 0;
    qint64 first_offset = 0;
    qint64 bytes_read = 0;
    int probed_frames = 0;
    QString error;

private:
    /** @brief 缓存少量连续字节，读取同步字和时间字时避免装入整个长周。 */
    bool readSpan(qint64 offset, qint64 count, QByteArray *bytes)
    {
        if (cancelled() || offset < 0 || count <= 0 || offset > file_bytes - count) {
            return false;
        }
        if (offset < cache_offset_ || offset + count > cache_offset_ + cache_.size()) {
            cache_offset_ = offset / kPageBytes * kPageBytes;
            const qint64 length = qMin(file_bytes - cache_offset_, qMax(kPageBytes, offset - cache_offset_ + count));
            if (!file_.seek(cache_offset_)) {
                error = file_.errorString();
                return false;
            }
            cache_ = file_.read(length);
            bytes_read += cache_.size();
            if (cache_.size() != length) {
                error = file_.error() == QFile::NoError ? QStringLiteral("扫描期间原始文件大小发生变化")
                                                       : file_.errorString();
                return false;
            }
        }
        *bytes = cache_.mid(int(offset - cache_offset_), int(count));
        return true;
    }

    /** @brief 验证全部子帧同步字和 ID，再使用生产解码规则读取原始时间字。 */
    bool readFrame(qint64 offset, TimeSample *sample)
    {
        if (cancelled() || offset < 0 || offset > file_bytes - frame_bytes) {
            return false;
        }
        const auto found = samples_.constFind(offset);
        if (found != samples_.cend()) {
            *sample = found.value();
            return true;
        }
        if (++probed_frames > 65536) {
            error = QStringLiteral("同步验证次数过多，请检查配置与原始数据是否匹配");
            return false;
        }
        int mask = 1;
        while (mask < layout_.subframe_depth - 1) {
            mask = (mask << 1) | 1;
        }
        for (int index = 0; index < layout_.subframe_depth; ++index) {
            const qint64 subframe = offset + qint64(index) * layout_.frame_length_words * 2;
            QByteArray bytes;
            if (!readSpan(subframe, stream_.sync_word.size(), &bytes) || bytes != stream_.sync_word) {
                return false;
            }
            if (layout_.id_position_word >= 0) {
                if (!readSpan(subframe + layout_.id_position_word * 2, 2, &bytes)) {
                    return false;
                }
                const quint16 id = quint8(bytes[0]) | (quint16(quint8(bytes[1])) << 8);
                if (((id >> layout_.id_lsb) & mask) != index) {
                    return false;
                }
            }
        }
        QList<quint16> words;
        for (qint64 address : time_addresses_) {
            quint16 word = 0;
            if (address >= 0) {
                QByteArray bytes;
                if (!readSpan(offset + address * 2, 2, &bytes)) {
                    return false;
                }
                word = quint8(bytes[0]) | (quint16(quint8(bytes[1])) << 8);
            }
            words.append(word);
        }
        double seconds = 0;
        if (!LegacyPcmDecoder::decodeTimeWords(layout_.time_code_type, words, &seconds) || !std::isfinite(seconds)) {
            return false;
        }
        *sample = {offset, seconds};
        samples_.insert(offset, *sample);
        return true;
    }

    QFile file_;
    const StreamDefinition &stream_;
    const StreamLayoutTable &layout_;
    RawTimeScanCancellation cancellation_;
    std::array<qint64, 4> time_addresses_{{-1, -1, -1, -1}};
    qint64 cache_offset_ = -1;
    QByteArray cache_;
    QMap<qint64, TimeSample> samples_;
};

/** @brief 确认新时钟在后续十秒仍持续，排除短暂时间尖峰。 */
bool sustained(TimeProbe &probe, const TimeSample &candidate, qint64 last_index, double fps)
{
    if (!probe.stable(candidate)) {
        return false;
    }
    const qint64 index = (candidate.offset - probe.first_offset) / probe.frame_bytes;
    const qint64 confirm_index = qMin(last_index, index + qint64(std::ceil(10 * fps)));
    TimeSample later;
    if (!probe.atIndex(confirm_index, &later)) {
        return false;
    }
    const double expected = probe.elapsed(candidate, later);
    double delta = later.seconds - candidate.seconds;
    delta += std::round((expected - delta) / kDaySeconds) * kDaySeconds;
    return std::abs(delta - expected) <= qMax(1.01, 2.0 / fps);
}
}

RawTimeRangeResult RawTimeRangeScanner::scan(const QString &config_path, const QString &raw_path,
                                              const RawTimeScanCancellation &cancellation)
{
    RawTimeRangeResult result;
    if (cancellation && cancellation->loadAcquire()) {
        result.cancelled = true;
        return result;
    }
    Config config;
    QList<StreamDefinition> streams;
    if (!TraditionalProcessingController::loadConfig(config_path, &config, &result.error) ||
        !TraditionalProcessingController::buildStreamDefinitions(config, &streams, &result.error)) {
        return result;
    }
    // 与原始文件选择约定一致：所选文件属于流 1，其余流使用同名后缀文件。
    for (const StreamDefinition &stream : streams) {
        if (stream.stream_index == 1 || streams.size() == 1) {
            for (const StreamLayoutTable &layout : config.layout_tables) {
                if (layout.stream_id == stream.stream_id) {
                    return scanFile(raw_path, stream, layout, cancellation);
                }
            }
        }
    }
    result.error = QStringLiteral("配置中没有所选原始文件对应的流 1");
    return result;
}

RawTimeRangeResult RawTimeRangeScanner::scanFile(const QString &raw_path, const StreamDefinition &stream,
                                                  const StreamLayoutTable &layout,
                                                  const RawTimeScanCancellation &cancellation)
{
    RawTimeRangeResult result;
    result.stream_index = stream.stream_index;
    TimeProbe probe(raw_path, stream, layout, cancellation);
    const auto finish = [&]() {
        result.cancelled = probe.cancelled();
        result.file_bytes = probe.file_bytes;
        result.bytes_read = probe.bytes_read;
        result.probed_frames = probe.probed_frames;
        if (!probe.error.isEmpty()) {
            result.error = probe.error;
        }
        if (result.cancelled || !result.error.isEmpty()) {
            result.success = false;
        }
        return result;
    };
    if (!probe.open()) {
        return finish();
    }
    TimeSample first, last;
    if (!probe.find(0, false, &first) || !probe.find(probe.file_bytes - probe.frame_bytes, true, &last)) {
        result.error = QStringLiteral("未找到完整且时间码有效的长周，请检查配置、同步字和时间参数");
        return finish();
    }
    probe.first_offset = first.offset;
    const qint64 last_index = (last.offset - first.offset) / probe.frame_bytes;
    const double fps = layout.major_frame_rate_hz;
    const double jump_threshold = qMax(5.0, 4.0 / fps);
    TimeSample chosen = first;
    if (first.seconds >= 0 && first.seconds < 3600 && probe.stable(first)) {
        const qint64 limit = qMin(last_index, qint64(std::ceil(kStartupSearchSeconds * fps)));
        qint64 previous_index = 0;
        for (qint64 index = 1; index <= limit && !probe.cancelled();) {
            TimeSample candidate;
            if (!probe.atIndex(index, &candidate)) {
                result.error = QStringLiteral("起始时间探测遇到无法识别的帧");
                return finish();
            }
            const double shift = candidate.seconds - first.seconds - probe.elapsed(first, candidate);
            if (shift > jump_threshold && sustained(probe, candidate, last_index, fps)) {
                qint64 low = previous_index, high = index;
                while (high - low > 1 && !probe.cancelled()) {
                    const qint64 middle = low + (high - low) / 2;
                    TimeSample sample;
                    if (!probe.atIndex(middle, &sample)) {
                        result.error = QStringLiteral("无法定位起始时间跳变边界");
                        return finish();
                    }
                    if (sample.seconds - first.seconds - probe.elapsed(first, sample) > jump_threshold) {
                        high = middle;
                    } else {
                        low = middle;
                    }
                }
                TimeSample boundary, before;
                if (!probe.atIndex(high, &boundary) || !probe.atIndex(high - 1, &before)) {
                    result.error = QStringLiteral("无法读取时间跳变相邻长周");
                    return finish();
                }
                if (boundary.seconds - before.seconds - probe.elapsed(before, boundary) > jump_threshold &&
                    sustained(probe, boundary, last_index, fps)) {
                    chosen = boundary;
                    result.skipped_startup = true;
                    break;
                }
            }
            previous_index = index;
            if (index == limit) {
                break;
            }
            index = qMin(limit, index * 2);
        }
    }
    result.original_start_seconds = first.seconds;
    result.start_seconds = chosen.seconds;
    result.start_byte_offset = chosen.offset;
    result.skipped_seconds = probe.elapsed(first, chosen);
    const double expected = probe.elapsed(chosen, last);
    const double delta = last.seconds - chosen.seconds;
    double days = qMax(0.0, std::round((expected - delta) / kDaySeconds));
    if (delta < 0 && days == 0) {
        result.error = QStringLiteral("文件首尾时间倒退，无法确认数据时间范围");
        return finish();
    }
    result.last_frame_seconds = last.seconds + days * kDaySeconds;
    result.end_seconds = result.last_frame_seconds + 1.0 / fps;
    if (result.end_seconds <= result.start_seconds) {
        result.error = QStringLiteral("原始数据时间范围无效");
        return finish();
    }
    result.success = true;
    return finish();
}

QString RawTimeRangeScanner::displayTime(double seconds)
{
    const qint64 milliseconds = qRound64(seconds * 1000);
    const qint64 days = milliseconds / 86400000;
    const QString clock = QTime::fromMSecsSinceStartOfDay(int(milliseconds % 86400000))
        .toString(QStringLiteral("HH:mm:ss.zzz"));
    return days > 0 ? QStringLiteral("第 %1 天 %2").arg(days + 1).arg(clock) : clock;
}
