#include "legacy_test_data.h"
#include "main_window.h"

#include <QAbstractButton>
#include <QApplication>
#include <QCheckBox>
#include <QDateTime>
#include <QDoubleSpinBox>
#include <QFile>
#include <QLineEdit>
#include <QListWidget>
#include <QMessageBox>
#include <QProgressBar>
#include <QPushButton>
#include <QRadioButton>
#include <QSettings>
#include <QSignalBlocker>
#include <QSignalSpy>
#include <QSplitter>
#include <QTemporaryDir>
#include <QTimeEdit>
#include <QTimer>
#include <QtTest>

class MainWindowTests : public QObject
{
    Q_OBJECT

private slots:
    void initTestCase();
    void init();
    void enablesOnlyTheSelectedTimeMode();
    void reportsPreflightErrorsWithoutStarting();
    void restoresSettingsWithoutSubmittingInvalidPaths();
    void resetsControlsAfterFailure();
    void presentsAllCompletedFiles();
    void showsOnlyTwentyRecentRecordsWithFileNames();
    void restoresHistoryWithoutRestoringTaskSpecificPaths();
    void requiresMultiRangeFileAfterHistoryRestore();
    void keepsHistoryBrowsableButReadOnlyWhileProcessing();
    void createsHistoryAfterValidatedStart();
    void tracksFileAndTemporaryParameterGroups();
    void deletesAndClearsHistoryRecords();
    void restoresHistoryLogSplitterState();

private:
    QTemporaryDir settings_directory_;
};

namespace {

void closeNextMessageBox(QString *text, QString *details = nullptr)
{
    QTimer::singleShot(100, QApplication::instance(), [text, details]() {
        QMessageBox *box = qobject_cast<QMessageBox *>(QApplication::activeModalWidget());
        if (box == nullptr) {
            return;
        }
        if (text != nullptr) {
            *text = box->text();
        }
        if (details != nullptr) {
            *details = box->detailedText();
        }
        box->accept();
    });
}

void acceptNextQuestion()
{
    QTimer::singleShot(100, QApplication::instance(), []() {
        QMessageBox *box = qobject_cast<QMessageBox *>(QApplication::activeModalWidget());
        if (box == nullptr) {
            return;
        }
        QAbstractButton *yes_button = box->button(QMessageBox::Yes);
        if (yes_button != nullptr) {
            yes_button->click();
        }
    });
}

QString historyConfigText()
{
    return LegacyTestData::config();
}

bool writeTestFile(const QString &path, const QByteArray &content)
{
    QFile file(path);
    if (!file.open(QIODevice::WriteOnly | QIODevice::Truncate)) {
        return false;
    }
    return file.write(content) == content.size();
}

QVariantMap makeHistoryRecord(const QString &task_id,
                              const QDateTime &started_at,
                              const QString &config_path,
                              const QString &parameter_group_path,
                              bool parameter_group_from_file,
                              int time_mode = 0)
{
    QVariantMap record;
    record.insert(QStringLiteral("taskId"), task_id);
    record.insert(QStringLiteral("startedAt"), started_at.toString(Qt::ISODateWithMs));
    record.insert(QStringLiteral("status"), QStringLiteral("完成"));
    record.insert(QStringLiteral("configPath"), config_path);
    record.insert(QStringLiteral("configFingerprint"), QString());
    record.insert(QStringLiteral("parameterGroupFromFile"), parameter_group_from_file);
    record.insert(QStringLiteral("parameterGroupPath"), parameter_group_path);
    record.insert(QStringLiteral("parameterGroupFingerprint"), QString());
    record.insert(QStringLiteral("parameterNames"), QStringList({QStringLiteral("TIME"), QStringLiteral("TEMP")}));
    record.insert(QStringLiteral("sampleRateHz"), 2048.0);
    record.insert(QStringLiteral("timeMode"), time_mode);
    record.insert(QStringLiteral("startTime"), QTime(1, 2, 3, 4));
    record.insert(QStringLiteral("endTime"), QTime(2, 3, 4, 5));
    record.insert(QStringLiteral("rawFilePath"), QStringLiteral("D:/historical/raw.pcm"));
    record.insert(QStringLiteral("outputPaths"), QStringList({QStringLiteral("D:/historical/result.txt")}));
    record.insert(QStringLiteral("timeRangeFilePath"), QStringLiteral("D:/historical/ranges.txt"));
    record.insert(QStringLiteral("includeRawValues"), false);
    return record;
}

void writeHistoryRecords(const QList<QVariantMap> &records)
{
    QSettings settings;
    settings.beginGroup(QStringLiteral("processingHistory"));
    settings.remove(QString());
    settings.setValue(QStringLiteral("version"), 1);
    settings.beginWriteArray(QStringLiteral("records"), records.size());
    for (int index = 0; index < records.size(); ++index) {
        settings.setArrayIndex(index);
        const QVariantMap &record = records.at(index);
        for (auto iterator = record.constBegin(); iterator != record.constEnd(); ++iterator) {
            settings.setValue(iterator.key(), iterator.value());
        }
    }
    settings.endArray();
    settings.endGroup();
    settings.sync();
}

} // namespace

