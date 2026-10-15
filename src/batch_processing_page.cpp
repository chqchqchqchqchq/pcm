#include "batch_processing_page.h"
#include "batch_task_editor.h"
#include "processing_history_store.h"
#include "processing_options_support.h"
#include <QDesktopServices>
#include <QDialog>
#include <QDialogButtonBox>
#include <QDoubleSpinBox>
#include <QFileDialog>
#include <QFileInfo>
#include <QFormLayout>
#include <QHeaderView>
#include <QLabel>
#include <QLineEdit>
#include <QMessageBox>
#include <QPainter>
#include <QPlainTextEdit>
#include <QProgressBar>
#include <QPushButton>
#include <QScrollArea>
#include <QSignalBlocker>
#include <QSplitter>
#include <QStyledItemDelegate>
#include <QStyleOptionProgressBar>
#include <QTableView>
#include <QTableWidget>
#include <QTimer>
#include <QUrl>
#include <QVBoxLayout>
#include <QtConcurrent>
#include <exception>

namespace {
/** @brief 用原生 Qt 进度条绘制单任务进度，保持平台外观一致。 */
class TaskProgressDelegate : public QStyledItemDelegate
{
public:
    /** @brief 父视图管理委托生命周期。 */
    explicit TaskProgressDelegate(QObject *parent) : QStyledItemDelegate(parent) {}
    /** @brief 当前行绘制百分比和进度。 */
    void paint(QPainter *painter, const QStyleOptionViewItem &option, const QModelIndex &index) const override
    {
        QStyleOptionProgressBar bar;
        bar.rect = option.rect.adjusted(4, 5, -4, -5);
        bar.minimum = 0; bar.maximum = 100; bar.progress = index.data().toInt();
        bar.text = QStringLiteral("%1%").arg(bar.progress); bar.textVisible = true;
        bar.textAlignment = Qt::AlignCenter;
        bar.palette = option.palette; bar.state = option.state;
        option.widget->style()->drawControl(QStyle::CE_ProgressBar, &bar, painter, option.widget);
    }
};
}

