#include "batch_plan.h"
#include "batch_task_model.h"
#include "processing_history_store.h"
#include "processing_config_cache.h"
#include "legacy_test_data.h"
#include <QAbstractItemModelTester>
#include <QJsonArray>
#include <QJsonDocument>
#include <QSettings>
#include <QSignalSpy>
#include <QTemporaryDir>
#include <QtTest>

/** @brief 方案往返、事务式加载、历史兼容及模型锁定回归。 */
class BatchPlanTests : public QObject
{
    Q_OBJECT
private slots:
    /** @brief 中文路径和多时间段快照完整往返，缺失文件允许保留。 */
    void roundTripsSnapshots();
    /** @brief 损坏、未知版本、重复标识与字段错误不替换现有文档。 */
    void rejectsInvalidDocumentsTransactionally();
    /** @brief 写入失败不破坏已有方案。 */
    void preservesFileOnSaveFailure();
    /** @brief 状态更新不污染文档，编辑和排序受运行锁保护。 */
    void keepsModelAndRuntimeIndependent();
    /** @brief 旧记录从辅助文件解析，新增快照不依赖辅助文件仍存在。 */
    void restoresOldAndNewHistory();
    /** @brief 相同配置共享对象，文件内容改变时失效，旧引用继续有效。 */
    void invalidatesConfigCacheByContent();
};

void BatchPlanTests::roundTripsSnapshots()
{
    QTemporaryDir dir;
    BatchPlan source; source.name = QStringLiteral("架次批处理"); source.output_root = dir.path();
    BatchTaskItem item; item.name = QStringLiteral("架次一"); item.source_description = QStringLiteral("历史记录");
    item.options.task_definition.config_file_path = dir.filePath(QStringLiteral("配置.cfte"));
    item.options.task_definition.raw_file_path = dir.filePath(QStringLiteral("缺失数据.pcm"));
    item.options.task_definition.user_sample_rate_hz = 100;
    item.options.parameter_names = QStringList{QStringLiteral("TIME"), QStringLiteral("TEMP")};
    item.options.time_ranges = {{0, 10}, {20, 30}}; item.time_mode = 2;
    item.options.include_raw_values = true;
    source.tasks = {item};
    BatchTaskItem draft; draft.name = QStringLiteral("待补充"); draft.time_mode = 2; draft.time_ranges_resolved = false;
    source.tasks.append(draft);
    QString error; const auto path = dir.filePath(QStringLiteral("方案.txt"));
    QVERIFY2(BatchPlanFile::save(path, source, &error), qPrintable(error));
    BatchPlan loaded; QVERIFY2(BatchPlanFile::load(path, &loaded, &error), qPrintable(error));
    QCOMPARE(loaded.name, source.name); QCOMPARE(loaded.tasks.size(), 2);
    const auto result = loaded.tasks.first();
    QCOMPARE(result.id, item.id); QCOMPARE(result.options.task_definition.raw_file_path,
        item.options.task_definition.raw_file_path);
    QCOMPARE(result.options.parameter_names, item.options.parameter_names);
    QCOMPARE(result.options.task_definition.user_sample_rate_hz, 100.0);
    QCOMPARE(result.options.time_ranges.size(), 2); QCOMPARE(result.options.time_ranges.last().end_seconds, 30.0);
    QVERIFY(result.options.include_raw_values); QVERIFY(!loaded.tasks.last().time_ranges_resolved);
    QVERIFY(result.options.output_file_path.isEmpty());
}

void BatchPlanTests::rejectsInvalidDocumentsTransactionally()
{
    QTemporaryDir dir; const auto path = dir.filePath("plan.txt");
    BatchPlan original; original.name = "keep";
    QVERIFY(LegacyTestData::write(path, "broken")); QString error;
    QVERIFY(!BatchPlanFile::load(path, &original, &error)); QCOMPARE(original.name, QString("keep"));
    BatchPlan valid; BatchTaskItem item; item.name = "flight"; valid.tasks = {item};
    QVERIFY(BatchPlanFile::save(path, valid, &error)); QFile file(path); QVERIFY(file.open(QIODevice::ReadOnly));
    auto root = QJsonDocument::fromJson(file.readAll()).object(); file.close();
    const auto good = root;
    root["version"] = 999;
    QVERIFY(LegacyTestData::write(path, QJsonDocument(root).toJson()));
    QVERIFY(!BatchPlanFile::load(path, &original, &error)); QCOMPARE(original.name, QString("keep"));
    root = good; auto tasks = root["tasks"].toArray(); tasks.append(tasks.first()); root["tasks"] = tasks;
    QVERIFY(LegacyTestData::write(path, QJsonDocument(root).toJson()));
    QVERIFY(!BatchPlanFile::load(path, &original, &error)); QCOMPARE(original.name, QString("keep"));
    root = good; auto task = root["tasks"].toArray().first().toObject(); task["parameterNames"] = "wrong type";
    root["tasks"] = QJsonArray{task}; QVERIFY(LegacyTestData::write(path, QJsonDocument(root).toJson()));
    QVERIFY(!BatchPlanFile::load(path, &original, &error)); QCOMPARE(original.name, QString("keep"));
}