void MainWindowTests::initTestCase()
{
    QVERIFY(settings_directory_.isValid());
    QApplication::setAttribute(Qt::AA_DontUseNativeDialogs, true);
    QApplication::setStyle(QStringLiteral("Fusion"));
    QApplication::setOrganizationName(QStringLiteral("PcmProcessTests"));
    QApplication::setApplicationName(QStringLiteral("MainWindowTests"));
    QSettings::setDefaultFormat(QSettings::IniFormat);
    QSettings::setPath(QSettings::IniFormat, QSettings::UserScope, settings_directory_.path());
}

void MainWindowTests::init()
{
    QSettings settings;
    settings.clear();
    settings.sync();
}

void MainWindowTests::enablesOnlyTheSelectedTimeMode()
{
    MainWindow window;
    QRadioButton *single = window.findChild<QRadioButton *>(QStringLiteral("singleTimeRadio"));
    QRadioButton *multiple = window.findChild<QRadioButton *>(QStringLiteral("multipleTimeRadio"));
    QTimeEdit *start = window.findChild<QTimeEdit *>(QStringLiteral("startTimeEdit"));
    QLineEdit *range_file = window.findChild<QLineEdit *>(QStringLiteral("timeRangeFileEdit"));
    QVERIFY(single != nullptr);
    QVERIFY(multiple != nullptr);
    QVERIFY(start != nullptr);
    QVERIFY(range_file != nullptr);
    QVERIFY(!start->isEnabled());
    QVERIFY(!range_file->isEnabled());
    single->setChecked(true);
    QVERIFY(start->isEnabled());
    QVERIFY(!range_file->isEnabled());
    multiple->setChecked(true);
    QVERIFY(!start->isEnabled());
    QVERIFY(range_file->isEnabled());
}

void MainWindowTests::reportsPreflightErrorsWithoutStarting()
{
    MainWindow window;
    window.show();
    QTest::qWait(20);
    QSignalSpy requests(&window, &MainWindow::traditionalProcessingRequested);
    QString dialog_text;
    closeNextMessageBox(&dialog_text);
    QVERIFY(QMetaObject::invokeMethod(&window, "startProcessing", Qt::DirectConnection));
    QVERIFY(dialog_text.contains(QStringLiteral("配置文件")));
    QCOMPARE(requests.count(), 0);
    QVERIFY(window.findChild<QPushButton *>(QStringLiteral("startButton"))->isEnabled());
}

void MainWindowTests::restoresSettingsWithoutSubmittingInvalidPaths()
{
    QSettings settings;
    settings.setValue(QStringLiteral("processing/sampleRate"), 4000.0);
    settings.setValue(QStringLiteral("processing/timeMode"), 2);
    settings.setValue(QStringLiteral("paths/raw"), QStringLiteral("Z:/missing/raw.pcm"));
    settings.sync();

    MainWindow window;
    QSignalSpy requests(&window, &MainWindow::traditionalProcessingRequested);
    QCOMPARE(window.findChild<QDoubleSpinBox *>(QStringLiteral("sampleRateSpin"))->value(), 4000.0);
    QVERIFY(window.findChild<QRadioButton *>(QStringLiteral("multipleTimeRadio"))->isChecked());
    QCOMPARE(window.findChild<QLineEdit *>(QStringLiteral("rawFileEdit"))->text(),
             QStringLiteral("Z:/missing/raw.pcm"));
    QCOMPARE(requests.count(), 0);
}