BatchProcessingPage::BatchProcessingPage(QWidget *parent) : QWidget(parent)
{
    setObjectName(QStringLiteral("batchProcessingPage"));
    model_ = new BatchTaskModel(this);
    controller_ = new BatchProcessingController(this);
    auto *layout = new QVBoxLayout(this);
    layout->setContentsMargins(18, 14, 18, 14); layout->setSpacing(8);
    auto *title_row = new QHBoxLayout;
    title_row->addWidget(new QLabel(QStringLiteral("方案名称"), this));
    name_ = new QLineEdit(model_->plan().name, this);
    name_->setObjectName(QStringLiteral("batchNameEdit"));
    title_row->addWidget(name_, 1);
    file_label_ = new QLabel(QStringLiteral("未保存"), this);
    file_label_->setTextInteractionFlags(Qt::TextSelectableByMouse);
    title_row->addWidget(file_label_);
    layout->addLayout(title_row);
    auto *file_actions = new QHBoxLayout;
    const auto add_button = [this](QHBoxLayout *row, const QString &text, const QString &object_name) {
        auto *button = new QPushButton(text, this); button->setObjectName(object_name); row->addWidget(button);
            return button;
    };
    auto *new_button = add_button(file_actions, QStringLiteral("新建方案"), "batchNewButton");
    auto *open_button = add_button(file_actions, QStringLiteral("打开方案"), "batchOpenButton");
    auto *save_button = add_button(file_actions, QStringLiteral("保存"), "batchSaveButton");
    auto *save_as = add_button(file_actions, QStringLiteral("另存为"), "batchSaveAsButton");
    file_actions->addStretch(); layout->addLayout(file_actions);
    auto *task_actions = new QHBoxLayout;
    auto *history = add_button(task_actions, QStringLiteral("从最近记录添加"), "batchHistoryButton");
    auto *manual = add_button(task_actions, QStringLiteral("手动添加任务"), "batchAddButton");
    edit_ = add_button(task_actions, QStringLiteral("编辑任务"), "batchEditButton");
    remove_ = add_button(task_actions, QStringLiteral("移除"), "batchRemoveButton");
    up_ = add_button(task_actions, QStringLiteral("上移"), "batchUpButton");
    down_ = add_button(task_actions, QStringLiteral("下移"), "batchDownButton");
    task_actions->addStretch(); layout->addLayout(task_actions);
    mutation_buttons_ = {new_button, open_button, history, manual};
    auto *splitter = new QSplitter(Qt::Vertical, this);
    table_ = new QTableView(splitter);
    table_->setObjectName(QStringLiteral("batchTaskTable"));
    table_->setModel(model_); table_->setSelectionBehavior(QAbstractItemView::SelectRows);
    table_->setSelectionMode(QAbstractItemView::SingleSelection);
    table_->setEditTriggers(QAbstractItemView::NoEditTriggers);
    table_->setAlternatingRowColors(true); table_->setMinimumHeight(120);
    table_->verticalHeader()->hide(); table_->verticalHeader()->setDefaultSectionSize(32);
    table_->horizontalHeader()->setSectionResizeMode(QHeaderView::ResizeToContents);
    table_->horizontalHeader()->setSectionResizeMode(BatchTaskModel::Name, QHeaderView::Stretch);
    table_->horizontalHeader()->setSectionResizeMode(BatchTaskModel::RawFile, QHeaderView::Stretch);
    table_->setItemDelegateForColumn(BatchTaskModel::Progress, new TaskProgressDelegate(table_));
    auto *detail_scroll = new QScrollArea(splitter);
    detail_scroll->setWidgetResizable(true); detail_scroll->setFrameShape(QFrame::NoFrame);
    auto *detail_body = new QWidget(detail_scroll);
    auto *detail_form = new QFormLayout(detail_body);
    raw_ = new QLineEdit(detail_body); raw_->setObjectName(QStringLiteral("batchRawEdit"));
    auto *raw_row = new QWidget(detail_body); auto *raw_layout = new QHBoxLayout(raw_row);
    raw_layout->setContentsMargins(0, 0, 0, 0); raw_layout->addWidget(raw_);
    change_raw_ = new QPushButton(QStringLiteral("更换"), raw_row); raw_layout->addWidget(change_raw_);
    rate_ = new QDoubleSpinBox(detail_body); rate_->setObjectName(QStringLiteral("batchRateSpin"));
    rate_->setDecimals(0); rate_->setRange(1, 1000000000); rate_->setSuffix(QStringLiteral(" Hz"));
    scan_label_ = new QLabel(detail_body); scan_label_->setWordWrap(true);
    scan_label_->setObjectName(QStringLiteral("batchTimeRangeLabel"));
    details_ = new QLabel(detail_body); details_->setWordWrap(true);
    details_->setTextInteractionFlags(Qt::TextSelectableByMouse); details_->setTextFormat(Qt::PlainText);
    detail_form->addRow(QStringLiteral("原始数据"), raw_row);
    detail_form->addRow(QStringLiteral("处理采样率"), rate_);
    detail_form->addRow(QStringLiteral("数据时间范围"), scan_label_);
    detail_form->addRow(QStringLiteral("选中任务详情"), details_);
    detail_scroll->setWidget(detail_body);
    splitter->addWidget(table_); splitter->addWidget(detail_scroll);
    splitter->setStretchFactor(0, 3); splitter->setStretchFactor(1, 2);
    splitter->setSizes({220, 180}); layout->addWidget(splitter, 1);
    auto *output_row = new QHBoxLayout;
    output_row->addWidget(new QLabel(QStringLiteral("输出根目录"), this));
    output_root_ = new QLineEdit(this); output_root_->setObjectName(QStringLiteral("batchOutputRootEdit"));
    output_row->addWidget(output_root_, 1);
    auto *browse_output = new QPushButton(QStringLiteral("浏览"), this); output_row->addWidget(browse_output);
    mutation_buttons_.append(browse_output); layout->addLayout(output_row);
    summary_ = new QLabel(this); layout->addWidget(summary_);
    auto *run_row = new QHBoxLayout;
    progress_ = new QProgressBar(this); progress_->setObjectName(QStringLiteral("batchProgressBar"));
    progress_->setRange(0, 100); progress_->setValue(0); run_row->addWidget(progress_, 1);
    start_ = add_button(run_row, QStringLiteral("开始处理"), "batchStartButton");
    stop_ = add_button(run_row, QStringLiteral("中断处理"), "batchStopButton");
    results_ = add_button(run_row, QStringLiteral("打开结果目录"), "batchResultsButton");
    layout->addLayout(run_row);
    state_ = new QLabel(QStringLiteral("就绪"), this); state_->setObjectName(QStringLiteral("batchStateLabel"));
    state_->setWordWrap(true); layout->addWidget(state_);
    log_ = new QPlainTextEdit(this); log_->setObjectName(QStringLiteral("batchLog"));
    log_->setReadOnly(true); log_->setMaximumBlockCount(1500); log_->setMaximumHeight(100);
    layout->addWidget(log_);
    scan_watcher_ = new QFutureWatcher<RawTimeRangeResult>(this);
    connect(scan_watcher_, &QFutureWatcher<RawTimeRangeResult>::finished, this, [this]() {
        scan_running_ = false;
        const auto result = scan_watcher_->result();
        if (active_scan_version_ != scan_version_) { startDetailScan(); return; }
        scan_label_->setText(result.success
            ? QStringLiteral("%1—%2").arg(RawTimeRangeScanner::displayTime(result.start_seconds),
                                            RawTimeRangeScanner::displayTime(result.end_seconds))
            : QStringLiteral("扫描未成功：%1").arg(result.error));
    });
    connect(new_button, &QPushButton::clicked, this, &BatchProcessingPage::newPlan);
    connect(open_button, &QPushButton::clicked, this, &BatchProcessingPage::openPlan);
    connect(save_button, &QPushButton::clicked, this, [this]() { save(); });
    connect(save_as, &QPushButton::clicked, this, [this]() { save(true); });
    connect(history, &QPushButton::clicked, this, &BatchProcessingPage::addHistory);
    connect(manual, &QPushButton::clicked, this, [this]() { editTask(true); });
    connect(edit_, &QPushButton::clicked, this, [this]() { editTask(false); });
    connect(table_, &QTableView::doubleClicked, this, [this]() { editTask(false); });
    connect(remove_, &QPushButton::clicked, this, [this]() { model_->removeTask(selectedRow()); validateRows();
        updateDetails(); });
    connect(up_, &QPushButton::clicked, this, [this]() {
        const int row = selectedRow(); model_->moveTask(row, row - 1); if (row > 0) { table_->selectRow(row - 1); }
    });
    connect(down_, &QPushButton::clicked, this, [this]() {
        const int row = selectedRow(); model_->moveTask(row, row + 1);
        if (row >= 0 && row + 1 < model_->rowCount()) { table_->selectRow(row + 1); }
    });
    connect(name_, &QLineEdit::textEdited, this, [this]() { model_->setMetadata(name_->text(),
        output_root_->text()); });
    connect(output_root_, &QLineEdit::textChanged, this, [this]() { model_->setMetadata(name_->text(),
        output_root_->text()); });
    connect(browse_output, &QPushButton::clicked, this, [this]() {
        const auto path = QFileDialog::getExistingDirectory(this, QStringLiteral("选择输出根目录"), output_root_->text());
        if (!path.isEmpty()) { output_root_->setText(path); }
    });
    connect(raw_, &QLineEdit::editingFinished, this, [this]() {
        const int row = selectedRow(); if (row < 0 || model_->isLocked()) { return; }
        auto item = model_->task(row);
        const auto path = ProcessingOptionsSupport::absolutePath(raw_->text());
        if (path == item.options.task_definition.raw_file_path) { return; }
        item.options.task_definition.raw_file_path = path; model_->replaceTask(row, item);
        validateRows(); updateDetails();
    });
    connect(change_raw_, &QPushButton::clicked, this, [this]() {
        const auto path = QFileDialog::getOpenFileName(this, QStringLiteral("更换原始数据"), raw_->text());
        if (!path.isEmpty()) { raw_->setText(path); emit raw_->editingFinished(); }
    });
    connect(rate_, &QDoubleSpinBox::editingFinished, this, [this]() {
        const int row = selectedRow(); if (row < 0 || model_->isLocked()) { return; }
        auto item = model_->task(row);
        if (item.options.task_definition.user_sample_rate_hz == rate_->value()) { return; }
        item.options.task_definition.user_sample_rate_hz = rate_->value(); model_->replaceTask(row, item);
        validateRows(); updateDetails();
    });
    connect(table_->selectionModel(), &QItemSelectionModel::currentRowChanged, this, [this]() { updateDetails();
        updateActions(); });
    connect(model_, &BatchTaskModel::documentChanged, this, &BatchProcessingPage::updateActions);
    connect(start_, &QPushButton::clicked, this, &BatchProcessingPage::startProcessing);
    connect(stop_, &QPushButton::clicked, controller_, &BatchProcessingController::requestStop);
    connect(results_, &QPushButton::clicked, this, [this]() {
        if (!QDesktopServices::openUrl(QUrl::fromLocalFile(last_results_directory_))) {
            appendLog(QStringLiteral("无法打开结果目录：%1").arg(last_results_directory_));
        }
    });
    connect(controller_, &BatchProcessingController::runningChanged, this, [this](bool running) {
        model_->setLocked(running); updateActions(); emit runningChanged(running);
    });
    connect(controller_, &BatchProcessingController::taskChanged, this, [this](const QString &id, const
        BatchTaskRuntime &state) {
        model_->setRuntime(id, state); updateActions();
        if (selectedRow() >= 0 && model_->task(selectedRow()).id == id) { updateDetails(); }
        if (state.state == BatchTaskState::Running || state.state == BatchTaskState::Stopping) {
            state_->setText(state.detail);
        }
    });
    connect(controller_, &BatchProcessingController::progressChanged, this,
            [this](int percent, int succeeded, int failed, int total) {
        progress_->setValue(percent);
        summary_->setText(QStringLiteral("串行处理 · 成功 %1 / 失败 %2 / 总计 %3").arg(succeeded).arg(failed).arg(total));
    });
    connect(controller_, &BatchProcessingController::message, this, &BatchProcessingPage::appendLog);
    connect(controller_, &BatchProcessingController::finished, this,
            [this](int succeeded, int failed, bool interrupted, const QString &directory) {
        last_results_directory_ = directory;
        const auto result = interrupted ? QStringLiteral("已中断")
            : (failed ? QStringLiteral("处理结束，部分失败") : QStringLiteral("全部处理完成"));
        state_->setText(QStringLiteral("%1：成功 %2，失败 %3。结果：%4").arg(result).arg(succeeded).arg(failed).arg(directory));
        appendLog(state_->text()); updateActions();
    });
    updateActions(); updateDetails();
}

