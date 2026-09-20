#include "config_file_document.h"

#include <QtTest>

class ConfigFileDocumentTests : public QObject
{
    Q_OBJECT

private slots:
    void parsesParameterSectionsAndWordFields();
    void editsAndExportsParameterCounts();
    void rejectsDuplicateIdsAndMissingNames();
};

namespace {

QString sampleConfigText()
{
    return QStringLiteral(
        "#CFTE/filemain/rec01.00\n"
        "Header_informations:\n"
        "Create_date: 2026-05-31\n"
        "test_id: demo\n"
        "stream_number: 2\n"
        "derpar_number: 0\n"
        "\n"
        "The format description of stream 1:\n"
        "aircraft_name: AC1\n"
        "test_name: T1\n"
        "parameter_number: 2\n"
        "bit_rate: 1000\n"
        "\n"
        "the_parameter_description_of_stream_1:\n"
        "serial_no.: 1\n"
        "id_no.: S1P001\n"
        "name: TEMP\n"
        "unit: C\n"
        "cal_type: line\n"
        "code_type: uint\n"
        "cal_date: 2026-05-31\n"
        "cal_number: K1\n"
        "rate: 50\n"
        "word_interval: 2\n"
        "sign_bit: 0\n"
        "inverse: 0\n"
        "pcm_words: 2\n"
        "frame_no: 0,0,0,0,0,0\n"
        "word_no: 1,2,0,0,0,0\n"
        "source_bit: 0,4,0,0,0,0\n"
        "bit_length: 8,8,0,0,0,0\n"
        "object_bit: 0,8,0,0,0,0\n"
        "description: temperature channel\n"
        "\n"
        "serial_no.: 2\n"
        "id_no.: S1P002\n"
        "name: PRESS\n"
        "unit: Pa\n"
        "cal_type: NO\n"
        "code_type: sint\n"
        "cal_date: \n"
        "cal_number: \n"
        "rate: 25\n"
        "word_interval: 4\n"
        "sign_bit: 15\n"
        "inverse: 0\n"
        "pcm_words: 1\n"
        "frame_no: 1,0,0,0,0,0\n"
        "word_no: 3,0,0,0,0,0\n"
        "source_bit: 0,0,0,0,0,0\n"
        "bit_length: 16,0,0,0,0,0\n"
        "object_bit: 0,0,0,0,0,0\n"
        "description: pressure channel\n"
        "\n"
        "The format description of stream 2:\n"
        "aircraft_name: AC2\n"
        "test_name: T2\n"
        "parameter_number: 1\n"
        "bit_rate: 2000\n"
        "\n"
        "the_parameter_description_of_stream_2:\n"
        "serial_no.: 1\n"
        "id_no.: S2P001\n"
        "name: VOLT\n"
        "unit: V\n"
        "cal_type: poly\n"
        "code_type: uint\n"
        "cal_date: 2026-05-31\n"
        "cal_number: K2\n"
        "rate: 100\n"
        "word_interval: 1\n"
        "sign_bit: 0\n"
        "inverse: 1\n"
        "pcm_words: 1\n"
        "frame_no: 0,0,0,0,0,0\n"
        "word_no: 0,0,0,0,0,0\n"
        "source_bit: 0,0,0,0,0,0\n"
        "bit_length: 12,0,0,0,0,0\n"
        "object_bit: 0,0,0,0,0,0\n"
        "description: voltage channel\n");
}

ConfigParameterRecord newRecord(int stream_index, const QString &id, const QString &name)
{
    ConfigParameterRecord record;
    record.stream_index = stream_index;
    record.setValue(QStringLiteral("id_no."), id);
    record.setValue(QStringLiteral("name"), name);
    record.setValue(QStringLiteral("unit"), QStringLiteral("m/s"));
    record.setValue(QStringLiteral("rate"), QStringLiteral("10"));
    record.setValue(QStringLiteral("frame_no"), QStringLiteral("0,0,0,0,0,0"));
    record.setValue(QStringLiteral("word_no"), QStringLiteral("0,0,0,0,0,0"));
    record.setValue(QStringLiteral("source_bit"), QStringLiteral("0,0,0,0,0,0"));
    record.setValue(QStringLiteral("bit_length"), QStringLiteral("16,0,0,0,0,0"));
    record.setValue(QStringLiteral("object_bit"), QStringLiteral("0,0,0,0,0,0"));
    return record;
}

} // namespace