void MainWindowTests::resetsControlsAfterFailure()
{
    MainWindow window;
    window.show();
    QTest::qWait(20);
    const QString task_id = QStringLiteral("gui-failure-test");
    QVERIFY(QMetaObject::invokeMethod(&window,
                                     "handleTraditionalTaskStarted",
                                     Qt::DirectConnection,
                                     Q_ARG(QString, task_id)));
    QVERIFY(!window.findChild<QPushButton *>(QStringLiteral("startButton"))->isEnabled());
    QVERIFY(!window.findChild<QDoubleSpinBox *>(QStringLiteral("sampleRateSpin"))->isEnabled());

    QString dialog_text;
    closeNextMessageBox(&dialog_text);
    QVERIFY(QMetaObject::invokeMethod(&window,
                                     "handleTraditionalTaskFailed",
                                     Qt::DirectConnection,
                                     Q_ARG(QString, task_id),
                                     Q_ARG(QString, QStringLiteral("测试失败原因"))));
    QCOMPARE(dialog_text, QStringLiteral("测试失败原因"));
    QVERIFY(window.findChild<QPushButton *>(QStringLiteral("startButton"))->isEnabled());
    QVERIFY(!window.findChild<QPushButton *>(QStringLiteral("interruptButton"))->isEnabled());
    QVERIFY(window.findChild<QDoubleSpinBox *>(QStringLiteral("sampleRateSpin"))->isEnabled());
}

void MainWindowTests::presentsAllCompletedFiles()
{
    MainWindow window;
    window.show();
    QTest::qWait(20);
    const QString task_id = QStringLiteral("gui-completion-test");
    const QStringList paths = {
        QStringLiteral("D:/results/raw_range001.txt"),
        QStringLiteral("D:/results/raw_range002.txt"),
    };
    QVERIFY(QMetaObject::invokeMethod(&window,
                                     "handleTraditionalTaskStarted",
                                     Qt::DirectConnection,
                                     Q_ARG(QString, task_id)));
    QString dialog_text;
    QString details;
    closeNextMessageBox(&dialog_text, &details);
    QVERIFY(QMetaObject::invokeMethod(&window,
                                     "handleTraditionalProcessingFinished",
                                     Qt::DirectConnection,
                                     Q_ARG(QString, task_id),
                                     Q_ARG(QStringList, paths)));
    QVERIFY(dialog_text.contains(QStringLiteral("2")));
    QCOMPARE(details, paths.join(QLatin1Char('\n')));
    QCOMPARE(window.findChild<QProgressBar *>(QStringLiteral("progressBar"))->value(), 100);
    QVERIFY(window.findChild<QPushButton *>(QStringLiteral("startButton"))->isEnabled());
}

void MainWindowTests::showsOnlyTwentyRecentRecordsWithFileNames()
{
    QList<QVariantMap> records;
    const QDateTime base_time = QDateTime::currentDateTime().addDays(-1);
    for (int index = 0; index < 21; ++index) {
        records.append(makeHistoryRecord(
            QStringLiteral("history-%1").arg(index),
            base_time.addSecs(index),
            QStringLiteral("D:/configs/config_%1.cfte").arg(index),
            QStringLiteral("D:/groups/group_%1.txt").arg(index),
            true));
    }
    writeHistoryRecords(records);

    MainWindow window;
    QListWidget *history = window.findChild<QListWidget *>(QStringLiteral("processingHistoryList"));
    QSplitter *splitter = window.findChild<QSplitter *>(QStringLiteral("historyLogSplitter"));
    QVERIFY(history != nullptr);
    QVERIFY(splitter != nullptr);
    QCOMPARE(splitter->count(), 2);
    QCOMPARE(history->count(), 20);
    QVERIFY(history->item(0)->text().contains(QStringLiteral("config_20.cfte")));
    QVERIFY(history->item(0)->text().contains(QStringLiteral("group_20.txt")));
    QVERIFY(!history->item(0)->text().contains(QStringLiteral("个参数")));
}