BatchProcessingPage::~BatchProcessingPage()
{
    if (cancellation_) { cancellation_->storeRelease(1); }
}
BatchTaskModel *BatchProcessingPage::model() const { return model_; }
BatchProcessingController *BatchProcessingPage::controller() const { return controller_; }
int BatchProcessingPage::selectedRow() const { return table_->currentIndex().isValid() ?
    table_->currentIndex().row() : -1; }
void BatchProcessingPage::setExternalBusy(bool busy) { external_busy_ = busy; updateActions(); }

void BatchProcessingPage::appendLog(const QString &text)
{
    log_->appendPlainText(QStringLiteral("[%1] %2").arg(QDateTime::currentDateTime().toString("HH:mm:ss"), text));
}

void BatchProcessingPage::updateActions()
{
    const bool running = controller_->isRunning();
    const int row = selectedRow(); const bool selected = row >= 0 && row < model_->rowCount();
    for (auto *button : mutation_buttons_) { button->setEnabled(!running); }
    name_->setEnabled(!running); output_root_->setEnabled(!running);
    edit_->setEnabled(!running && selected); remove_->setEnabled(!running && selected);
    up_->setEnabled(!running && row > 0); down_->setEnabled(!running && selected && row + 1 < model_->rowCount());
    raw_->setEnabled(!running && selected); rate_->setEnabled(!running && selected);
    change_raw_->setEnabled(!running && selected);
    start_->setEnabled(!running && !external_busy_ && model_->rowCount() > 0);
    start_->setToolTip(external_busy_ ? QStringLiteral("单任务仍在执行或清理，请等待结束") : QString());
    stop_->setEnabled(running); results_->setEnabled(!last_results_directory_.isEmpty());
    file_label_->setText(QStringLiteral("%1%2").arg(file_path_.isEmpty() ? QStringLiteral("未保存") :
        QFileInfo(file_path_).fileName(),
        model_->isModified() ? QStringLiteral(" · 有修改") : QString()));
    file_label_->setToolTip(file_path_);
    if (!running) {
        int ready = 0; int needs_attention = 0;
        for (const auto &item : model_->plan().tasks) {
            if (model_->runtime(item.id).state == BatchTaskState::NeedsAttention) { ++needs_attention; }
            if (model_->runtime(item.id).state == BatchTaskState::Ready) { ++ready; }
        }
        summary_->setText(QStringLiteral("共 %1 个任务 · %2 个就绪 · %3 个待修正 · 按列表顺序处理")
                             .arg(model_->rowCount()).arg(ready).arg(needs_attention));
    }
}

