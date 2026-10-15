#include "batch_processing_controller.h"
#include "processing_options_support.h"
#include "logger.h"
#include <QDateTime>
#include <QDir>
#include <QFileInfo>
#include <QRegularExpression>
#include <QTemporaryFile>
#include <QTimer>
#include <QSet>
#include <QJsonDocument>
#include <QJsonArray>
#include <QSaveFile>

namespace {
/** @brief 文件名片段使用跨平台安全字符，序号和唯一目录避免同名覆盖。 */
QString safeName(QString name)
{
    name.replace(QRegularExpression(QStringLiteral("[<>:\"/\\\\|?*\\x00-\\x1f]")), QStringLiteral("_"));
    while (name.endsWith('.') || name.endsWith(' ')) { name.chop(1); }
    return name.trimmed().isEmpty() ? QStringLiteral("任务") : name.left(48);
}
}

BatchProcessingController::BatchProcessingController(QObject *parent) : QObject(parent)
{
    qRegisterMetaType<BatchTaskRuntime>();
    connect(this, &BatchProcessingController::message, this, [this](const QString &text) {
        writeRunLog(text);
        Logger::instance().log(LogLevel::Info, QStringLiteral("BatchProcessing"), QString(), QString(), text);
    });
    connect(&Logger::instance(), &Logger::messageLogged, this, [this](const LogRecord &record) {
        if (!running_ || current_index_ < 0 || current_index_ >= plan_.tasks.size()) { return; }
        const QString id = plan_.tasks.at(current_index_).id;
        if (record.task_id == id || record.task_id.startsWith(id + QStringLiteral("_range_"))) {
            writeRunLog(QStringLiteral("[%1] [%2] [%3] %4")
                .arg(Logger::levelToString(record.level), record.module, record.task_id, record.message));
        }
    });
    connect(&controller_, &TraditionalProcessingController::taskProgressChanged, this,
            [this](const QString &id, int percent, const QString &detail) {
        if (!running_ || !runtime_.contains(id)) { return; }
        auto &state = runtime_[id];
        state.progress = qMax(state.progress, qBound(0, percent, 99));
        state.detail = detail;
        emit taskChanged(id, state);
        updateProgress();
    });
    connect(&controller_, &TraditionalProcessingController::rangeFinished, this,
            [this](const QString &id, int, int, const QString &path) {
        if (!running_ || !runtime_.contains(id)) { return; }
        runtime_[id].output_paths.append(path);
        emit taskChanged(id, runtime_.value(id));
    });
    connect(&controller_, &TraditionalProcessingController::processingSettled, this,
            [this](const QString &id, TraditionalProcessingController::ProcessingOutcome outcome,
                   const QStringList &paths, const QString &reason) {
        if (!running_ || !runtime_.contains(id)) { return; }
        auto &state = runtime_[id];
        state.output_paths = paths;
        if (outcome == TraditionalProcessingController::ProcessingOutcome::Succeeded) {
            state.state = BatchTaskState::Succeeded;
            state.progress = 100;
            state.detail = QStringLiteral("全部时间段结果已写入");
            ++succeeded_;
        } else if (outcome == TraditionalProcessingController::ProcessingOutcome::Failed) {
            state.state = BatchTaskState::Failed;
            state.detail = reason;
            ++failed_;
        } else {
            state.state = BatchTaskState::Stopped;
            state.detail = QStringLiteral("已中断，保留已提交的结果");
            stop_requested_ = true;
        }
        emit taskChanged(id, state);
        emit message(QStringLiteral("%1：%2 %3").arg(plan_.tasks.at(current_index_).name,
                                                     batchTaskStateText(state.state), state.detail));
        updateProgress();
        QTimer::singleShot(0, this, &BatchProcessingController::startNext);
    });
}

bool BatchProcessingController::isRunning() const { return running_; }
QString BatchProcessingController::outputDirectory() const { return output_directory_; }

QString BatchProcessingController::validateTask(const BatchTaskItem &item)
{
    if (item.name.trimmed().isEmpty()) { return QStringLiteral("请填写任务名称。"); }
    if (item.time_mode < 0 || item.time_mode > 2 ||
        (item.time_mode == 0 && !item.options.time_ranges.isEmpty()) ||
        (item.time_mode != 0 && (!item.time_ranges_resolved || item.options.time_ranges.isEmpty())) ||
        (item.time_mode == 1 && item.options.time_ranges.size() != 1)) {
        return QStringLiteral("时间设置不完整，请编辑任务补充时间段。");
    }
    QString error;
    controller_.validateOptions(item.processingOptions(), &error);
    return error;
}

