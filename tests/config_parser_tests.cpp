#include "config_parser.h"

#include <QtTest>

class ConfigParserTests : public QObject
{
    Q_OBJECT

private slots:
    void generatesLayoutTableForMultiWordParameters();
    void rebuildsConfigLayoutTables();
    void rejectsInvalidSourceBitRanges();
    void rejectsOverlappingObjectBitRanges();
};

namespace {

ParameterDescription makeParameter(const QString &id,
                                   const QString &name,
                                   const QList<ParameterWordSegment> &segments)
{
    ParameterDescription parameter;
    parameter.serial_no = 1;
    parameter.id_no = id;
    parameter.name = name;
    parameter.unit = QStringLiteral("V");
    parameter.code_type = ParameterCodeType::UnsignedInteger;
    parameter.code_type_token = QStringLiteral("uint");
    parameter.rate_hz = 50.0;
    parameter.pcm_words = segments.size();
    parameter.word_segments = segments;
    return parameter;
}

StreamFormatDescription makeStream()
{
    StreamFormatDescription stream;
    stream.stream_index = 1;
    stream.stream_id = QStringLiteral("S1");
    stream.word_length_bits = 16;
    stream.frame_length_words = 8;
    stream.subframe_depth = 4;
    return stream;
}

} // namespace

void ConfigParserTests::generatesLayoutTableForMultiWordParameters()
{
    StreamFormatDescription stream = makeStream();

    ParameterWordSegment first_segment;
    first_segment.frame_no = 1;
    first_segment.word_no = 2;
    first_segment.source_bit = 0;
    first_segment.bit_length = 12;
    first_segment.object_bit = 0;

    ParameterWordSegment second_segment;
    second_segment.frame_no = 1;
    second_segment.word_no = 3;
    second_segment.source_bit = 4;
    second_segment.bit_length = 8;
    second_segment.object_bit = 12;

    ParameterDescription parameter = makeParameter(QStringLiteral("P001"),
                                                   QStringLiteral("voltage"),
                                                   {first_segment, second_segment});
    parameter.serial_no = 7;
    parameter.inverse = true;
    parameter.calibration.type = CalibrationType::Linear;
    parameter.calibration.coefficients = {1.0, 2.0};
    stream.parameters.append(parameter);

    StreamLayoutTable layout_table;
    QString error_message;
    QVERIFY2(ConfigParser::generateLayoutTable(stream, &layout_table, &error_message),
             qPrintable(error_message));

    QCOMPARE(layout_table.stream_index, stream.stream_index);
    QCOMPARE(layout_table.stream_id, stream.stream_id);
    QCOMPARE(layout_table.word_length_bits, stream.word_length_bits);
    QCOMPARE(layout_table.entries.size(), 1);

    const StreamLayoutEntry *by_id = layout_table.findById(QStringLiteral("P001"));
    QVERIFY(by_id != nullptr);
    QCOMPARE(layout_table.findByName(QStringLiteral("voltage")), by_id);
    QCOMPARE(by_id->serial_no, 7);
    QCOMPARE(by_id->parameter_name, QStringLiteral("voltage"));
    QCOMPARE(by_id->unit, QStringLiteral("V"));
    QCOMPARE(by_id->inverse, true);
    QCOMPARE(by_id->calibration.type, CalibrationType::Linear);
    QCOMPARE(by_id->word_segments.size(), 2);
    QCOMPARE(by_id->word_segments.at(0).bit_length, 12);
    QCOMPARE(by_id->word_segments.at(1).object_bit, 12);
}

void ConfigParserTests::rebuildsConfigLayoutTables()
{
    StreamFormatDescription stream = makeStream();

    ParameterWordSegment segment;
    segment.frame_no = 0;
    segment.word_no = 0;
    segment.source_bit = 0;
    segment.bit_length = 16;
    segment.object_bit = 0;
    stream.parameters.append(makeParameter(QStringLiteral("P010"), QStringLiteral("pressure"), {segment}));

    Config config;
    config.streams.append(stream);

    QString error_message;
    QVERIFY2(ConfigParser::rebuildLayoutTables(&config, &error_message), qPrintable(error_message));
    QCOMPARE(config.layout_tables.size(), 1);
    QVERIFY(config.layoutByStreamIndex(1) != nullptr);
    QVERIFY(config.layoutByStreamIndex(1)->findById(QStringLiteral("P010")) != nullptr);
}

void ConfigParserTests::rejectsInvalidSourceBitRanges()
{
    StreamFormatDescription stream = makeStream();

    ParameterWordSegment invalid_segment;
    invalid_segment.frame_no = 1;
    invalid_segment.word_no = 1;
    invalid_segment.source_bit = 12;
    invalid_segment.bit_length = 8;
    invalid_segment.object_bit = 0;
    stream.parameters.append(makeParameter(QStringLiteral("P020"), QStringLiteral("invalid"), {invalid_segment}));

    StreamLayoutTable layout_table;
    QString error_message;
    QVERIFY(!ConfigParser::generateLayoutTable(stream, &layout_table, &error_message));
    QVERIFY2(error_message.contains(QStringLiteral("source range")), qPrintable(error_message));
}

void ConfigParserTests::rejectsOverlappingObjectBitRanges()
{
    StreamFormatDescription stream = makeStream();

    ParameterWordSegment first_segment;
    first_segment.frame_no = 1;
    first_segment.word_no = 1;
    first_segment.source_bit = 0;
    first_segment.bit_length = 8;
    first_segment.object_bit = 0;

    ParameterWordSegment second_segment = first_segment;
    second_segment.word_no = 2;
    second_segment.object_bit = 4;

    stream.parameters.append(makeParameter(QStringLiteral("P030"),
                                           QStringLiteral("overlap"),
                                           {first_segment, second_segment}));

    StreamLayoutTable layout_table;
    QString error_message;
    QVERIFY(!ConfigParser::generateLayoutTable(stream, &layout_table, &error_message));
    QVERIFY2(error_message.contains(QStringLiteral("overlaps object bits")), qPrintable(error_message));
}

QTEST_MAIN(ConfigParserTests)

#include "config_parser_tests.moc"
