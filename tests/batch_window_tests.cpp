#include "batch_processing_page.h"
#include "batch_task_editor.h"
#include "main_window.h"
#include "processing_history_store.h"
#include "legacy_test_data.h"
#include <QApplication>
#include <QDialogButtonBox>
#include <QDoubleSpinBox>
#include <QLineEdit>
#include <QLabel>
#include <QMessageBox>
#include <QPushButton>
#include <QSettings>
#include <QSignalSpy>
#include <QTableView>
#include <QTableWidget>
#include <QTabWidget>
#include <QTemporaryDir>
#include <QTimer>
#include <QtTest>

/** @brief 使用真实 Qt Widgets 操作验证多任务交互及跨页面互斥。 */
class BatchWindowTests : public QObject
{
    Q_OBJECT
private slots:
    /** @brief 测试专用设置目录，避免触碰用户历史。 */
    void initTestCase();
    /** @brief 每个用例清空隔离设置。 */
    void init();
    /** @brief 勾选多条历史并允许更换原始数据、采样率，历史保持不变。 */
    void importsHistoryAndEditsIndependentCopies();
    /** @brief 手动编辑器取消不写回，接受时保存时间与参数快照。 */
    void manualEditorCommitsOnlyOnAccept();
    /** @brief 打开损坏文件保留当前方案，成功打开不自动执行。 */
    void opensAndSavesWithoutExecuting();
    /** @brief 保存提示取消时不丢失未保存修改。 */
    void protectsUnsavedChanges();
    /** @brief 单任务和批次处理的启动入口互斥。 */
    void excludesSimultaneousSingleAndBatchStarts();
    /** @brief 渲染真实主窗口和任务编辑器，检查最小窗口尺寸。 */
    void rendersWorkspace();
    /** @brief 快速切换任务时旧扫描不会覆盖新原始数据的时间范围。 */
    void scansLatestSelection();
private:
    /** @brief 创建真实可解算的小型任务。 */
    BatchTaskItem fixture(const QString &name);
    QTemporaryDir settings_dir_;
    QTemporaryDir data_dir_;
};

void BatchWindowTests::initTestCase()
{
    QCoreApplication::setOrganizationName("BatchWindowTests");
    QCoreApplication::setApplicationName("Isolated");
    QSettings::setDefaultFormat(QSettings::IniFormat);
    QSettings::setPath(QSettings::IniFormat, QSettings::UserScope, settings_dir_.path());
}
void BatchWindowTests::init() { QSettings settings; settings.clear(); settings.sync(); }

BatchTaskItem BatchWindowTests::fixture(const QString &name)
{
    BatchTaskItem item; item.name = name; item.source_description = QStringLiteral("手动添加");
    item.options.task_definition.config_file_path = data_dir_.filePath("config.cfte");
    item.options.task_definition.raw_file_path = data_dir_.filePath(name + ".pcm");
    item.options.task_definition.user_sample_rate_hz = 1;
    item.options.parameter_names = QStringList{QStringLiteral("TIME"), QStringLiteral("TEMP")};
    LegacyTestData::write(item.options.task_definition.config_file_path, LegacyTestData::config().toUtf8());
    LegacyTestData::write(item.options.task_definition.raw_file_path, LegacyTestData::frame(0, 17) +
        LegacyTestData::frame(1, 18));
    return item;
}