void BatchProcessingPage::updateDetails()
{
    const QSignalBlocker raw_block(raw_); const QSignalBlocker rate_block(rate_);
    if (selectedRow() < 0 || selectedRow() >= model_->rowCount()) {
        raw_->clear(); details_->setText(QStringLiteral("选择任务查看完整设置及结果")); requestDetailScan(); return;
    }
    const auto item = model_->task(selectedRow());
    raw_->setText(item.options.task_definition.raw_file_path);
    rate_->setValue(item.options.task_definition.user_sample_rate_hz);
    const auto state = model_->runtime(item.id);
    details_->setText(QStringLiteral("%1 · %2\n配置：%3\n参数组：%4（含 TIME 共 %5 个参数）\n时间：%6\n"
                                    "输出原始码值：%7\n%8%9")
        .arg(item.name, item.source_description, item.options.task_definition.config_file_path,
             item.options.parameter_group_file_path.isEmpty() ? QStringLiteral("界面选择参数") :
                 item.options.parameter_group_file_path)
        .arg(item.options.parameter_names.size()).arg(item.timeDescription(),
             item.options.include_raw_values ? QStringLiteral("是") : QStringLiteral("否"), state.detail,
             state.output_paths.isEmpty() ? QString() : QStringLiteral("\n结果：\n") +
                 state.output_paths.join(QLatin1Char('\n'))));
    requestDetailScan();
}