void BatchPlanTests::preservesFileOnSaveFailure()
{
    QTemporaryDir dir; const auto path = dir.filePath("plan.txt");
    BatchPlan plan; plan.name = "original"; QString error;
    QVERIFY(BatchPlanFile::save(path, plan, &error));
    BatchTaskItem item; plan.tasks = {item, item};
    QVERIFY(!BatchPlanFile::save(path, plan, &error));
    BatchPlan loaded; QVERIFY(BatchPlanFile::load(path, &loaded, &error)); QCOMPARE(loaded.name, QString("original"));
    QVERIFY(!BatchPlanFile::save(dir.path(), loaded, &error));
}

void BatchPlanTests::keepsModelAndRuntimeIndependent()
{
    BatchTaskModel model;
    QAbstractItemModelTester tester(&model, QAbstractItemModelTester::FailureReportingMode::QtTest);
    BatchTaskItem a; a.name = "A"; BatchTaskItem b; b.name = "B";
    model.appendTask(a); model.appendTask(b); QVERIFY(model.isModified()); model.markSaved();
    BatchTaskRuntime running; running.state = BatchTaskState::Running; running.progress = 47;
    model.setRuntime(a.id, running); QVERIFY(!model.isModified());
    model.moveTask(0, 1); QCOMPARE(model.task(1).id, a.id); QCOMPARE(model.runtime(a.id).progress, 47);
    model.setLocked(true); model.removeTask(1); model.moveTask(1, 0); model.appendTask(BatchTaskItem());
    model.setMetadata("should not change", "");
    QCOMPARE(model.rowCount(), 2); QCOMPARE(model.task(1).id, a.id);
    model.setLocked(false); auto edited = model.task(1); edited.options.task_definition.user_sample_rate_hz = 50;
    model.replaceTask(1, edited); QCOMPARE(model.runtime(a.id).progress, 0);
    QCOMPARE(model.task(1).options.task_definition.user_sample_rate_hz, 50.0);
    model.setPlan(model.plan()); QVERIFY(!model.isModified()); QCOMPARE(model.runtime(a.id).state,
        BatchTaskState::Ready);
}

void BatchPlanTests::restoresOldAndNewHistory()
{
    QTemporaryDir dir; QCoreApplication::setOrganizationName("BatchPlanTests");
    QCoreApplication::setApplicationName("History"); QSettings::setDefaultFormat(QSettings::IniFormat);
    QSettings::setPath(QSettings::IniFormat, QSettings::UserScope, dir.path());
    ProcessingHistoryEntry record; record.task_id = "old"; record.started_at = QDateTime::currentDateTime();
    record.config_path = dir.filePath("config.cfte"); record.raw_file_path = dir.filePath("raw.pcm");
    record.parameter_names = QStringList{QStringLiteral("TIME"), QStringLiteral("TEMP")}; record.sample_rate_hz = 50;
    record.time_mode = 2; record.time_range_file_path = dir.filePath("ranges.txt");
    QVERIFY(LegacyTestData::write(record.time_range_file_path, "1 2\n3 4\n"));
    QVERIFY(ProcessingHistoryStore::save({record}));
    const auto old = ProcessingHistoryStore::load().first(); const auto imported = old.toBatchTask();
    QCOMPARE(imported.options.time_ranges.size(), 2); QVERIFY(imported.time_ranges_resolved);
    QVERIFY(imported.id != record.task_id); QVERIFY(imported.id != old.toBatchTask().id);
    record.time_ranges = imported.options.time_ranges; record.has_time_ranges_snapshot = true;
    QVERIFY(ProcessingHistoryStore::save({record})); QVERIFY(QFile::remove(record.time_range_file_path));
    const auto snapshotted = ProcessingHistoryStore::load().first().toBatchTask();
    QVERIFY(snapshotted.time_ranges_resolved); QCOMPARE(snapshotted.options.time_ranges.last().end_seconds, 4.0);
    QVERIFY(!old.toBatchTask().time_ranges_resolved);
}

void BatchPlanTests::invalidatesConfigCacheByContent()
{
    QTemporaryDir dir; const auto path = dir.filePath("config.cfte");
    QVERIFY(LegacyTestData::write(path, LegacyTestData::config().toUtf8()));
    ProcessingConfigCache cache; QString error;
    const auto first = cache.load(path, &error); QVERIFY2(first, qPrintable(error));
    const auto repeated = cache.load(path, &error); QCOMPARE(first.data(), repeated.data());
    QVERIFY(LegacyTestData::write(path, LegacyTestData::config(1, 2).toUtf8()));
    const auto changed = cache.load(path, &error); QVERIFY2(changed, qPrintable(error));
    QVERIFY(first.data() != changed.data());
    QCOMPARE(first->layout_tables.first().major_frame_rate_hz, 1.0);
    QCOMPARE(changed->layout_tables.first().major_frame_rate_hz, 2.0);
    cache.clear(); QCOMPARE(first->layout_tables.first().major_frame_rate_hz, 1.0);
}

QTEST_GUILESS_MAIN(BatchPlanTests)
#include "batch_plan_tests.moc"