void MainWindowTests::restoresHistoryWithoutRestoringTaskSpecificPaths()
{
    QTemporaryDir task_directory;
    QVERIFY(task_directory.isValid());
    const QString config_path = task_directory.filePath(QStringLiteral("history_config.cfte"));
    QVERIFY(writeTestFile(config_path, historyConfigText().toUtf8()));
    writeHistoryRecords({makeHistoryRecord(QStringLiteral("restore-single"),
                                           QDateTime::currentDateTime(),
                                           config_path,
                                           QString(),
                                           false,
                                           1)});

    QSettings settings;
    settings.setValue(QStringLiteral("paths/raw"), QStringLiteral("D:/current/raw.pcm"));
    settings.setValue(QStringLiteral("paths/output"), QStringLiteral("D:/current/result.txt"));
    settings.setValue(QStringLiteral("processing/includeRawValues"), true);
    settings.sync();

    MainWindow window;
    QListWidget *history = window.findChild<QListWidget *>(QStringLiteral("processingHistoryList"));
    QVERIFY(history != nullptr);
    history->setCurrentRow(0);
    QTest::mouseClick(window.findChild<QPushButton *>(QStringLiteral("historyApplyButton")), Qt::LeftButton);

    QCOMPARE(QFileInfo(window.findChild<QLineEdit *>(QStringLiteral("headerFileEdit"))->text()).absoluteFilePath(),
             QFileInfo(config_path).absoluteFilePath());
    QCOMPARE(window.findChild<QDoubleSpinBox *>(QStringLiteral("sampleRateSpin"))->value(), 2048.0);
    QVERIFY(window.findChild<QRadioButton *>(QStringLiteral("singleTimeRadio"))->isChecked());
    QCOMPARE(window.findChild<QTimeEdit *>(QStringLiteral("startTimeEdit"))->time(), QTime(1, 2, 3, 4));
    QCOMPARE(window.findChild<QTimeEdit *>(QStringLiteral("endTimeEdit"))->time(), QTime(2, 3, 4, 5));
    QCOMPARE(window.findChild<QLineEdit *>(QStringLiteral("rawFileEdit"))->text(),
             QStringLiteral("D:/current/raw.pcm"));
    QCOMPARE(window.findChild<QLineEdit *>(QStringLiteral("outputFileEdit"))->text(),
             QStringLiteral("D:/current/result.txt"));
    QVERIFY(window.findChild<QCheckBox *>(QStringLiteral("includeRawValuesCheck"))->isChecked());
    QCOMPARE(window.findChild<QListWidget *>(QStringLiteral("processingParameterList"))->count(), 2);
    QVERIFY(window.findChild<QLineEdit *>(QStringLiteral("parameterGroupFileEdit"))->text().isEmpty());
}

void MainWindowTests::requiresMultiRangeFileAfterHistoryRestore()
{
    QTemporaryDir task_directory;
    QVERIFY(task_directory.isValid());
    const QString config_path = task_directory.filePath(QStringLiteral("multi_history_config.cfte"));
    QVERIFY(writeTestFile(config_path, historyConfigText().toUtf8()));
    writeHistoryRecords({makeHistoryRecord(QStringLiteral("restore-multiple"),
                                           QDateTime::currentDateTime(),
                                           config_path,
                                           QString(),
                                           false,
                                           2)});
    QSettings settings;
    settings.setValue(QStringLiteral("paths/timeRanges"), QStringLiteral("D:/current/ranges.txt"));
    settings.setValue(QStringLiteral("processing/includeRawValues"), true);
    settings.sync();

    MainWindow window;
    QListWidget *history = window.findChild<QListWidget *>(QStringLiteral("processingHistoryList"));
    history->setCurrentRow(0);
    closeNextMessageBox(nullptr);
    QTest::mouseClick(window.findChild<QPushButton *>(QStringLiteral("historyApplyButton")), Qt::LeftButton);

    QVERIFY(window.findChild<QRadioButton *>(QStringLiteral("multipleTimeRadio"))->isChecked());
    QVERIFY(window.findChild<QLineEdit *>(QStringLiteral("timeRangeFileEdit"))->text().isEmpty());
    QVERIFY(window.findChild<QCheckBox *>(QStringLiteral("includeRawValuesCheck"))->isChecked());
}

