#include "batch_task_editor.h"
#include "config_file_document.h"
#include "processing_options_support.h"
#include "time_range_parser.h"
#include <QCheckBox>
#include <QComboBox>
#include <QDialogButtonBox>
#include <QDoubleSpinBox>
#include <QFileDialog>
#include <QFileInfo>
#include <QFormLayout>
#include <QGridLayout>
#include <QLabel>
#include <QLineEdit>
#include <QListWidget>
#include <QPlainTextEdit>
#include <QPushButton>
#include <QScrollArea>
#include <QTimer>
#include <QVBoxLayout>
#include <QtConcurrent>
#include <exception>

namespace {
/** @brief 创建文件选择行，路径也允许直接粘贴。 */
QWidget *pathRow(QLineEdit *edit, const QString &title, QWidget *parent)
{
    auto *row = new QWidget(parent);
    auto *layout = new QHBoxLayout(row);
    layout->setContentsMargins(0, 0, 0, 0);
    layout->addWidget(edit);
    auto *button = new QPushButton(QStringLiteral("浏览"), row);
    layout->addWidget(button);
    QObject::connect(button, &QPushButton::clicked, row, [edit, title, row]() {
        const auto path = QFileDialog::getOpenFileName(row, title, edit->text());
        if (!path.isEmpty()) { edit->setText(path); emit edit->editingFinished(); }
    });
    return row;
}
}