void BatchWindowTests::importsHistoryAndEditsIndependentCopies()
{
    const auto a = fixture("flight01"); const auto b = fixture("flight02");
    ProcessingHistoryEntry first; first.task_id = "old-1"; first.started_at = QDateTime::currentDateTime();
    first.config_path = a.options.task_definition.config_file_path;
    first.raw_file_path = a.options.task_definition.raw_file_path; first.sample_rate_hz = 1;
    first.parameter_names = a.options.parameter_names; first.status = QStringLiteral("完成");
    auto second = first; second.task_id = "old-2"; second.raw_file_path = b.options.task_definition.raw_file_path;
    second.started_at = first.started_at.addSecs(-10);
    QVERIFY(ProcessingHistoryStore::save({first, second}));
    BatchProcessingPage page; page.show();
    QTimer::singleShot(0, &page, [&]() {
        auto *dialog = page.findChild<QDialog *>("batchHistoryDialog");
        QVERIFY(dialog); auto *table = dialog->findChild<QTableWidget *>("batchHistoryTable"); QVERIFY(table);
        QCOMPARE(table->rowCount(), 2); table->item(0, 0)->setCheckState(Qt::Checked);
        table->item(1, 0)->setCheckState(Qt::Checked);
        dialog->findChild<QPushButton *>("batchHistoryAccept")->click();
    });
    page.findChild<QPushButton *>("batchHistoryButton")->click();
    QCOMPARE(page.model()->rowCount(), 2);
    QVERIFY(page.model()->task(0).id != first.task_id);
    auto *rate = page.findChild<QDoubleSpinBox *>("batchRateSpin"); rate->setValue(50); emit rate->editingFinished();
    QCOMPARE(page.model()->task(0).options.task_definition.user_sample_rate_hz, 50.0);
    auto *raw = page.findChild<QLineEdit *>("batchRawEdit"); raw->setText(b.options.task_definition.raw_file_path);
    emit raw->editingFinished();
    QCOMPARE(page.model()->task(0).options.task_definition.raw_file_path, b.options.task_definition.raw_file_path);
    const auto unchanged = ProcessingHistoryStore::load().first();
    QCOMPARE(unchanged.sample_rate_hz, 1.0); QCOMPARE(unchanged.raw_file_path, first.raw_file_path);
}

void BatchWindowTests::manualEditorCommitsOnlyOnAccept()
{
    const auto item = fixture("manual");
    {
        BatchTaskEditor editor(item, false);
        editor.findChild<QLineEdit *>("taskNameEdit")->setText("changed"); editor.reject();
        QCOMPARE(editor.task().name, item.name);
    }
    BatchTaskEditor editor(item, true);
    editor.findChild<QLineEdit *>("taskNameEdit")->setText(QStringLiteral("手动新架次"));
    editor.findChild<QDoubleSpinBox *>("taskRateSpin")->setValue(100);
    editor.accept(); QCOMPARE(editor.result(), int(QDialog::Accepted));
    QCOMPARE(editor.task().name, QStringLiteral("手动新架次"));
    QCOMPARE(editor.task().options.parameter_names, item.options.parameter_names);
    QCOMPARE(editor.task().options.task_definition.user_sample_rate_hz, 100.0);
}

void BatchWindowTests::opensAndSavesWithoutExecuting()
{
    BatchProcessingPage page; page.model()->appendTask(fixture("save"));
    page.model()->setMetadata(QStringLiteral("保存测试"), data_dir_.path());
    const auto path = data_dir_.filePath(QStringLiteral("任务方案.txt")); QString error;
    QVERIFY(page.savePlan(path, &error)); QVERIFY(!page.model()->isModified());
    const auto id = page.model()->task(0).id;
    const auto broken = data_dir_.filePath("broken.txt"); QVERIFY(LegacyTestData::write(broken, "{broken"));
    QVERIFY(!page.loadPlan(broken, &error)); QCOMPARE(page.model()->task(0).id, id);
    QSignalSpy running(page.controller(), &BatchProcessingController::runningChanged);
    QVERIFY(page.loadPlan(path, &error)); QCOMPARE(running.size(), 0); QVERIFY(!page.controller()->isRunning());
    QCOMPARE(page.model()->task(0).id, id); QVERIFY(!page.model()->isModified());
}

void BatchWindowTests::protectsUnsavedChanges()
{
    BatchProcessingPage page; page.model()->appendTask(fixture("unsaved"));
    QTimer::singleShot(0, &page, [&]() {
        auto *box = page.findChild<QMessageBox *>(); QVERIFY(box);
        for (auto *button : box->buttons()) { if (box->buttonRole(button) == QMessageBox::RejectRole) {
            button->click(); return; } }
    });
    QVERIFY(!page.confirmSaveChanges()); QVERIFY(page.model()->isModified()); QCOMPARE(page.model()->rowCount(), 1);
}