void ConfigFileDocumentTests::parsesParameterSectionsAndWordFields()
{
    ConfigFileDocument document;
    QString error_message;
    QVERIFY2(document.loadFromText(sampleConfigText(), QStringLiteral("sample.cfg"), &error_message),
             qPrintable(error_message));

    QCOMPARE(document.streamIndexes(), QList<int>({1, 2}));
    QCOMPARE(document.parameters().size(), 3);
    QCOMPARE(document.parameterNames(), QStringList({QStringLiteral("TEMP"),
                                                     QStringLiteral("PRESS"),
                                                     QStringLiteral("VOLT")}));

    const ConfigParameterRecord first = document.parameters().first();
    QCOMPARE(first.stream_index, 1);
    QCOMPARE(first.serial_no, 1);
    QCOMPARE(first.idNo(), QStringLiteral("S1P001"));
    QCOMPARE(first.value(QStringLiteral("pcm_words")), QStringLiteral("2"));
    QCOMPARE(first.value(QStringLiteral("frame_no")), QStringLiteral("0,0,0,0,0,0"));
    QCOMPARE(first.value(QStringLiteral("word_no")), QStringLiteral("1,2,0,0,0,0"));
    QCOMPARE(first.value(QStringLiteral("source_bit")), QStringLiteral("0,4,0,0,0,0"));
    QCOMPARE(first.value(QStringLiteral("bit_length")), QStringLiteral("8,8,0,0,0,0"));
    QCOMPARE(first.value(QStringLiteral("object_bit")), QStringLiteral("0,8,0,0,0,0"));
}

void ConfigFileDocumentTests::editsAndExportsParameterCounts()
{
    ConfigFileDocument document;
    QString error_message;
    QVERIFY2(document.loadFromText(sampleConfigText(), QStringLiteral("sample.cfg"), &error_message),
             qPrintable(error_message));

    ConfigParameterRecord added = newRecord(1, QStringLiteral("S1P003"), QStringLiteral("FLOW"));
    QVERIFY2(document.addParameter(added, &error_message), qPrintable(error_message));
    QVERIFY(document.isDirty());

    QList<ConfigParameterRecord> parameters = document.parameters();
    QCOMPARE(parameters.size(), 4);
    QCOMPARE(parameters.at(2).stream_index, 1);
    QCOMPARE(parameters.at(2).serial_no, 3);
    QCOMPARE(parameters.at(2).name(), QStringLiteral("FLOW"));

    ConfigParameterRecord edited = parameters.at(2);
    edited.setValue(QStringLiteral("name"), QStringLiteral("FLOW_RATE"));
    QVERIFY2(document.updateParameter(2, edited, &error_message), qPrintable(error_message));

    QVERIFY2(document.removeParameter(1, &error_message), qPrintable(error_message));
    const QString exported_text = document.toText(&error_message);
    for (const QString &line : exported_text.split(QLatin1Char('\n'))) {
        if (!line.contains(QLatin1Char(':'))) {
            continue;
        }
        if (line.startsWith(QStringLiteral("The format description of stream"))) {
            QVERIFY(line == QStringLiteral("The format description of stream 1:") ||
                    line == QStringLiteral("The format description of stream 2:"));
        } else if (line.startsWith(QStringLiteral("the_parameter_description_of_stream"))) {
            QVERIFY(line == QStringLiteral("the_parameter_description_of_stream 1:") ||
                    line == QStringLiteral("the_parameter_description_of_stream 2:"));
        } else {
            QCOMPARE(line.indexOf(QLatin1Char(':')), 19);
            QVERIFY(!line.at(18).isSpace());
        }
    }

    QVERIFY2(!exported_text.isEmpty(), qPrintable(error_message));
    QVERIFY(exported_text.contains(QStringLiteral("\nHeader_informations:\n")));
    QVERIFY(exported_text.contains(QStringLiteral("   parameter_number: 2")));
    QVERIFY(exported_text.contains(QStringLiteral("               name: FLOW_RATE")));
    QVERIFY(!exported_text.contains(QStringLiteral("               name: PRESS")));

    ConfigFileDocument reloaded;
    QVERIFY2(reloaded.loadFromText(exported_text, QStringLiteral("exported.cfg"), &error_message),
             qPrintable(error_message));
    QCOMPARE(reloaded.parameters().size(), 3);
    QCOMPARE(reloaded.parameters().at(1).name(), QStringLiteral("FLOW_RATE"));
}

void ConfigFileDocumentTests::rejectsDuplicateIdsAndMissingNames()
{
    ConfigFileDocument document;
    QString error_message;
    QVERIFY2(document.loadFromText(sampleConfigText(), QStringLiteral("sample.cfg"), &error_message),
             qPrintable(error_message));

    ConfigParameterRecord duplicate = newRecord(1, QStringLiteral("S1P001"), QStringLiteral("DUP"));
    QVERIFY(!document.addParameter(duplicate, &error_message));
    QVERIFY2(error_message.contains(QStringLiteral("重复")), qPrintable(error_message));

    ConfigParameterRecord missing_name = newRecord(1, QStringLiteral("S1P099"), QString());
    QVERIFY(!document.addParameter(missing_name, &error_message));
    QVERIFY2(error_message.contains(QStringLiteral("不能为空")), qPrintable(error_message));
}

QTEST_MAIN(ConfigFileDocumentTests)

#include "config_file_document_tests.moc"