BatchTaskEditor::BatchTaskEditor(const BatchTaskItem &item, bool adding, QWidget *parent)
    : QDialog(parent), item_(item), selected_names_(item.options.parameter_names)
{
    setObjectName(QStringLiteral("batchTaskEditor"));
    setWindowTitle(adding ? QStringLiteral("添加任务") : QStringLiteral("编辑任务"));
    resize(780, 690);
    auto *outer = new QVBoxLayout(this);
    auto *scroll = new QScrollArea(this);
    scroll->setWidgetResizable(true);
    auto *body = new QWidget(scroll);
    auto *layout = new QVBoxLayout(body);
    auto *form = new QFormLayout;
    name_ = new QLineEdit(item.name, body);
    name_->setObjectName(QStringLiteral("taskNameEdit"));
    config_ = new QLineEdit(item.options.task_definition.config_file_path, body);
    config_->setObjectName(QStringLiteral("taskConfigEdit"));
    raw_ = new QLineEdit(item.options.task_definition.raw_file_path, body);
    raw_->setObjectName(QStringLiteral("taskRawEdit"));
    scan_label_ = new QLabel(QStringLiteral("选择配置和原始数据后自动扫描"), body);
    scan_label_->setWordWrap(true);
    group_ = new QLineEdit(item.options.parameter_group_file_path, body);
    group_->setObjectName(QStringLiteral("taskGroupEdit"));
    form->addRow(QStringLiteral("任务名称"), name_);
    form->addRow(QStringLiteral("配置文件"), pathRow(config_, QStringLiteral("选择配置文件"), body));
    form->addRow(QStringLiteral("原始数据"), pathRow(raw_, QStringLiteral("选择原始数据"), body));
    form->addRow(QStringLiteral("数据时间范围"), scan_label_);
    form->addRow(QStringLiteral("参数组文件"), pathRow(group_, QStringLiteral("选择参数组文件"), body));
    layout->addLayout(form);
    auto *grid = new QGridLayout;
    config_search_ = new QLineEdit(body);
    config_search_->setPlaceholderText(QStringLiteral("查找配置参数"));
    selected_search_ = new QLineEdit(body);
    selected_search_->setPlaceholderText(QStringLiteral("查找已选参数"));
    available_ = new QListWidget(body);
    selected_ = new QListWidget(body);
    available_->setObjectName(QStringLiteral("taskAvailableParameters"));
    selected_->setObjectName(QStringLiteral("taskSelectedParameters"));
    available_->setSelectionMode(QAbstractItemView::ExtendedSelection);
    selected_->setSelectionMode(QAbstractItemView::ExtendedSelection);
    available_->setMinimumHeight(140);
    selected_->setMinimumHeight(140);
    grid->addWidget(config_search_, 0, 0);
    grid->addWidget(selected_search_, 0, 2);
    grid->addWidget(available_, 1, 0);
    grid->addWidget(selected_, 1, 2);
    auto *buttons = new QVBoxLayout;
    auto *add = new QPushButton(QStringLiteral("添加选中 →"), body);
    auto *all = new QPushButton(QStringLiteral("添加全部 →"), body);
    auto *remove = new QPushButton(QStringLiteral("← 移除"), body);
    buttons->addWidget(add); buttons->addWidget(all); buttons->addWidget(remove); buttons->addStretch();
    grid->addLayout(buttons, 1, 1);
    grid->setColumnStretch(0, 1); grid->setColumnStretch(2, 1);
    layout->addLayout(grid);
    for (int index = selected_names_.size() - 1; index >= 0; --index) {
        if (selected_names_.at(index).compare(QStringLiteral("TIME"), Qt::CaseInsensitive) == 0) {
            selected_names_.removeAt(index);
        }
    }
    selected_names_.prepend(QStringLiteral("TIME"));
    const auto append_names = [this](bool all_visible) {
        for (int index = 0; index < available_->count(); ++index) {
            const auto *entry = available_->item(index);
            if (!all_visible && !entry->isSelected()) { continue; }
            const auto name = entry->text();
            if (!selected_names_.contains(name, Qt::CaseInsensitive)) { selected_names_.append(name); }
        }
        refreshParameters();
    };
    connect(add, &QPushButton::clicked, this, [append_names]() { append_names(false); });
    connect(all, &QPushButton::clicked, this, [append_names]() { append_names(true); });
    connect(remove, &QPushButton::clicked, this, [this]() {
        for (const auto *entry : selected_->selectedItems()) {
            if (entry->text().compare(QStringLiteral("TIME"), Qt::CaseInsensitive) != 0) {
                selected_names_.removeAll(entry->text());
            }
        }
        refreshParameters();
    });
    connect(config_search_, &QLineEdit::textChanged, this, &BatchTaskEditor::refreshParameters);
    connect(selected_search_, &QLineEdit::textChanged, this, &BatchTaskEditor::refreshParameters);
    auto *settings = new QFormLayout;
    rate_ = new QDoubleSpinBox(body);
    rate_->setObjectName(QStringLiteral("taskRateSpin"));
    rate_->setDecimals(0); rate_->setRange(1, 1000000000); rate_->setSuffix(QStringLiteral(" Hz"));
    rate_->setValue(item.options.task_definition.user_sample_rate_hz > 0 ?
        item.options.task_definition.user_sample_rate_hz : 1);
    time_mode_ = new QComboBox(body);
    time_mode_->setObjectName(QStringLiteral("taskTimeMode"));
    time_mode_->addItems({QStringLiteral("全程处理"), QStringLiteral("单时间段"), QStringLiteral("多时间段")});
    time_mode_->setCurrentIndex(item.time_mode);
    start_ = new QLineEdit(QStringLiteral("00:00:00.000"), body);
    end_ = new QLineEdit(QStringLiteral("23:59:59.000"), body);
    start_->setObjectName(QStringLiteral("taskStartEdit")); end_->setObjectName(QStringLiteral("taskEndEdit"));
    if (item.time_mode == 1 && !item.options.time_ranges.isEmpty()) {
        start_->setText(QString::number(item.options.time_ranges.first().start_seconds, 'f', 3));
        end_->setText(QString::number(item.options.time_ranges.first().end_seconds, 'f', 3));
    }
    ranges_path_ = new QLineEdit(item.time_range_file_path, body);
    ranges_path_->setObjectName(QStringLiteral("taskRangesPathEdit"));
    ranges_ = new QPlainTextEdit(body);
    ranges_->setObjectName(QStringLiteral("taskRangesEdit"));
    ranges_->setPlaceholderText(QStringLiteral("每行一个时间段：10:00:00.000 10:10:00.000"));
    ranges_->setMaximumHeight(85);
    QStringList range_lines;
    if (item.time_mode == 2) {
        for (const auto &range : item.options.time_ranges) {
            range_lines << QStringLiteral("%1 %2").arg(range.start_seconds, 0, 'f', 3).arg(range.end_seconds, 0,
                'f', 3);
        }
    }
    ranges_->setPlainText(range_lines.join(QLatin1Char('\n')));
    raw_values_ = new QCheckBox(QStringLiteral("输出原始码值"), body);
    raw_values_->setChecked(item.options.include_raw_values);
    settings->addRow(QStringLiteral("处理采样率"), rate_);
    settings->addRow(QStringLiteral("时间设置"), time_mode_);
    settings->addRow(QStringLiteral("起始时间 / 秒数"), start_);
    settings->addRow(QStringLiteral("结束时间 / 秒数"), end_);
    auto *ranges_row = pathRow(ranges_path_, QStringLiteral("导入时间段文件"), body);
    settings->addRow(QStringLiteral("时间段来源"), ranges_row);
    settings->addRow(QStringLiteral("时间段快照"), ranges_);
    settings->addRow(raw_values_);
    settings->addRow(QStringLiteral("结果位置"), new QLabel(QStringLiteral("由多任务方案统一生成"), body));
    layout->addLayout(settings);
    const auto time_changed = [this, ranges_row]() {
        start_->setEnabled(time_mode_->currentIndex() == 1);
        end_->setEnabled(time_mode_->currentIndex() == 1);
        ranges_row->setEnabled(time_mode_->currentIndex() == 2);
        ranges_->setEnabled(time_mode_->currentIndex() == 2);
    };
    connect(time_mode_, QOverload<int>::of(&QComboBox::currentIndexChanged), this, time_changed);
    time_changed();
    error_ = new QLabel(body); error_->setWordWrap(true);
    error_->setObjectName(QStringLiteral("taskEditorError"));
    layout->addWidget(error_);
    scroll->setWidget(body); outer->addWidget(scroll);
    auto *dialog_buttons = new QDialogButtonBox(QDialogButtonBox::Ok | QDialogButtonBox::Cancel, this);
    dialog_buttons->button(QDialogButtonBox::Ok)->setText(adding ? QStringLiteral("添加到任务列表") : QStringLiteral("保存修改"));
    dialog_buttons->button(QDialogButtonBox::Cancel)->setText(QStringLiteral("取消"));
    connect(dialog_buttons, &QDialogButtonBox::accepted, this, &BatchTaskEditor::accept);
    connect(dialog_buttons, &QDialogButtonBox::rejected, this, &QDialog::reject);
    outer->addWidget(dialog_buttons);
    scan_timer_ = new QTimer(this); scan_timer_->setSingleShot(true);
    scan_watcher_ = new QFutureWatcher<RawTimeRangeResult>(this);
    connect(scan_timer_, &QTimer::timeout, this, &BatchTaskEditor::startScan);
    connect(scan_watcher_, &QFutureWatcher<RawTimeRangeResult>::finished, this, [this]() {
        scan_running_ = false;
        const auto result = scan_watcher_->result();
        if (active_scan_version_ != scan_version_) { scan_timer_->start(0); return; }
        scan_label_->setText(result.success
            ? QStringLiteral("%1—%2").arg(RawTimeRangeScanner::displayTime(result.start_seconds),
                                            RawTimeRangeScanner::displayTime(result.end_seconds))
            : QStringLiteral("时间范围扫描失败：%1").arg(result.error));
    });
    connect(config_, &QLineEdit::editingFinished, this, &BatchTaskEditor::loadConfig);
    connect(config_, &QLineEdit::textChanged, this, &BatchTaskEditor::scheduleScan);
    connect(raw_, &QLineEdit::textChanged, this, &BatchTaskEditor::scheduleScan);
    connect(group_, &QLineEdit::editingFinished, this, &BatchTaskEditor::importGroup);
    connect(ranges_path_, &QLineEdit::editingFinished, this, &BatchTaskEditor::importRanges);
    loadConfig(); scheduleScan();
}