void BatchProcessingPage::validateRows()
{
    if (controller_->isRunning()) { return; }
    for (const auto &item : model_->plan().tasks) {
        const auto previous = model_->runtime(item.id).state;
        if (previous != BatchTaskState::Ready && previous != BatchTaskState::NeedsAttention) { continue; }
        BatchTaskRuntime state;
        const auto error = controller_->validateTask(item);
        state.state = error.isEmpty() ? BatchTaskState::Ready : BatchTaskState::NeedsAttention;
        state.detail = error;
        if (error.isEmpty() && !item.config_fingerprint.isEmpty() &&
            ProcessingOptionsSupport::fingerprint(item.options.task_definition.config_file_path) !=
                item.config_fingerprint) {
            state.detail = QStringLiteral("配置文件内容已变化，当前参数已通过重新校验。");
        }
        model_->setRuntime(item.id, state);
    }
    updateActions();
}

bool BatchProcessingPage::savePlan(const QString &path, QString *error)
{
    if (!BatchPlanFile::save(path, model_->plan(), error)) { return false; }
    file_path_ = ProcessingOptionsSupport::absolutePath(path);
    model_->markSaved(); appendLog(QStringLiteral("已保存方案：%1").arg(file_path_)); return true;
}

bool BatchProcessingPage::loadPlan(const QString &path, QString *error)
{
    if (controller_->isRunning()) { if (error) { *error = QStringLiteral("运行期间不能打开方案。"); } return false; }
    BatchPlan loaded;
    if (!BatchPlanFile::load(path, &loaded, error)) { return false; }
    model_->setPlan(loaded);
    const QSignalBlocker name_block(name_); const QSignalBlocker root_block(output_root_);
    name_->setText(loaded.name); output_root_->setText(loaded.output_root);
    file_path_ = ProcessingOptionsSupport::absolutePath(path); last_results_directory_.clear();
    progress_->setValue(0); state_->setText(QStringLiteral("已打开方案，等待开始处理"));
    validateRows(); if (model_->rowCount()) { table_->selectRow(0); }
    updateDetails(); updateActions(); return true;
}

