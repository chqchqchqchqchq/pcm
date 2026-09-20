#include "time_range_parser.h"

#include <QFile>
#include <QTemporaryDir>
#include <QtTest>

class TimeRangeParserTests : public QObject
{
    Q_OBJECT

private slots:
    void parsesFormatsSeparatorsCommentsAndLongHours();
    void reportsTheInvalidLineNumber();
    void warnsButKeepsOverlappingRanges();
    void parsesUtf8File();
};

void TimeRangeParserTests::parsesFormatsSeparatorsCommentsAndLongHours()
{
    const QString content = QStringLiteral(
        "# 支持表头、空行和注释\n"
        "start,end\n"
        "0,1.25\n"
        "01:02:03.004\t01:02:04.5\n"
        "90000 90001 # 超过 24 小时的秒数\n"
        "25:00:00 25:00:01\n"
        "01:02:03:004 01:02:04:500\n");

    QList<TaskTimeRange> ranges;
    QStringList warnings;
    QString error_message;
    QVERIFY2(TimeRangeParser::parseText(content, &ranges, &warnings, &error_message),
             qPrintable(error_message));
    QCOMPARE(ranges.size(), 5);
    QCOMPARE(ranges.at(0).start_seconds, 0.0);
    QCOMPARE(ranges.at(0).end_seconds, 1.25);
    QVERIFY(qAbs(ranges.at(1).start_seconds - 3723.004) < 0.000001);
    QVERIFY(qAbs(ranges.at(1).end_seconds - 3724.5) < 0.000001);
    QCOMPARE(ranges.at(2).start_seconds, 90000.0);
    QCOMPARE(ranges.at(3).start_seconds, 90000.0);
    QCOMPARE(ranges.at(4).start_seconds, 3723.004);
    QCOMPARE(ranges.at(4).end_seconds, 3724.5);
}

void TimeRangeParserTests::reportsTheInvalidLineNumber()
{
    QList<TaskTimeRange> ranges;
    QString error_message;
    QVERIFY(!TimeRangeParser::parseText(QStringLiteral("0 1\nnot-a-range\n2 3\n"),
                                       &ranges,
                                       nullptr,
                                       &error_message));
    QVERIFY(error_message.contains(QStringLiteral("第 2 行")));

    QVERIFY(!TimeRangeParser::parseText(QStringLiteral("2 2\n"),
                                       &ranges,
                                       nullptr,
                                       &error_message));
    QVERIFY(error_message.contains(QStringLiteral("第 1 行")));
}

void TimeRangeParserTests::warnsButKeepsOverlappingRanges()
{
    QList<TaskTimeRange> ranges;
    QStringList warnings;
    QString error_message;
    QVERIFY2(TimeRangeParser::parseText(QStringLiteral("0 2\n1.5 3\n3 4\n"),
                                       &ranges,
                                       &warnings,
                                       &error_message),
             qPrintable(error_message));
    QCOMPARE(ranges.size(), 3);
    QCOMPARE(warnings.size(), 1);
    QVERIFY(warnings.first().contains(QStringLiteral("重叠")));
}

void TimeRangeParserTests::parsesUtf8File()
{
    QTemporaryDir temp_dir;
    QVERIFY(temp_dir.isValid());
    const QString path = temp_dir.filePath(QStringLiteral("时间段.txt"));
    QFile file(path);
    QVERIFY(file.open(QIODevice::WriteOnly));
    const QByteArray content = QStringLiteral("# 时间范围\n00:00:00.250,00:00:00.750\n").toUtf8();
    QCOMPARE(file.write(content), static_cast<qint64>(content.size()));
    file.close();

    QList<TaskTimeRange> ranges;
    QString error_message;
    QVERIFY2(TimeRangeParser::parseFile(path, &ranges, nullptr, &error_message), qPrintable(error_message));
    QCOMPARE(ranges.size(), 1);
    QCOMPARE(ranges.first().start_seconds, 0.25);
    QCOMPARE(ranges.first().end_seconds, 0.75);
}

QTEST_MAIN(TimeRangeParserTests)

#include "time_range_parser_tests.moc"