BatchTaskEditor::~BatchTaskEditor()
{
    if (cancellation_) { cancellation_->storeRelease(1); }
}
BatchTaskItem BatchTaskEditor::task() const { return item_; }

void BatchTaskEditor::loadConfig()
{
    ConfigFileDocument document;
    QString error;
    config_names_.clear();
    if (!config_->text().trimmed().isEmpty() && !document.loadFromFile(config_->text().trimmed(), &error)) {
        error_->setText(error);
    } else {
        config_names_ = document.parameterNames();
        error_->clear();
    }
    refreshParameters();
}

void BatchTaskEditor::refreshParameters()
{
    available_->clear(); selected_->clear();
    for (const auto &name : config_names_) {
        if (name.contains(config_search_->text(), Qt::CaseInsensitive)) { available_->addItem(name); }
    }
    for (const auto &name : selected_names_) {
        if (name.contains(selected_search_->text(), Qt::CaseInsensitive)) { selected_->addItem(name); }
    }
}

void BatchTaskEditor::importGroup()
{
    if (group_->text().trimmed().isEmpty()) { return; }
    QString error;
    QStringList names;
    if (!ProcessingOptionsSupport::readParameterGroup(group_->text().trimmed(), &names, &error)) {
        error_->setText(error); return;
    }
    selected_names_ = names;
    error_->clear(); refreshParameters();
}