bool BatchProcessingPage::save(bool save_as)
{
    QString path = file_path_;
    if (save_as || path.isEmpty()) {
        path = QFileDialog::getSaveFileName(this, QStringLiteral("保存多任务方案"),
            path.isEmpty() ? QStringLiteral("多任务方案.txt") : path, QStringLiteral("多任务方案 (*.txt)"));
        if (path.isEmpty()) { return false; }
        if (!path.endsWith(QStringLiteral(".txt"), Qt::CaseInsensitive)) { path += QStringLiteral(".txt"); }
    }
    QString error;
    if (!savePlan(path, &error)) { QMessageBox::warning(this, QStringLiteral("保存失败"), error); return false; }
    return true;
}

bool BatchProcessingPage::confirmSaveChanges()
{
    if (!model_->isModified()) { return true; }
    QMessageBox box(QMessageBox::Question, QStringLiteral("方案尚未保存"),
                    QStringLiteral("是否保存当前多任务方案的修改？"), QMessageBox::NoButton, this);
    auto *save_button = box.addButton(QStringLiteral("保存"), QMessageBox::AcceptRole);
    auto *discard = box.addButton(QStringLiteral("放弃"), QMessageBox::DestructiveRole);
    box.addButton(QStringLiteral("取消"), QMessageBox::RejectRole);
    box.exec();
    return box.clickedButton() == discard || (box.clickedButton() == save_button && save());
}

void BatchProcessingPage::newPlan()
{
    if (controller_->isRunning() || !confirmSaveChanges()) { return; }
    model_->setPlan(BatchPlan());
    const QSignalBlocker name_block(name_); const QSignalBlocker root_block(output_root_);
    name_->setText(model_->plan().name); output_root_->clear();
    file_path_.clear(); last_results_directory_.clear(); progress_->setValue(0); state_->setText(QStringLiteral("就绪"));
    updateActions(); updateDetails();
}

void BatchProcessingPage::openPlan()
{
    if (controller_->isRunning() || !confirmSaveChanges()) { return; }
    const auto path = QFileDialog::getOpenFileName(this, QStringLiteral("打开多任务方案"), file_path_,
                                                 QStringLiteral("多任务方案 (*.txt)"));
    if (path.isEmpty()) { return; }
    QString error;
    if (!loadPlan(path, &error)) { QMessageBox::warning(this, QStringLiteral("打开失败"), error); }
}