void BatchWindowTests::excludesSimultaneousSingleAndBatchStarts()
{
    MainWindow window; auto *page = window.findChild<BatchProcessingPage *>(); QVERIFY(page);
    BatchPlan plan; plan.output_root = data_dir_.path(); plan.tasks = {fixture("exclusive")};
    QSignalSpy requested(&window, &MainWindow::traditionalProcessingRequested);
    QSignalSpy done(page->controller(), &BatchProcessingController::finished);
    QString error; QVERIFY(page->controller()->start(plan, &error));
    QVERIFY(!window.findChild<QPushButton *>("startButton")->isEnabled());
    QVERIFY(QMetaObject::invokeMethod(&window, "startProcessing")); QCOMPARE(requested.size(), 0);
    QTRY_COMPARE_WITH_TIMEOUT(done.size(), 1, 15000);
    QVERIFY(window.findChild<QPushButton *>("startButton")->isEnabled());
    page->model()->setPlan(plan);
    QVERIFY(QMetaObject::invokeMethod(&window, "handleTraditionalTaskStarted", Q_ARG(QString, QString("single-test"))));
    QVERIFY(!page->findChild<QPushButton *>("batchStartButton")->isEnabled());
    page->startProcessing(); QCOMPARE(done.size(), 1);
}

void BatchWindowTests::rendersWorkspace()
{
    MainWindow window; auto *page = window.findChild<BatchProcessingPage *>();
    BatchPlan plan; plan.name = QStringLiteral("架次批处理_0923"); plan.output_root = data_dir_.path();
    auto a = fixture(QStringLiteral("架次001")); auto b = fixture(QStringLiteral("架次002"));
    b.time_mode = 2; b.options.time_ranges = {{0, 1}, {1, 2}};
    auto c = fixture(QStringLiteral("架次003")); c.options.task_definition.raw_file_path =
        data_dir_.filePath("missing.pcm");
    plan.tasks = {a, b, c}; QString error; const auto path = data_dir_.filePath("preview.txt");
    QVERIFY(BatchPlanFile::save(path, plan, &error)); QVERIFY(page->loadPlan(path, &error));
    window.findChild<QTabWidget *>()->setCurrentIndex(1); window.resize(1100, 820); window.show();
    QTest::qWait(200); QVERIFY(window.grab().save("batch-workspace.png"));
    window.resize(900, 620); QTest::qWait(100); QVERIFY(window.grab().save("batch-workspace-minimum.png"));
    BatchTaskEditor editor(b, false); editor.show(); QTest::qWait(150);
    QVERIFY(editor.grab().save("batch-task-editor.png"));
    QVERIFY(page->findChild<QPushButton *>("batchStartButton")->isVisible());
    QVERIFY(page->findChild<QTableView *>("batchTaskTable")->height() >= 100);
}

void BatchWindowTests::scansLatestSelection()
{
    BatchProcessingPage page;
    const auto first = fixture("scan-first");
    const auto second = fixture("scan-second");
    QVERIFY(LegacyTestData::write(second.options.task_definition.raw_file_path,
                                 LegacyTestData::frame(10, 1) + LegacyTestData::frame(11, 2)));
    page.model()->appendTask(first); page.model()->appendTask(second);
    auto *table = page.findChild<QTableView *>("batchTaskTable");
    table->selectRow(0); table->selectRow(1);
    auto *label = page.findChild<QLabel *>("batchTimeRangeLabel");
    QTRY_VERIFY_WITH_TIMEOUT(label->text().contains("00:00:10.000"), 10000);
    QVERIFY(label->text().contains("00:00:12.000"));
}

QTEST_MAIN(BatchWindowTests)
#include "batch_window_tests.moc"
