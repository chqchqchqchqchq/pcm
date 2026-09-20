#include "legacy_test_data.h"
#include "raw_time_range_scanner.h"

#include <QTemporaryDir>
#include <QtTest>

/** @brief 独立构造时间序列，验证正常范围、零点前导、尖峰、帧尾及取消。 */
class RawTimeRangeScannerTests : public QObject
{
    Q_OBJECT
private slots:
    void detectsStartup_data();
    void detectsStartup();
    void preservesMidnightAndRejectsSpike();
    void supportsCrossDayAndShortFiles();
    void rejectsInvalidInputAndCancellation();
};

namespace {
/** @brief 按毫秒生成手工时间字，参数值不参与扫描。 */
QByteArray frameAt(qint64 milliseconds)
{
    const qint64 time = milliseconds % 86400000;
    return LegacyTestData::frame(int(time / 1000 % 60), 7, int(time / 3600000),
                                 int(time / 60000 % 60), int(time % 1000));
}

/** @brief 为手工夹具提供同步定义，完全独立于配置生成器。 */
StreamDefinition stream()
{
    StreamDefinition value;
    value.stream_index = 1;
    value.stream_id = "stream_1";
    value.sync_word = QByteArray::fromHex("55aa");
    value.frame_length_bytes = 16;
    return value;
}
}

void RawTimeRangeScannerTests::detectsStartup_data()
{
    QTest::addColumn<int>("prefix_frames");
    QTest::addColumn<bool>("frozen");
    QTest::addColumn<bool>("noise");
    QTest::newRow("normal") << 0 << false << false;
    QTest::newRow("thirty-seconds") << 240 << false << false;
    QTest::newRow("three-minutes") << 1440 << false << false;
    QTest::newRow("fractional-boundary") << 245 << false << true;
    QTest::newRow("frozen-startup") << 1440 << true << false;
    QTest::newRow("twenty-nine-minutes") << 13920 << false << false;
}

void RawTimeRangeScannerTests::detectsStartup()
{
    QFETCH(int, prefix_frames);
    QFETCH(bool, frozen);
    QFETCH(bool, noise);
    QTemporaryDir directory;
    const int frames = prefix_frames + 1024;
    QByteArray bytes = noise ? QByteArray(3, char(0x13)) : QByteArray();
    for (int index = 0; index < frames; ++index) {
        const qint64 ms = index < prefix_frames ? (frozen ? 0 : index * 125)
                                               : 3723456 + (index - prefix_frames) * 125;
        bytes.append(frameAt(ms));
    }
    if (noise) {
        bytes.append(QByteArray(7, char(0x13)));
    }
    const QString path = directory.filePath("sample.pcm");
    QVERIFY(LegacyTestData::write(path, bytes));
    const RawTimeRangeResult result = RawTimeRangeScanner::scanFile(path, stream(), LegacyTestData::layout(8));
    QVERIFY2(result.success, qPrintable(result.error));
    QCOMPARE(result.skipped_startup, prefix_frames > 0);
    QCOMPARE(result.start_byte_offset, qint64(prefix_frames * 16 + (noise ? 3 : 0)));
    QCOMPARE(result.start_seconds, 3723.456);
    QCOMPARE(result.end_seconds, 3723.456 + 128);
    QCOMPARE(result.skipped_seconds, prefix_frames / 8.0);
    QVERIFY2(result.probed_frames < 300, qPrintable(QString::number(result.probed_frames)));
}

void RawTimeRangeScannerTests::preservesMidnightAndRejectsSpike()
{
    QTemporaryDir directory;
    QByteArray bytes;
    for (int index = 0; index < 4096; ++index) {
        // 指数探测正好会命中该尖峰，但后续长周恢复，不能据此排除正常的零点采集。
        bytes.append(frameAt(index == 32 ? 36000000 : index * 125));
    }
    const QString path = directory.filePath("midnight.pcm");
    QVERIFY(LegacyTestData::write(path, bytes));
    const auto result = RawTimeRangeScanner::scanFile(path, stream(), LegacyTestData::layout(8));
    QVERIFY2(result.success, qPrintable(result.error));
    QVERIFY(!result.skipped_startup);
    QCOMPARE(result.start_seconds, 0.0);
    QCOMPARE(result.end_seconds, 512.0);
}

void RawTimeRangeScannerTests::supportsCrossDayAndShortFiles()
{
    QTemporaryDir directory;
    const QString path = directory.filePath("short.pcm");
    QVERIFY(LegacyTestData::write(path, frameAt(3723456)));
    auto result = RawTimeRangeScanner::scanFile(path, stream(), LegacyTestData::layout(8));
    QVERIFY2(result.success, qPrintable(result.error));
    QCOMPARE(result.start_seconds, 3723.456);
    QCOMPARE(result.end_seconds, 3723.581);
    QByteArray bytes;
    for (int index = 0; index < 64; ++index) {
        bytes.append(frameAt(86398000 + index * 125));
    }
    QVERIFY(LegacyTestData::write(path, bytes));
    result = RawTimeRangeScanner::scanFile(path, stream(), LegacyTestData::layout(8));
    QVERIFY2(result.success, qPrintable(result.error));
    QCOMPARE(result.start_seconds, 86398.0);
    QCOMPARE(result.end_seconds, 86406.0);
    QCOMPARE(RawTimeRangeScanner::displayTime(result.end_seconds), QStringLiteral("第 2 天 00:00:06.000"));
}

void RawTimeRangeScannerTests::rejectsInvalidInputAndCancellation()
{
    QTemporaryDir directory;
    const QString path = directory.filePath("bad.pcm");
    QVERIFY(LegacyTestData::write(path, QByteArray(7, char(0))));
    auto result = RawTimeRangeScanner::scanFile(path, stream(), LegacyTestData::layout());
    QVERIFY(!result.success);
    QVERIFY(!result.error.isEmpty());
    QVERIFY(LegacyTestData::write(path, LegacyTestData::frame(0, 0, 25)));
    result = RawTimeRangeScanner::scanFile(path, stream(), LegacyTestData::layout());
    QVERIFY(!result.success);
    QVERIFY(LegacyTestData::write(path, frameAt(3723456)));
    auto layout = LegacyTestData::layout();
    layout.entries.removeFirst();
    result = RawTimeRangeScanner::scanFile(path, stream(), layout);
    QVERIFY(!result.success);
    RawTimeScanCancellation cancellation = RawTimeScanCancellation::create(1);
    result = RawTimeRangeScanner::scanFile(path, stream(), LegacyTestData::layout(), cancellation);
    QVERIFY(!result.success);
    QVERIFY(result.cancelled);
}

QTEST_GUILESS_MAIN(RawTimeRangeScannerTests)
#include "raw_time_range_scanner_tests.moc"