void BatchProcessingPage::editTask(bool adding)
{
    if (controller_->isRunning() || (!adding && selectedRow() < 0)) { return; }
    const int row = selectedRow();
    BatchTaskItem item = adding ? BatchTaskItem() : model_->task(row);
    BatchTaskEditor editor(item, adding, this);
    if (editor.exec() != QDialog::Accepted) { return; }
    if (adding) { model_->appendTask(editor.task()); table_->selectRow(model_->rowCount() - 1); }
    else { model_->replaceTask(row, editor.task()); }
    validateRows(); updateDetails();
}

void BatchProcessingPage::addHistory()
{
    if (controller_->isRunning()) { return; }
    const auto entries = ProcessingHistoryStore::load();
    QDialog dialog(this); dialog.setWindowTitle(QStringLiteral("从最近单次处理记录添加"));
    dialog.setObjectName(QStringLiteral("batchHistoryDialog")); dialog.resize(850, 450);
    auto *layout = new QVBoxLayout(&dialog);
    auto *search = new QLineEdit(&dialog); search->setPlaceholderText(QStringLiteral("查找原始文件或配置名称"));
    layout->addWidget(search);
    auto *table = new QTableWidget(entries.size(), 5, &dialog);
    table->setObjectName(QStringLiteral("batchHistoryTable"));
    table->setHorizontalHeaderLabels({QStringLiteral("选择"), QStringLiteral("最近处理时间"), QStringLiteral("原始数据"),
                                     QStringLiteral("配置文件"), QStringLiteral("上次状态")});
    table->setEditTriggers(QAbstractItemView::NoEditTriggers);
    table->setSelectionBehavior(QAbstractItemView::SelectRows);
        table->setSelectionMode(QAbstractItemView::SingleSelection);
    table->horizontalHeader()->setSectionResizeMode(QHeaderView::ResizeToContents);
    table->horizontalHeader()->setSectionResizeMode(2, QHeaderView::Stretch);
    for (int row = 0; row < entries.size(); ++row) {
        const auto &entry = entries.at(row);
        auto *check = new QTableWidgetItem; check->setCheckState(Qt::Unchecked); table->setItem(row, 0, check);
        table->setItem(row, 1, new QTableWidgetItem(entry.started_at.toString("yyyy-MM-dd HH:mm")));
        table->setItem(row, 2, new QTableWidgetItem(QFileInfo(entry.raw_file_path).fileName()));
        table->item(row, 2)->setToolTip(entry.raw_file_path);
        table->setItem(row, 3, new QTableWidgetItem(QFileInfo(entry.config_path).fileName()));
        table->item(row, 3)->setToolTip(entry.config_path);
        table->setItem(row, 4, new QTableWidgetItem(entry.status));
    }
    layout->addWidget(table);
    auto *detail = new QLabel(entries.isEmpty() ? QStringLiteral("暂无单次处理记录，可返回手动添加任务。")
                                               : QStringLiteral("选择记录查看详情；勾选后批量添加。"), &dialog);
    detail->setWordWrap(true); detail->setTextFormat(Qt::PlainText); layout->addWidget(detail);
    auto *count = new QLabel(QStringLiteral("已选择 0 条"), &dialog); layout->addWidget(count);
    auto *buttons = new QDialogButtonBox(QDialogButtonBox::Ok | QDialogButtonBox::Cancel, &dialog);
    auto *ok = buttons->button(QDialogButtonBox::Ok);
    ok->setText(QStringLiteral("添加到任务列表")); ok->setEnabled(false);
        ok->setObjectName(QStringLiteral("batchHistoryAccept"));
    buttons->button(QDialogButtonBox::Cancel)->setText(QStringLiteral("取消")); layout->addWidget(buttons);
    connect(buttons, &QDialogButtonBox::accepted, &dialog, &QDialog::accept);
    connect(buttons, &QDialogButtonBox::rejected, &dialog, &QDialog::reject);
    connect(search, &QLineEdit::textChanged, &dialog, [table, entries](const QString &text) {
        for (int row = 0; row < entries.size(); ++row) {
            table->setRowHidden(row, !entries.at(row).raw_file_path.contains(text, Qt::CaseInsensitive) &&
                                    !entries.at(row).config_path.contains(text, Qt::CaseInsensitive));
        }
    });
    connect(table, &QTableWidget::itemChanged, &dialog, [table, ok, count]() {
        int checked = 0;
        for (int row = 0; row < table->rowCount(); ++row) { checked += table->item(row, 0)->checkState() ==
            Qt::Checked; }
        count->setText(QStringLiteral("已选择 %1 条").arg(checked)); ok->setEnabled(checked > 0);
    });
    connect(table, &QTableWidget::currentCellChanged, &dialog, [entries, detail](int row, int, int, int) {
        if (row < 0 || row >= entries.size()) { return; }
        const auto &entry = entries.at(row);
        detail->setText(QStringLiteral("采样率：%1 Hz · 参数：%2 个\n原始数据：%3\n配置文件：%4")
            .arg(entry.sample_rate_hz).arg(entry.parameter_names.size())
            .arg(QFileInfo(entry.raw_file_path).isReadable() ? entry.raw_file_path : QStringLiteral("文件不可用，添加后可更换"),
                 QFileInfo(entry.config_path).isReadable() ? entry.config_path : QStringLiteral("文件不可用，添加后可修正")));
    });
    if (dialog.exec() != QDialog::Accepted) { return; }
    const int first_new = model_->rowCount();
    for (int row = 0; row < entries.size(); ++row) {
        if (table->item(row, 0)->checkState() == Qt::Checked) { model_->appendTask(entries.at(row).toBatchTask()); }
    }
    validateRows(); if (model_->rowCount() > first_new) { table_->selectRow(first_new); } updateDetails();
}