QHash<QString, QString> BatchProcessingController::validatePlan(const BatchPlan &plan)
{
    QHash<QString, QString> errors;
    if (running_) { errors.insert(QString(), QStringLiteral("已有多任务方案正在运行。")); return errors; }
    cache_.reset(new ProcessingConfigCache);
    controller_.setConfigCache(cache_);
    if (plan.tasks.isEmpty()) { errors.insert(QString(), QStringLiteral("请先添加任务。")); }
    if (plan.name.trimmed().isEmpty()) { errors.insert(QString(), QStringLiteral("请填写方案名称。")); }
    QSet<QString> ids;
    for (const auto &item : plan.tasks) {
        if (item.id.isEmpty() || ids.contains(item.id)) {
            errors.insert(QString(), QStringLiteral("任务标识为空或重复。"));
        }
        ids.insert(item.id);
        const auto error = validateTask(item);
        if (!error.isEmpty()) { errors.insert(item.id, error); }
    }
    const QFileInfo root(plan.output_root);
    if (plan.output_root.trimmed().isEmpty() || !root.isDir()) {
        errors.insert(QString(), QStringLiteral("请选择有效的输出根目录。"));
    } else {
        QTemporaryFile probe(QDir(root.absoluteFilePath()).filePath(QStringLiteral(".pcm_write_check_XXXXXX")));
        if (!probe.open()) { errors.insert(QString(), QStringLiteral("输出根目录不可写：%1").arg(probe.errorString())); }
    }
    return errors;
}

bool BatchProcessingController::start(const BatchPlan &plan, QString *error)
{
    const auto errors = validatePlan(plan);
    if (!errors.isEmpty()) {
        QStringList messages;
        if (errors.contains(QString())) { messages << errors.value(QString()); }
        for (const auto &item : plan.tasks) {
            if (!errors.contains(item.id)) { continue; }
            BatchTaskRuntime state;
            state.state = BatchTaskState::NeedsAttention;
            state.detail = errors.value(item.id);
            emit taskChanged(item.id, state);
            messages << QStringLiteral("%1：%2").arg(item.name, state.detail);
        }
        if (error) { *error = messages.join(QLatin1Char('\n')); }
        if (!running_) { controller_.setConfigCache({}); cache_.clear(); }
        return false;
    }
    const QString folder = QStringLiteral("%1_%2_%3").arg(safeName(plan.name),
        QDateTime::currentDateTime().toString("yyyyMMdd_HHmmss_zzz"),
        QUuid::createUuid().toString(QUuid::WithoutBraces));
    const QDir root(ProcessingOptionsSupport::absolutePath(plan.output_root));
    if (!root.mkdir(folder)) {
        if (error) { *error = QStringLiteral("无法创建独立运行目录。"); }
        controller_.setConfigCache({}); cache_.clear();
        return false;
    }
    output_directory_ = root.filePath(folder);
    run_log_.setFileName(QDir(output_directory_).filePath(QStringLiteral("batch.log")));
    if (!run_log_.open(QIODevice::WriteOnly | QIODevice::Text)) {
        if (error) { *error = QStringLiteral("无法创建批次日志：%1").arg(run_log_.errorString()); }
        controller_.setConfigCache({}); cache_.clear();
        return false;
    }
    plan_ = plan;
    runtime_.clear();
    current_index_ = -1;
    succeeded_ = failed_ = 0;
    stop_requested_ = false;
    running_ = true;
    for (const auto &item : plan.tasks) {
        BatchTaskRuntime state;
        state.state = BatchTaskState::Waiting;
        runtime_.insert(item.id, state);
        emit taskChanged(item.id, state);
    }
    emit runningChanged(true);
    emit message(QStringLiteral("开始串行处理 %1 条任务，结果目录：%2").arg(plan.tasks.size()).arg(output_directory_));
    updateProgress();
    QTimer::singleShot(0, this, &BatchProcessingController::startNext);
    return true;
}