void MainWindowTests::keepsHistoryBrowsableButReadOnlyWhileProcessing()
{
    QTemporaryDir task_directory;
    QVERIFY(task_directory.isValid());
    const QString config_path = task_directory.filePath(QStringLiteral("state_history_config.cfte"));
    QVERIFY(writeTestFile(config_path, historyConfigText().toUtf8()));
    const QString task_id = QStringLiteral("history-state");
    writeHistoryRecords({makeHistoryRecord(task_id,
                                           QDateTime::currentDateTime(),
                                           config_path,
                                           QString(),
                                           false)});

    MainWindow window;
    QListWidget *history = window.findChild<QListWidget *>(QStringLiteral("processingHistoryList"));
    history->setCurrentRow(0);
    QVERIFY(QMetaObject::invokeMethod(&window,
                                     "handleTraditionalTaskStarted",
                                     Qt::DirectConnection,
                                     Q_ARG(QString, task_id)));
    QVERIFY(history->isEnabled());
    QVERIFY(!window.findChild<QPushButton *>(QStringLiteral("historyApplyButton"))->isEnabled());
    QVERIFY(!window.findChild<QPushButton *>(QStringLiteral("historyDeleteButton"))->isEnabled());
    QVERIFY(!window.findChild<QPushButton *>(QStringLiteral("historyClearButton"))->isEnabled());

    QVERIFY(QMetaObject::invokeMethod(&window,
                                     "handleTraditionalTaskStopped",
                                     Qt::DirectConnection,
                                     Q_ARG(QString, task_id)));
    QVERIFY(history->item(0)->text().contains(QStringLiteral("中断")));
    QVERIFY(window.findChild<QPushButton *>(QStringLiteral("historyApplyButton"))->isEnabled());
}

void MainWindowTests::createsHistoryAfterValidatedStart()
{
    QTemporaryDir task_directory;
    QVERIFY(task_directory.isValid());
    const QString config_path = task_directory.filePath(QStringLiteral("start_history_config.cfte"));
    const QString raw_path = task_directory.filePath(QStringLiteral("raw.pcm"));
    const QString output_path = task_directory.filePath(QStringLiteral("result.txt"));
    QVERIFY(writeTestFile(config_path, historyConfigText().toUtf8()));
    QVERIFY(writeTestFile(raw_path, LegacyTestData::frame(0, 17)));
    writeHistoryRecords({makeHistoryRecord(QStringLiteral("starting-point"),
                                           QDateTime::currentDateTime().addSecs(-1),
                                           config_path,
                                           QString(),
                                           false)});

    MainWindow window;
    QListWidget *history = window.findChild<QListWidget *>(QStringLiteral("processingHistoryList"));
    history->setCurrentRow(0);
    QTest::mouseClick(window.findChild<QPushButton *>(QStringLiteral("historyApplyButton")), Qt::LeftButton);
    window.findChild<QLineEdit *>(QStringLiteral("rawFileEdit"))->setText(raw_path);
    window.findChild<QLineEdit *>(QStringLiteral("outputFileEdit"))->setText(output_path);
    const int old_count = history->count();

    const QSignalBlocker block_processing_signals(&window);
    QVERIFY(QMetaObject::invokeMethod(&window, "startProcessing", Qt::DirectConnection));
    QCOMPARE(history->count(), old_count + 1);
    QVERIFY(history->item(0)->text().contains(QStringLiteral("start_history_config.cfte")));
    QVERIFY(history->item(0)->text().contains(QStringLiteral("临时参数组")));
    QVERIFY(history->item(0)->text().contains(QStringLiteral("处理中")));
}