void BatchProcessingPage::startProcessing()
{
    if (external_busy_ || controller_->isRunning()) { return; }
    // 直接调用接口时也提交尚未离焦的编辑字段。
    emit raw_->editingFinished(); emit rate_->editingFinished();
    model_->setMetadata(name_->text(), output_root_->text());
    QString error;
    if (!controller_->start(model_->plan(), &error)) {
        state_->setText(QStringLiteral("启动前检查失败")); appendLog(error);
        QMessageBox::warning(this, QStringLiteral("启动前检查失败"), error); return;
    }
    progress_->setValue(0); last_results_directory_ = controller_->outputDirectory(); updateActions();
}

void BatchProcessingPage::requestDetailScan()
{
    const auto item = selectedRow() >= 0 ? model_->task(selectedRow()) : BatchTaskItem();
    const QString key = selectedRow() < 0 ? QString() : item.id + QLatin1Char('\n') +
        item.options.task_definition.config_file_path + QLatin1Char('\n') + item.options.task_definition.raw_file_path;
    if (key == scan_key_) { return; }
    scan_key_ = key; ++scan_version_;
    if (cancellation_) { cancellation_->storeRelease(1); }
    if (key.isEmpty()) { scan_label_->clear(); return; }
    scan_label_->setText(QStringLiteral("正在扫描数据时间范围…"));
    if (!scan_running_) { startDetailScan(); }
}

void BatchProcessingPage::startDetailScan()
{
    if (selectedRow() < 0 || scan_key_.isEmpty() || scan_running_) { return; }
    const auto item = model_->task(selectedRow());
    const auto config = item.options.task_definition.config_file_path;
    const auto raw = item.options.task_definition.raw_file_path;
    if (config.isEmpty() || raw.isEmpty()) { scan_label_->setText(QStringLiteral("请选择配置和原始数据")); return; }
    cancellation_.reset(new QAtomicInteger<int>(0));
    const auto cancellation = cancellation_; active_scan_version_ = scan_version_;
    scan_running_ = true;
    scan_watcher_->setFuture(QtConcurrent::run([config, raw, cancellation]() {
        try { return RawTimeRangeScanner::scan(config, raw, cancellation); }
        catch (const std::exception &error) { RawTimeRangeResult result; result.error =
            QString::fromUtf8(error.what()); return result; }
        catch (...) { RawTimeRangeResult result; result.error = QStringLiteral("扫描发生异常"); return result; }
    }));
}