void BatchProcessingController::startNext()
{
    if (!running_) { return; }
    if (stop_requested_) {
        for (const auto &item : plan_.tasks) {
            if (runtime_[item.id].state == BatchTaskState::Waiting) {
                runtime_[item.id].state = BatchTaskState::NotRun;
                emit taskChanged(item.id, runtime_.value(item.id));
            }
        }
        finish();
        return;
    }
    ++current_index_;
    if (current_index_ >= plan_.tasks.size()) { finish(); return; }
    const auto item = plan_.tasks.at(current_index_);
    auto &state = runtime_[item.id];
    const QString folder = QStringLiteral("%1_%2").arg(current_index_ + 1, 3, 10,
        QLatin1Char('0')).arg(safeName(item.name));
    auto options = item.processingOptions();
    options.task_definition.output_directory = QDir(output_directory_).filePath(folder);
    options.output_file_path = QDir(options.task_definition.output_directory).filePath(QStringLiteral("result.txt"));
    QString error = validateTask(item);
    if (error.isEmpty() && !QDir(output_directory_).mkdir(folder)) { error = QStringLiteral("无法创建任务输出目录。"); }
    if (!error.isEmpty()) {
        state.state = BatchTaskState::Failed;
        state.detail = error;
        ++failed_;
        emit taskChanged(item.id, state);
        emit message(QStringLiteral("%1：失败，%2").arg(item.name, error));
        updateProgress();
        QTimer::singleShot(0, this, &BatchProcessingController::startNext);
        return;
    }
    state.state = BatchTaskState::Running;
    state.detail = QStringLiteral("正在启动");
    emit taskChanged(item.id, state);
    emit message(QStringLiteral("开始任务 %1/%2：%3").arg(current_index_ + 1).arg(plan_.tasks.size()).arg(item.name));
    controller_.startProcessing(options);
    if (stop_requested_) { controller_.requestStop(); }
}

void BatchProcessingController::requestStop()
{
    if (!running_ || stop_requested_) { return; }
    stop_requested_ = true;
    if (controller_.isBusy()) {
        if (current_index_ >= 0 && current_index_ < plan_.tasks.size()) {
            const QString id = plan_.tasks.at(current_index_).id;
            runtime_[id].state = BatchTaskState::Stopping;
            emit taskChanged(id, runtime_.value(id));
        }
        controller_.requestStop();
    }
    emit message(QStringLiteral("已请求中断，后续任务不再启动。"));
}

void BatchProcessingController::updateProgress()
{
    int progress_sum = 0;
    for (const auto &state : runtime_) {
        progress_sum += state.state == BatchTaskState::Succeeded || state.state == BatchTaskState::Failed
            ? 100 : state.progress;
    }
    emit progressChanged(plan_.tasks.isEmpty() ? 0 : qRound(double(progress_sum) / plan_.tasks.size()),
                         succeeded_, failed_, plan_.tasks.size());
}

void BatchProcessingController::finish()
{
    const QString summary = QStringLiteral("%1：成功 %2，失败 %3，总计 %4")
        .arg(stop_requested_ ? QStringLiteral("已中断") : QStringLiteral("处理结束"))
        .arg(succeeded_).arg(failed_).arg(plan_.tasks.size());
    emit message(summary);
    writeSummary();
    run_log_.close();
    running_ = false;
    controller_.setConfigCache({});
    cache_.clear();
    emit runningChanged(false);
    emit finished(succeeded_, failed_, stop_requested_, output_directory_);
}

void BatchProcessingController::writeRunLog(const QString &text)
{
    if (!run_log_.isOpen()) { return; }
    const QByteArray line = QStringLiteral("[%1] %2\n")
        .arg(QDateTime::currentDateTime().toString(Qt::ISODateWithMs), text).toUtf8();
    if (run_log_.write(line) != line.size() || !run_log_.flush()) {
        const QString reason = run_log_.errorString();
        run_log_.close();
        Logger::instance().log(LogLevel::Error, QStringLiteral("BatchProcessing"), QString(), QString(),
                               QStringLiteral("批次日志写入失败：%1").arg(reason));
    }
}

void BatchProcessingController::writeSummary()
{
    QJsonArray tasks;
    for (const auto &item : plan_.tasks) {
        const auto state = runtime_.value(item.id);
        tasks.append(QJsonObject{{"taskId", item.id}, {"name", item.name},
            {"status", batchTaskStateText(state.state)}, {"detail", state.detail},
            {"outputPaths", QJsonArray::fromStringList(state.output_paths)}});
    }
    const QJsonObject summary{{"planName", plan_.name}, {"succeeded", succeeded_}, {"failed", failed_},
                             {"interrupted", stop_requested_}, {"tasks", tasks}};
    QSaveFile file(QDir(output_directory_).filePath(QStringLiteral("batch-summary.json")));
    const auto bytes = QJsonDocument(summary).toJson(QJsonDocument::Indented);
    if (!file.open(QIODevice::WriteOnly) || file.write(bytes) != bytes.size() || !file.commit()) {
        emit message(QStringLiteral("结果已保留，但批次汇总保存失败：%1").arg(file.errorString()));
    }
}
