#include "batch_processing_controller.h"
#include "legacy_test_data.h"
#include <QDir>
#include <QSignalSpy>
#include <QTemporaryDir>
#include <QtTest>

namespace {
/** @brief 创建预期值明确的两帧任务快照，原始数据与配置位于临时目录。 */
BatchTaskItem makeTask(const QTemporaryDir &dir, const QString &name, int first_value = 17)
{
    BatchTaskItem item; item.name = name;
    item.options.task_definition.config_file_path = dir.filePath("config.cfte");
    item.options.task_definition.raw_file_path = dir.filePath(name + ".pcm");
    item.options.task_definition.user_sample_rate_hz = 1;
    item.options.parameter_names = QStringList{QStringLiteral("TIME"), QStringLiteral("TEMP")};
    LegacyTestData::write(item.options.task_definition.config_file_path, LegacyTestData::config().toUtf8());
    LegacyTestData::write(item.options.task_definition.raw_file_path,
                         LegacyTestData::frame(0, first_value) + LegacyTestData::frame(1, first_value + 1));
    return item;
}
/** @brief 读取已提交 TXT 用于隔离断言。 */
QByteArray read(const QString &path)
{
    QFile file(path); return file.open(QIODevice::ReadOnly) ? file.readAll() : QByteArray();
}
}

/** @brief 使用真实解算控制器验证批次顺序、结果隔离及终态时序。 */
class BatchProcessingControllerTests : public QObject
{
    Q_OBJECT
private slots:
    /** @brief 多时间段全部提交后再派发下一条，重跑创建不同目录。 */
    void serializesRangesAndSeparatesReruns();
    /** @brief 预检通过后丢失文件及解码失败都继续后续任务。 */
    void continuesAfterRuntimeFailures();
    /** @brief 预检收集所有任务错误，不创建运行目录。 */
    void preflightRejectsAllInvalidTasks();
    /** @brief 中断保留已提交范围，后续任务不启动。 */
    void stopsWithoutDispatchingNextTask();
    /** @brief 接受批次后立即中断，所有任务仍未执行。 */
    void stopsBeforeFirstDispatch();
    /** @brief settled 恰好一次且发出时资源空闲，可立即复用控制器。 */
    void settlesFailureBeforeRestart();
};

void BatchProcessingControllerTests::serializesRangesAndSeparatesReruns()
{
    QTemporaryDir dir; BatchPlan plan; plan.output_root = dir.path();
    auto a = makeTask(dir, "first", 17); a.time_mode = 2; a.options.time_ranges = {{0, 1}, {1, 2}};
    const auto b = makeTask(dir, "second", 34); plan.tasks = {a, b};
    BatchProcessingController controller; QSignalSpy done(&controller, &BatchProcessingController::finished);
    QHash<QString, BatchTaskRuntime> states; QStringList started; bool first_complete_before_second = false;
    connect(&controller, &BatchProcessingController::taskChanged, this, [&](const QString &id, const
        BatchTaskRuntime &state) {
        if (state.state == BatchTaskState::Running && !started.contains(id)) {
            started << id;
            if (id == b.id) { first_complete_before_second = states.value(a.id).output_paths.size() == 2 &&
                states.value(a.id).state == BatchTaskState::Succeeded; }
        }
        states[id] = state;
    });
    QString error; QVERIFY2(controller.start(plan, &error), qPrintable(error));
    QTRY_COMPARE_WITH_TIMEOUT(done.size(), 1, 15000);
    QVERIFY(first_complete_before_second); QCOMPARE(started, QStringList({a.id, b.id}));
    QCOMPARE(done.first().at(0).toInt(), 2); QCOMPARE(done.first().at(1).toInt(), 0);
    QVERIFY(read(states[a.id].output_paths.first()).contains("\t17"));
    QVERIFY(read(states[b.id].output_paths.first()).contains("\t34"));
    QVERIFY(!read(states[b.id].output_paths.first()).contains("\t17"));
    QVERIFY(QFileInfo::exists(QDir(controller.outputDirectory()).filePath("batch-summary.json")));
    QVERIFY(!read(QDir(controller.outputDirectory()).filePath("batch.log")).isEmpty());
    const auto first_directory = controller.outputDirectory(); const auto first_file =
        states[a.id].output_paths.first();
    const auto first_content = read(first_file);
    QVERIFY(controller.start(plan, &error)); QTRY_COMPARE_WITH_TIMEOUT(done.size(), 2, 15000);
    QVERIFY(controller.outputDirectory() != first_directory); QCOMPARE(read(first_file), first_content);
}