void BatchTaskEditor::importRanges()
{
    if (ranges_path_->text().trimmed().isEmpty()) { return; }
    QList<TaskTimeRange> ranges;
    QString error;
    if (!TimeRangeParser::parseFile(ranges_path_->text().trimmed(), &ranges, nullptr, &error)) {
        error_->setText(error); return;
    }
    QStringList lines;
    for (const auto &range : ranges) {
        lines << QStringLiteral("%1 %2").arg(range.start_seconds, 0, 'f', 3).arg(range.end_seconds, 0, 'f', 3);
    }
    ranges_->setPlainText(lines.join(QLatin1Char('\n'))); error_->clear();
}

void BatchTaskEditor::scheduleScan()
{
    ++scan_version_;
    if (cancellation_) { cancellation_->storeRelease(1); }
    scan_label_->setText(QStringLiteral("等待扫描数据时间范围…"));
    scan_timer_->start(250);
}

void BatchTaskEditor::startScan()
{
    if (scan_running_) { return; }
    const QString config = config_->text().trimmed();
    const QString raw = raw_->text().trimmed();
    if (config.isEmpty() || raw.isEmpty()) {
        scan_label_->setText(QStringLiteral("请选择配置和原始数据")); return;
    }
    cancellation_.reset(new QAtomicInteger<int>(0));
    const auto cancellation = cancellation_;
    active_scan_version_ = scan_version_;
    scan_label_->setText(QStringLiteral("正在扫描数据时间范围…"));
    scan_running_ = true;
    scan_watcher_->setFuture(QtConcurrent::run([config, raw, cancellation]() {
        try { return RawTimeRangeScanner::scan(config, raw, cancellation); }
        catch (const std::exception &error) { RawTimeRangeResult result; result.error =
            QString::fromUtf8(error.what()); return result; }
        catch (...) { RawTimeRangeResult result; result.error = QStringLiteral("扫描发生异常"); return result; }
    }));
}

void BatchTaskEditor::accept()
{
    BatchTaskItem edited = item_;
    edited.name = name_->text().trimmed();
    if (edited.name.isEmpty()) { edited.name = QFileInfo(raw_->text()).completeBaseName(); }
    if (edited.name.isEmpty()) { error_->setText(QStringLiteral("请填写任务名称。")); return; }
    edited.options.task_definition.config_file_path = config_->text().trimmed();
    edited.options.task_definition.raw_file_path = raw_->text().trimmed();
    edited.options.task_definition.user_sample_rate_hz = rate_->value();
    edited.options.parameter_group_file_path = group_->text().trimmed();
    edited.options.parameter_names = selected_names_;
    edited.options.include_raw_values = raw_values_->isChecked();
    edited.time_mode = time_mode_->currentIndex();
    edited.time_range_file_path = ProcessingOptionsSupport::absolutePath(ranges_path_->text());
    edited.options.time_ranges.clear();
    edited.time_ranges_resolved = true;
    QString error;
    if (edited.time_mode == 1) {
        TaskTimeRange range;
        if (!TimeRangeParser::parseTimeValue(start_->text(), &range.start_seconds, &error) ||
            !TimeRangeParser::parseTimeValue(end_->text(), &range.end_seconds, &error) ||
            range.end_seconds <= range.start_seconds) {
            error_->setText(QStringLiteral("单时间段无效：%1").arg(error)); return;
        }
        edited.options.time_ranges = {range};
    } else if (edited.time_mode == 2) {
        if (ranges_->toPlainText().trimmed().isEmpty()) { edited.time_ranges_resolved = false; }
        else if (!TimeRangeParser::parseText(ranges_->toPlainText(), &edited.options.time_ranges, nullptr, &error)) {
            error_->setText(error); return;
        }
    }
    edited.config_fingerprint = ProcessingOptionsSupport::fingerprint(edited.options.task_definition.config_file_path);
    if (edited.source_description.isEmpty()) { edited.source_description = QStringLiteral("手动添加"); }
    edited.options = edited.processingOptions();
    item_ = edited;
    QDialog::accept();
}