void MainWindowTests::tracksFileAndTemporaryParameterGroups()
{
    QTemporaryDir task_directory;
    QVERIFY(task_directory.isValid());
    const QString config_path = task_directory.filePath(QStringLiteral("group_source_config.cfte"));
    const QString group_path = task_directory.filePath(QStringLiteral("flight_group.txt"));
    const QString raw_path = task_directory.filePath(QStringLiteral("raw.pcm"));
    const QString output_path = task_directory.filePath(QStringLiteral("result.txt"));
    QVERIFY(writeTestFile(config_path, historyConfigText().toUtf8()));
    QVERIFY(writeTestFile(group_path, QByteArrayLiteral("TIME\nTEMP\n")));
    QVERIFY(writeTestFile(raw_path, LegacyTestData::frame(0, 17)));

    QSettings settings;
    settings.setValue(QStringLiteral("paths/config"), config_path);
    settings.setValue(QStringLiteral("paths/parameterGroup"), group_path);
    settings.setValue(QStringLiteral("paths/raw"), raw_path);
    settings.setValue(QStringLiteral("paths/output"), output_path);
    settings.sync();

    MainWindow window;
    QListWidget *history = window.findChild<QListWidget *>(QStringLiteral("processingHistoryList"));
    {
        const QSignalBlocker block_processing_signals(&window);
        QVERIFY(QMetaObject::invokeMethod(&window, "startProcessing", Qt::DirectConnection));
    }
    QCOMPARE(history->count(), 1);
    QVERIFY(history->item(0)->text().contains(QStringLiteral("flight_group.txt")));
    const QString first_task_id = history->item(0)->data(Qt::UserRole).toString();
    QVERIFY(QMetaObject::invokeMethod(&window,
                                     "handleTraditionalTaskStopped",
                                     Qt::DirectConnection,
                                     Q_ARG(QString, first_task_id)));

    QListWidget *processing_parameters =
        window.findChild<QListWidget *>(QStringLiteral("processingParameterList"));
    QListWidget *config_parameters = window.findChild<QListWidget *>(QStringLiteral("configParameterList"));
    processing_parameters->setCurrentRow(1);
    QVERIFY(QMetaObject::invokeMethod(&window,
                                     "removeSelectedProcessingParameters",
                                     Qt::DirectConnection));
    config_parameters->setCurrentRow(0);
    QVERIFY(QMetaObject::invokeMethod(&window, "addSelectedConfigParameters", Qt::DirectConnection));

    {
        const QSignalBlocker block_processing_signals(&window);
        QVERIFY(QMetaObject::invokeMethod(&window, "startProcessing", Qt::DirectConnection));
    }
    QCOMPARE(history->count(), 2);
    QVERIFY(history->item(0)->text().contains(QStringLiteral("临时参数组")));
}

void MainWindowTests::deletesAndClearsHistoryRecords()
{
    const QDateTime now = QDateTime::currentDateTime();
    writeHistoryRecords({
        makeHistoryRecord(QStringLiteral("delete-first"),
                          now,
                          QStringLiteral("D:/configs/first.cfte"),
                          QString(),
                          false),
        makeHistoryRecord(QStringLiteral("delete-second"),
                          now.addSecs(-1),
                          QStringLiteral("D:/configs/second.cfte"),
                          QString(),
                          false),
    });

    MainWindow window;
    window.show();
    QTest::qWait(20);
    QListWidget *history = window.findChild<QListWidget *>(QStringLiteral("processingHistoryList"));
    QCOMPARE(history->count(), 2);
    history->setCurrentRow(0);
    acceptNextQuestion();
    QTest::mouseClick(window.findChild<QPushButton *>(QStringLiteral("historyDeleteButton")), Qt::LeftButton);
    QCOMPARE(history->count(), 1);

    acceptNextQuestion();
    QTest::mouseClick(window.findChild<QPushButton *>(QStringLiteral("historyClearButton")), Qt::LeftButton);
    QCOMPARE(history->count(), 0);
    QVERIFY(!window.findChild<QPushButton *>(QStringLiteral("historyClearButton"))->isEnabled());
}

void MainWindowTests::restoresHistoryLogSplitterState()
{
    QByteArray expected_state;
    {
        MainWindow window;
        window.show();
        QTest::qWait(20);
        QSplitter *splitter = window.findChild<QSplitter *>(QStringLiteral("historyLogSplitter"));
        splitter->setSizes({300, 700});
        expected_state = splitter->saveState();
    }

    MainWindow restored_window;
    restored_window.show();
    QTest::qWait(20);
    QSplitter *restored_splitter =
        restored_window.findChild<QSplitter *>(QStringLiteral("historyLogSplitter"));
    QCOMPARE(restored_splitter->saveState(), expected_state);
}

QTEST_MAIN(MainWindowTests)

#include "main_window_tests.moc"