void BatchProcessingControllerTests::continuesAfterRuntimeFailures()
{
    QTemporaryDir dir; BatchPlan plan; plan.output_root = dir.path();
    const auto a = makeTask(dir, "first"); const auto missing = makeTask(dir, "missing");
    const auto malformed = makeTask(dir, "regression"); const auto last = makeTask(dir, "last", 60);
    QVERIFY(LegacyTestData::write(malformed.options.task_definition.raw_file_path,
                                 LegacyTestData::frame(2, 1) + LegacyTestData::frame(1, 2)));
    plan.tasks = {a, missing, malformed, last};
    BatchProcessingController controller; QSignalSpy done(&controller, &BatchProcessingController::finished);
    QHash<QString, BatchTaskRuntime> states;
    connect(&controller, &BatchProcessingController::taskChanged, this, [&](const QString &id, const
        BatchTaskRuntime &state) {
        states[id] = state;
        if (id == a.id && state.state == BatchTaskState::Succeeded) {
            QFile::remove(missing.options.task_definition.raw_file_path); }
    });
    QString error; QVERIFY2(controller.start(plan, &error), qPrintable(error));
    QTRY_COMPARE_WITH_TIMEOUT(done.size(), 1, 15000);
    QCOMPARE(done.first().at(0).toInt(), 2); QCOMPARE(done.first().at(1).toInt(), 2);
    QCOMPARE(states[missing.id].state, BatchTaskState::Failed); QVERIFY(!states[missing.id].detail.isEmpty());
    QCOMPARE(states[malformed.id].state, BatchTaskState::Failed);
    QCOMPARE(states[last.id].state, BatchTaskState::Succeeded);
}

void BatchProcessingControllerTests::preflightRejectsAllInvalidTasks()
{
    QTemporaryDir dir; BatchPlan plan; plan.output_root = dir.path();
    auto a = makeTask(dir, "first"); auto b = makeTask(dir, "second");
    a.options.task_definition.user_sample_rate_hz = 0;
    b.options.task_definition.raw_file_path = dir.filePath("not-there.pcm"); plan.tasks = {a, b};
    BatchProcessingController controller; const auto errors = controller.validatePlan(plan);
    QVERIFY(errors.contains(a.id)); QVERIFY(errors.contains(b.id));
    QString error; QVERIFY(!controller.start(plan, &error)); QVERIFY(!controller.isRunning());
    QVERIFY(controller.outputDirectory().isEmpty());
    QCOMPARE(QDir(dir.path()).entryList(QDir::Dirs | QDir::NoDotAndDotDot).size(), 0);
}

void BatchProcessingControllerTests::stopsWithoutDispatchingNextTask()
{
    QTemporaryDir dir; BatchPlan plan; plan.output_root = dir.path();
    auto a = makeTask(dir, "first"); a.time_mode = 2; a.options.time_ranges = {{0, 1}, {1, 2}};
    const auto b = makeTask(dir, "second"); plan.tasks = {a, b};
    BatchProcessingController controller; QSignalSpy done(&controller, &BatchProcessingController::finished);
    QHash<QString, BatchTaskRuntime> states; bool requested = false;
    connect(&controller, &BatchProcessingController::taskChanged, this, [&](const QString &id, const
        BatchTaskRuntime &state) {
        states[id] = state;
        if (id == a.id && !state.output_paths.isEmpty() && !requested) { requested = true; controller.requestStop(); }
    });
    QString error; QVERIFY(controller.start(plan, &error)); QTRY_COMPARE_WITH_TIMEOUT(done.size(), 1, 15000);
    QVERIFY(done.first().at(2).toBool()); QCOMPARE(states[b.id].state, BatchTaskState::NotRun);
    QCOMPARE(states[a.id].output_paths.size(), 1); QVERIFY(QFileInfo::exists(states[a.id].output_paths.first()));
}

void BatchProcessingControllerTests::stopsBeforeFirstDispatch()
{
    QTemporaryDir dir; BatchPlan plan; plan.output_root = dir.path(); plan.tasks = {makeTask(dir, "first")};
    BatchProcessingController controller; QSignalSpy done(&controller, &BatchProcessingController::finished);
    BatchTaskState last = BatchTaskState::Ready;
    connect(&controller, &BatchProcessingController::taskChanged, this, [&](const QString &, const BatchTaskRuntime
        &state) {
        last = state.state;
    });
    QString error; QVERIFY(controller.start(plan, &error)); controller.requestStop();
    QTRY_COMPARE(done.size(), 1); QCOMPARE(last, BatchTaskState::NotRun); QCOMPARE(done.first().at(0).toInt(), 0);
}

void BatchProcessingControllerTests::settlesFailureBeforeRestart()
{
    QTemporaryDir dir; auto task = makeTask(dir, "first"); auto options = task.processingOptions();
    options.output_file_path = dir.filePath("result.txt");
    QVERIFY(LegacyTestData::write(options.task_definition.raw_file_path,
                                 LegacyTestData::frame(2, 1) + LegacyTestData::frame(1, 2)));
    TraditionalProcessingController controller; int settled = 0; bool busy_on_settle = false;
    connect(&controller, &TraditionalProcessingController::processingSettled, this,
            [&](const QString &, TraditionalProcessingController::ProcessingOutcome, const QStringList &, const
                QString &) {
        busy_on_settle |= controller.isBusy(); ++settled;
        if (settled == 1) {
            LegacyTestData::write(options.task_definition.raw_file_path, LegacyTestData::frame(1, 17));
            controller.startProcessing(options);
        }
    });
    controller.startProcessing(options); QTRY_COMPARE_WITH_TIMEOUT(settled, 2, 15000);
    QVERIFY(!busy_on_settle); QVERIFY(!controller.isBusy()); QVERIFY(read(options.output_file_path).contains("\t17"));
}

QTEST_GUILESS_MAIN(BatchProcessingControllerTests)
#include "batch_processing_controller_tests.moc"
