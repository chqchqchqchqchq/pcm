#include "main_window.h"

#include "traditional_processing_controller.h"

#include <QAbstractItemView>
#include <QAbstractSpinBox>
#include <QApplication>
#include <QCheckBox>
#include <QComboBox>
#include <QDateTime>
#include <QDialog>
#include <QDialogButtonBox>
#include <QDir>
#include <QDoubleSpinBox>
#include <QFile>
#include <QFileDialog>
#include <QFileInfo>
#include <QFormLayout>
#include <QFrame>
#include <QGridLayout>
#include <QGroupBox>
#include <QHBoxLayout>
#include <QHeaderView>
#include <QLabel>
#include <QLineEdit>
#include <QListWidget>
#include <QListWidgetItem>
#include <QMessageBox>
#include <QPlainTextEdit>
#include <QProgressBar>
#include <QPushButton>
#include <QRadioButton>
#include <QScrollArea>
#include <QSizePolicy>
#include <QStatusBar>
#include <QStyle>
#include <QTabWidget>
#include <QTableWidget>
#include <QTableWidgetItem>
#include <QTextEdit>
#include <QTime>
#include <QTimeEdit>
#include <QVBoxLayout>

namespace {

constexpr double kMaxSampleRate = 1000000000000.0;

enum ManagedConfigColumn
{
    StreamColumn,
    SerialColumn,
    IdColumn,
    NameColumn,
    UnitColumn,
    CalibrationColumn,
    CodeTypeColumn,
    RateColumn,
    PcmWordsColumn,
    FrameNoColumn,
    WordNoColumn,
    SourceBitColumn,
    BitLengthColumn,
    ObjectBitColumn,
    DescriptionColumn,
    ManagedConfigColumnCount
};

QStringList managedConfigTableHeaders()
{
    return {
        QStringLiteral("流号"),
        QStringLiteral("序号"),
        QStringLiteral("ID"),
        QStringLiteral("名称"),
        QStringLiteral("单位"),
        QStringLiteral("校准"),
        QStringLiteral("编码"),
        QStringLiteral("频率"),
        QStringLiteral("PCM字数"),
        QStringLiteral("帧号"),
        QStringLiteral("字号"),
        QStringLiteral("源位"),
        QStringLiteral("位长"),
        QStringLiteral("目标位"),
        QStringLiteral("描述"),
    };
}

QString managedConfigCellValue(const ConfigParameterRecord &record, int column)
{
    switch (column) {
    case StreamColumn:
        return QString::number(record.stream_index);
    case SerialColumn:
        return QString::number(record.serial_no);
    case IdColumn:
        return record.idNo();
    case NameColumn:
        return record.name();
    case UnitColumn:
        return record.value(QStringLiteral("unit"));
    case CalibrationColumn:
        return record.value(QStringLiteral("cal_type"));
    case CodeTypeColumn:
        return record.value(QStringLiteral("code_type"));
    case RateColumn:
        return record.value(QStringLiteral("rate"));
    case PcmWordsColumn:
        return record.value(QStringLiteral("pcm_words"));
    case FrameNoColumn:
        return record.value(QStringLiteral("frame_no"));
    case WordNoColumn:
        return record.value(QStringLiteral("word_no"));
    case SourceBitColumn:
        return record.value(QStringLiteral("source_bit"));
    case BitLengthColumn:
        return record.value(QStringLiteral("bit_length"));
    case ObjectBitColumn:
        return record.value(QStringLiteral("object_bit"));
    case DescriptionColumn:
        return record.value(QStringLiteral("description"));
    default:
        return QString();
    }
}

QString makeTaskId()
{
    const QString timestamp = QDateTime::currentDateTime().toString(QStringLiteral("yyyyMMddhhmmsszzz"));
    return QStringLiteral("traditional_%1").arg(timestamp);
}

QString normalizedPathText(const QString &path)
{
    return QDir::toNativeSeparators(QDir::cleanPath(path));
}

double secondsFromTime(const QTime &time)
{
    return static_cast<double>(QTime(0, 0, 0).secsTo(time));
}

QString stripParameterToken(const QString &token)
{
    QString normalized = token.trimmed();
    normalized.remove(QChar(0xfeff));
    if (normalized.size() >= 2 &&
        ((normalized.startsWith(QLatin1Char('"')) && normalized.endsWith(QLatin1Char('"'))) ||
         (normalized.startsWith(QLatin1Char('\'')) && normalized.endsWith(QLatin1Char('\''))))) {
        normalized = normalized.mid(1, normalized.size() - 2).trimmed();
    }

    return normalized;
}

QString textFromBytes(const QByteArray &content_bytes)
{
    QString content = QString::fromUtf8(content_bytes);
    if (content.contains(QChar(QChar::ReplacementCharacter))) {
        content = QString::fromLocal8Bit(content_bytes);
    }

    return content;
}

bool containsParameterName(const QStringList &parameter_names, const QString &parameter_name)
{
    for (const QString &existing_name : parameter_names) {
        if (existing_name.compare(parameter_name, Qt::CaseInsensitive) == 0) {
            return true;
        }
    }

    return false;
}

QStringList parseParameterNames(const QString &content)
{
    QStringList parameter_names;
    QString token;

    const auto flush_token = [&parameter_names, &token]() {
        const QString name = token.trimmed();
        if (!name.isEmpty()) {
            parameter_names.append(name);
        }
        token.clear();
    };

    for (const QChar character : content) {
        if (character.isSpace() || character == QLatin1Char(',') || character == QLatin1Char(';') ||
            character == QChar(0xff0c) || character == QChar(0xff1b)) {
            flush_token();
            continue;
        }

        token.append(character);
    }

    flush_token();
    return parameter_names;
}

} // namespace

MainWindow::MainWindow(QWidget *parent)
    : QMainWindow(parent)
{
    qRegisterMetaType<TraditionalProcessingOptions>("TraditionalProcessingOptions");

    traditional_controller_ = new TraditionalProcessingController(this);
    connect(this,
            &MainWindow::traditionalProcessingRequested,
            traditional_controller_,
            &TraditionalProcessingController::startProcessing);
    connect(this,
            &MainWindow::processingInterrupted,
            traditional_controller_,
            &TraditionalProcessingController::requestStop);
    connect(traditional_controller_,
            &TraditionalProcessingController::taskStarted,
            this,
            &MainWindow::handleTraditionalTaskStarted);
    connect(traditional_controller_,
            &TraditionalProcessingController::taskProgressChanged,
            this,
            &MainWindow::handleTraditionalTaskProgress);
    connect(traditional_controller_,
            &TraditionalProcessingController::taskFinished,
            this,
            &MainWindow::handleTraditionalTaskFinished);
    connect(traditional_controller_,
            &TraditionalProcessingController::taskFailed,
            this,
            &MainWindow::handleTraditionalTaskFailed);
    connect(traditional_controller_,
            &TraditionalProcessingController::taskStopped,
            this,
            &MainWindow::handleTraditionalTaskStopped);
    connect(&Logger::instance(), &Logger::messageLogged, this, &MainWindow::handleBackendLogRecord);

    setWindowTitle(QStringLiteral("PCM 帧数据事后解算系统"));
    resize(1040, 720);
    setMinimumSize(900, 620);
    setCentralWidget(createCentralWidget());
    statusBar()->showMessage(QStringLiteral("就绪"));
    interrupt_button_->setEnabled(false);
    state_label_->setText(QStringLiteral("姝ｅ湪涓柇"));
    statusBar()->showMessage(QStringLiteral("姝ｅ湪涓柇"));
    setProcessingState(false);
}

void MainWindow::browseHeaderFile()
{
    const QString file_path = QFileDialog::getOpenFileName(
        this,
        QStringLiteral("选择头文件/配置文件"),
        QString(),
        QStringLiteral("配置文件 (*.cfg *.ini *.txt *.json *.xml);;所有文件 (*.*)"));

    if (!file_path.isEmpty()) {
        header_file_edit_->setText(normalizedPathText(file_path));
        QString error_message;
        if (!loadConfigParameterFile(file_path, &error_message)) {
            QMessageBox::warning(this, QStringLiteral("配置参数读取失败"), error_message);
        }
    }
}

void MainWindow::browseRawFile()
{
    const QString file_path = QFileDialog::getOpenFileName(
        this,
        QStringLiteral("选择原始数据文件"),
        QString(),
        QStringLiteral("原始数据文件 (*.pcm *.dat *.bin *.raw);;所有文件 (*.*)"));

    if (!file_path.isEmpty()) {
        raw_file_edit_->setText(normalizedPathText(file_path));
    }
}

void MainWindow::browseParameterGroupFile()
{
    const QString file_path = QFileDialog::getOpenFileName(
        this,
        QStringLiteral("选择参数组文件"),
        QString(),
        QStringLiteral("参数组文件 (*.txt *.csv *.lst);;所有文件 (*.*)"));

    if (file_path.isEmpty()) {
        return;
    }

    QString error_message;
    if (!loadParameterGroupFile(file_path, &error_message)) {
        QMessageBox::warning(this, QStringLiteral("参数组文件无效"), error_message);
    }
}

void MainWindow::browseTimeRangeFile()
{
    const QString file_path = QFileDialog::getOpenFileName(
        this,
        QStringLiteral("选择时间段文本文件"),
        QString(),
        QStringLiteral("文本文件 (*.txt *.csv);;所有文件 (*.*)"));

    if (!file_path.isEmpty()) {
        time_range_file_edit_->setText(normalizedPathText(file_path));
    }
}

void MainWindow::browseOutputDirectory()
{
    const QString current_path = output_dir_edit_->text().trimmed();
    const QString suggested_path = current_path.isEmpty()
        ? QDir::home().filePath(QStringLiteral("pcm_results.txt"))
        : current_path;
    const QString file_path = QFileDialog::getSaveFileName(
        this,
        QStringLiteral("指定输出结果文件"),
        suggested_path,
        QStringLiteral("文本文件 (*.txt);;CSV 文件 (*.csv);;所有文件 (*.*)"));

    if (!file_path.isEmpty()) {
        output_dir_edit_->setText(normalizedPathText(file_path));
    }
}

void MainWindow::updateTimeMode()
{
    const bool single_time_mode = single_time_radio_->isChecked();
    const bool multiple_time_mode = multiple_time_radio_->isChecked();

    start_time_edit_->setEnabled(single_time_mode);
    end_time_edit_->setEnabled(single_time_mode);
    time_range_file_edit_->setEnabled(multiple_time_mode);
    time_range_browse_button_->setEnabled(multiple_time_mode);
}

void MainWindow::updateConfigParameterFilter()
{
    refreshConfigParameterList();
}

void MainWindow::updateProcessingParameterFilter()
{
    refreshProcessingParameterList();
}

void MainWindow::addSelectedConfigParameters()
{
    QStringList selected_parameter_names;
    const QList<QListWidgetItem *> selected_items = config_parameter_list_widget_->selectedItems();
    for (const QListWidgetItem *item : selected_items) {
        selected_parameter_names.append(item->data(Qt::UserRole).toString());
    }

    addProcessingParameters(selected_parameter_names);
}

void MainWindow::addVisibleConfigParameters()
{
    QStringList visible_parameter_names;
    for (int row = 0; row < config_parameter_list_widget_->count(); ++row) {
        visible_parameter_names.append(config_parameter_list_widget_->item(row)->data(Qt::UserRole).toString());
    }

    addProcessingParameters(visible_parameter_names);
}

void MainWindow::removeSelectedProcessingParameters()
{
    QStringList removed_parameter_names;
    const QList<QListWidgetItem *> selected_items = processing_parameter_list_widget_->selectedItems();
    for (const QListWidgetItem *item : selected_items) {
        const QString parameter_name = item->data(Qt::UserRole).toString();
        if (parameter_name.compare(QStringLiteral("TIME"), Qt::CaseInsensitive) == 0) {
            continue;
        }

        removed_parameter_names.append(parameter_name);
    }

    for (const QString &parameter_name : removed_parameter_names) {
        for (int index = parameter_names_.size() - 1; index >= 0; --index) {
            if (parameter_names_.at(index).compare(parameter_name, Qt::CaseInsensitive) == 0) {
                parameter_names_.removeAt(index);
                break;
            }
        }
    }

    if (!removed_parameter_names.isEmpty()) {
        appendLog(QStringLiteral("已从处理参数组删除 %1 个参数。").arg(removed_parameter_names.size()));
    }

    refreshProcessingParameterList();
    updateParameterCountLabels();
}

void MainWindow::startProcessing()
{
    QString error_message;
    if (!validateTraditionalForm(&error_message)) {
        QMessageBox::warning(this, QStringLiteral("参数不完整"), error_message);
        return;
    }

    const TraditionalProcessingOptions options = buildTraditionalOptions();
    current_traditional_task_id_ = options.task_definition.task_id;
    setProcessingState(true);
    progress_bar_->setValue(0);
    appendLog(QStringLiteral("传统处理任务已创建：%1").arg(options.task_definition.task_id));
    emit traditionalProcessingRequested(options);
}

void MainWindow::interruptProcessing()
{
    if (!is_processing_) {
        return;
    }

    appendLog(QStringLiteral("用户请求中断当前处理。"));
    emit processingInterrupted();
    interrupt_button_->setEnabled(false);
    state_label_->setText(QStringLiteral("姝ｅ湪涓柇"));
    statusBar()->showMessage(QStringLiteral("姝ｅ湪涓柇"));
}

void MainWindow::handleTraditionalTaskStarted(const QString &task_id)
{
    current_traditional_task_id_ = task_id;
    setProcessingState(true);
    appendLog(QStringLiteral("后端已开始处理任务：%1").arg(task_id));
}

void MainWindow::handleTraditionalTaskProgress(const QString &task_id,
                                               int progress_percent,
                                               const QString &detail)
{
    if (!current_traditional_task_id_.isEmpty() && task_id != current_traditional_task_id_) {
        return;
    }

    progress_bar_->setValue(qBound(0, progress_percent, 100));
    state_label_->setText(detail);
    statusBar()->showMessage(detail);
}

void MainWindow::handleTraditionalTaskFinished(const QString &task_id, const QString &output_path)
{
    if (!current_traditional_task_id_.isEmpty() && task_id != current_traditional_task_id_) {
        return;
    }

    progress_bar_->setValue(100);
    appendLog(QStringLiteral("传统处理任务完成，输出：%1").arg(output_path));
    current_traditional_task_id_.clear();
    setProcessingState(false);
}

void MainWindow::handleTraditionalTaskFailed(const QString &task_id, const QString &reason)
{
    if (!current_traditional_task_id_.isEmpty() && task_id != current_traditional_task_id_) {
        return;
    }

    appendLog(QStringLiteral("传统处理任务失败：%1").arg(reason));
    current_traditional_task_id_.clear();
    setProcessingState(false);
}

void MainWindow::handleTraditionalTaskStopped(const QString &task_id)
{
    if (!current_traditional_task_id_.isEmpty() && task_id != current_traditional_task_id_) {
        return;
    }

    appendLog(QStringLiteral("传统处理任务已中断：%1").arg(task_id));
    current_traditional_task_id_.clear();
    setProcessingState(false);
}

void MainWindow::handleBackendLogRecord(const LogRecord &record)
{
    if (log_view_ == nullptr) {
        return;
    }

    const QString task_text = record.task_id.isEmpty() ? QStringLiteral("-") : record.task_id;
    const QString parameter_text = record.parameter_name.isEmpty() ? QStringLiteral("-") : record.parameter_name;
    log_view_->appendPlainText(QStringLiteral("[%1] [%2] [%3] [task=%4] [param=%5] %6")
                                   .arg(record.timestamp.toString(Qt::ISODateWithMs),
                                        Logger::levelToString(record.level),
                                        record.module,
                                        task_text,
                                        parameter_text,
                                        record.message));
}

void MainWindow::browseManagedConfigFile()
{
    const QString file_path = QFileDialog::getOpenFileName(
        this,
        QStringLiteral("导入配置文件"),
        QString(),
        QStringLiteral("配置文件 (*.cfg *.ini *.txt);;所有文件 (*.*)"));
    if (file_path.isEmpty()) {
        return;
    }

    QString error_message;
    ConfigFileDocument document;
    if (!document.loadFromFile(file_path, &error_message)) {
        QMessageBox::warning(this, QStringLiteral("配置文件导入失败"), error_message);
        return;
    }

    managed_config_document_ = document;
    managed_config_path_edit_->setText(normalizedPathText(file_path));
    if (managed_config_search_edit_ != nullptr) {
        managed_config_search_edit_->clear();
    }
    refreshManagedConfigParameterTable();
    updateManagedConfigState();
    statusBar()->showMessage(QStringLiteral("已导入配置文件：%1").arg(QFileInfo(file_path).fileName()));
}

void MainWindow::filterManagedConfigParameters()
{
    refreshManagedConfigParameterTable();
}

void MainWindow::addManagedConfigParameter()
{
    if (!managed_config_document_.hasContent()) {
        QMessageBox::information(this, QStringLiteral("尚未导入配置"), QStringLiteral("请先导入一个配置文件。"));
        return;
    }

    const QList<int> stream_indexes = managed_config_document_.streamIndexes();
    if (stream_indexes.isEmpty()) {
        QMessageBox::warning(this, QStringLiteral("无法新增参数"), QStringLiteral("配置文件中没有可用的数据流。"));
        return;
    }

    ConfigParameterRecord record = makeDefaultManagedConfigParameter(stream_indexes.first());
    if (!openManagedConfigParameterDialog(QStringLiteral("新增参数"), &record, true, false)) {
        return;
    }

    QString error_message;
    if (!managed_config_document_.addParameter(record, &error_message)) {
        QMessageBox::warning(this, QStringLiteral("新增参数失败"), error_message);
        return;
    }

    refreshManagedConfigParameterTable();
    updateManagedConfigState();
}

void MainWindow::editManagedConfigParameter()
{
    const int parameter_index = selectedManagedConfigParameterIndex();
    if (parameter_index < 0) {
        return;
    }

    QList<ConfigParameterRecord> parameters = managed_config_document_.parameters();
    if (parameter_index >= parameters.size()) {
        return;
    }

    ConfigParameterRecord record = parameters.at(parameter_index);
    if (!openManagedConfigParameterDialog(QStringLiteral("编辑参数"), &record, false, false)) {
        return;
    }

    QString error_message;
    if (!managed_config_document_.updateParameter(parameter_index, record, &error_message)) {
        QMessageBox::warning(this, QStringLiteral("编辑参数失败"), error_message);
        return;
    }

    refreshManagedConfigParameterTable();
    updateManagedConfigState();
}

void MainWindow::deleteManagedConfigParameter()
{
    const int parameter_index = selectedManagedConfigParameterIndex();
    if (parameter_index < 0) {
        return;
    }

    const QList<ConfigParameterRecord> parameters = managed_config_document_.parameters();
    if (parameter_index >= parameters.size()) {
        return;
    }

    const ConfigParameterRecord record = parameters.at(parameter_index);
    const int result = QMessageBox::question(
        this,
        QStringLiteral("删除参数"),
        QStringLiteral("确定删除参数 %1 / %2 吗？").arg(record.idNo(), record.name()));
    if (result != QMessageBox::Yes) {
        return;
    }

    QString error_message;
    if (!managed_config_document_.removeParameter(parameter_index, &error_message)) {
        QMessageBox::warning(this, QStringLiteral("删除参数失败"), error_message);
        return;
    }

    refreshManagedConfigParameterTable();
    updateManagedConfigState();
}

void MainWindow::viewManagedConfigParameter()
{
    const int parameter_index = selectedManagedConfigParameterIndex();
    if (parameter_index < 0) {
        return;
    }

    const QList<ConfigParameterRecord> parameters = managed_config_document_.parameters();
    if (parameter_index >= parameters.size()) {
        return;
    }

    ConfigParameterRecord record = parameters.at(parameter_index);
    openManagedConfigParameterDialog(QStringLiteral("参数详情"), &record, false, true);
}

void MainWindow::exportManagedConfigFile()
{
    if (!managed_config_document_.hasContent()) {
        QMessageBox::information(this, QStringLiteral("尚未导入配置"), QStringLiteral("请先导入一个配置文件。"));
        return;
    }

    const QFileInfo source_info(managed_config_document_.sourcePath());
    const QString suggested_name = source_info.completeBaseName().isEmpty()
        ? QStringLiteral("modified_config.cfg")
        : QStringLiteral("%1_modified.%2")
              .arg(source_info.completeBaseName(),
                   source_info.suffix().isEmpty() ? QStringLiteral("cfg") : source_info.suffix());
    const QString suggested_path = source_info.absoluteDir().filePath(suggested_name);
    const QString file_path = QFileDialog::getSaveFileName(
        this,
        QStringLiteral("导出新配置文件"),
        suggested_path,
        QStringLiteral("配置文件 (*.cfg *.ini *.txt);;所有文件 (*.*)"));
    if (file_path.isEmpty()) {
        return;
    }

    QString error_message;
    if (!managed_config_document_.saveAs(file_path, &error_message)) {
        QMessageBox::warning(this, QStringLiteral("导出配置失败"), error_message);
        return;
    }

    managed_config_path_edit_->setText(normalizedPathText(file_path));
    refreshManagedConfigParameterTable();
    updateManagedConfigState();
    QMessageBox::information(this, QStringLiteral("导出完成"), QStringLiteral("新配置文件已导出。"));
}

QWidget *MainWindow::createCentralWidget()
{
    mode_tabs_ = new QTabWidget(this);
    mode_tabs_->setDocumentMode(true);
    mode_tabs_->addTab(createTraditionalPage(), QStringLiteral("传统处理模式"));
    mode_tabs_->addTab(createMultiTaskPage(), QStringLiteral("多任务处理"));
    mode_tabs_->addTab(createConfigManagementPage(), QStringLiteral("配置文件管理"));
    return mode_tabs_;
}

QWidget *MainWindow::createTraditionalPage()
{
    QWidget *page = new QWidget(this);
    QVBoxLayout *page_layout = new QVBoxLayout(page);
    page_layout->setContentsMargins(18, 18, 18, 18);
    page_layout->setSpacing(14);

    QGridLayout *form_grid = new QGridLayout;
    form_grid->setColumnStretch(0, 1);
    form_grid->setColumnStretch(1, 1);
    form_grid->setHorizontalSpacing(14);
    form_grid->setVerticalSpacing(14);
    form_grid->addWidget(createInputGroup(), 0, 0, 1, 2);
    form_grid->addWidget(createTimeGroup(), 1, 0);
    form_grid->addWidget(createOutputGroup(), 1, 1);

    page_layout->addLayout(form_grid);
    page_layout->addWidget(createActionArea());
    return page;
}

QWidget *MainWindow::createMultiTaskPage()
{
    QWidget *page = new QWidget(this);
    QVBoxLayout *layout = new QVBoxLayout(page);
    layout->setContentsMargins(24, 24, 24, 24);
    layout->setSpacing(12);

    QLabel *title_label = new QLabel(QStringLiteral("多任务处理模式"), page);
    QFont title_font = title_label->font();
    title_font.setPointSize(title_font.pointSize() + 4);
    title_font.setBold(true);
    title_label->setFont(title_font);

    QPushButton *batch_entry_button = new QPushButton(QStringLiteral("进入多任务队列"), page);
    batch_entry_button->setIcon(style()->standardIcon(QStyle::SP_FileDialogDetailedView));
    batch_entry_button->setEnabled(false);
    batch_entry_button->setMinimumHeight(36);

    layout->addWidget(title_label);
    layout->addWidget(batch_entry_button, 0, Qt::AlignLeft);
    layout->addStretch(1);
    return page;
}

QWidget *MainWindow::createConfigManagementPage()
{
    QWidget *page = new QWidget(this);
    QVBoxLayout *layout = new QVBoxLayout(page);
    layout->setContentsMargins(18, 18, 18, 18);
    layout->setSpacing(10);

    QLabel *title_label = new QLabel(QStringLiteral("配置文件管理"), page);
    QFont title_font = title_label->font();
    title_font.setPointSize(title_font.pointSize() + 3);
    title_font.setBold(true);
    title_label->setFont(title_font);

    QHBoxLayout *file_layout = new QHBoxLayout;
    managed_config_path_edit_ = new QLineEdit(page);
    managed_config_path_edit_->setReadOnly(true);
    managed_config_path_edit_->setPlaceholderText(QStringLiteral("尚未导入配置文件"));

    QPushButton *import_button = new QPushButton(QStringLiteral("导入配置"), page);
    import_button->setIcon(style()->standardIcon(QStyle::SP_DialogOpenButton));
    connect(import_button, &QPushButton::clicked, this, &MainWindow::browseManagedConfigFile);

    managed_config_export_button_ = new QPushButton(QStringLiteral("导出新配置"), page);
    managed_config_export_button_->setIcon(style()->standardIcon(QStyle::SP_DialogSaveButton));
    connect(managed_config_export_button_, &QPushButton::clicked, this, &MainWindow::exportManagedConfigFile);

    file_layout->addWidget(managed_config_path_edit_, 1);
    file_layout->addWidget(import_button);
    file_layout->addWidget(managed_config_export_button_);

    QHBoxLayout *tool_layout = new QHBoxLayout;
    managed_config_search_edit_ = new QLineEdit(page);
    managed_config_search_edit_->setPlaceholderText(QStringLiteral("查询流号、ID、名称、单位、描述"));
    connect(managed_config_search_edit_,
            &QLineEdit::textChanged,
            this,
            &MainWindow::filterManagedConfigParameters);

    managed_config_add_button_ = new QPushButton(QStringLiteral("新增"), page);
    managed_config_edit_button_ = new QPushButton(QStringLiteral("编辑"), page);
    managed_config_delete_button_ = new QPushButton(QStringLiteral("删除"), page);
    managed_config_view_button_ = new QPushButton(QStringLiteral("查看详情"), page);
    managed_config_add_button_->setIcon(style()->standardIcon(QStyle::SP_FileIcon));
    managed_config_edit_button_->setIcon(style()->standardIcon(QStyle::SP_FileDialogDetailedView));
    managed_config_delete_button_->setIcon(style()->standardIcon(QStyle::SP_TrashIcon));
    managed_config_view_button_->setIcon(style()->standardIcon(QStyle::SP_FileDialogInfoView));
    connect(managed_config_add_button_, &QPushButton::clicked, this, &MainWindow::addManagedConfigParameter);
    connect(managed_config_edit_button_, &QPushButton::clicked, this, &MainWindow::editManagedConfigParameter);
    connect(managed_config_delete_button_, &QPushButton::clicked, this, &MainWindow::deleteManagedConfigParameter);
    connect(managed_config_view_button_, &QPushButton::clicked, this, &MainWindow::viewManagedConfigParameter);

    tool_layout->addWidget(managed_config_search_edit_, 1);
    tool_layout->addWidget(managed_config_add_button_);
    tool_layout->addWidget(managed_config_edit_button_);
    tool_layout->addWidget(managed_config_delete_button_);
    tool_layout->addWidget(managed_config_view_button_);

    managed_config_table_ = new QTableWidget(page);
    managed_config_table_->setColumnCount(ManagedConfigColumnCount);
    managed_config_table_->setHorizontalHeaderLabels(managedConfigTableHeaders());
    managed_config_table_->setEditTriggers(QAbstractItemView::NoEditTriggers);
    managed_config_table_->setSelectionBehavior(QAbstractItemView::SelectRows);
    managed_config_table_->setSelectionMode(QAbstractItemView::SingleSelection);
    managed_config_table_->setAlternatingRowColors(true);
    managed_config_table_->verticalHeader()->setVisible(false);
    managed_config_table_->horizontalHeader()->setStretchLastSection(true);
    managed_config_table_->horizontalHeader()->setSectionResizeMode(QHeaderView::ResizeToContents);
    managed_config_table_->setMinimumHeight(360);
    connect(managed_config_table_,
            &QTableWidget::itemSelectionChanged,
            this,
            &MainWindow::updateManagedConfigState);
    connect(managed_config_table_, &QTableWidget::cellDoubleClicked, this, [this](int, int) {
        editManagedConfigParameter();
    });

    managed_config_status_label_ = new QLabel(QStringLiteral("未导入配置文件"), page);

    layout->addWidget(title_label);
    layout->addLayout(file_layout);
    layout->addLayout(tool_layout);
    layout->addWidget(managed_config_table_, 1);
    layout->addWidget(managed_config_status_label_);
    updateManagedConfigState();
    return page;
}

QGroupBox *MainWindow::createInputGroup()
{
    QGroupBox *group_box = new QGroupBox(QStringLiteral("输入与参数"), this);
    QFormLayout *form_layout = new QFormLayout(group_box);
    form_layout->setLabelAlignment(Qt::AlignRight | Qt::AlignVCenter);
    form_layout->setFieldGrowthPolicy(QFormLayout::ExpandingFieldsGrow);
    form_layout->setHorizontalSpacing(12);
    form_layout->setVerticalSpacing(10);

    header_file_edit_ = new QLineEdit(group_box);
    header_file_edit_->setReadOnly(true);
    QPushButton *header_browse_button = new QPushButton(group_box);
    header_browse_button->setIcon(style()->standardIcon(QStyle::SP_DialogOpenButton));
    header_browse_button->setToolTip(QStringLiteral("选择头文件/配置文件"));
    connect(header_browse_button, &QPushButton::clicked, this, &MainWindow::browseHeaderFile);

    raw_file_edit_ = new QLineEdit(group_box);
    raw_file_edit_->setReadOnly(true);
    QPushButton *raw_browse_button = new QPushButton(group_box);
    raw_browse_button->setIcon(style()->standardIcon(QStyle::SP_DialogOpenButton));
    raw_browse_button->setToolTip(QStringLiteral("选择原始数据文件"));
    connect(raw_browse_button, &QPushButton::clicked, this, &MainWindow::browseRawFile);

    parameter_group_file_edit_ = new QLineEdit(group_box);
    parameter_group_file_edit_->setReadOnly(true);
    QPushButton *parameter_group_browse_button = new QPushButton(group_box);
    parameter_group_browse_button->setIcon(style()->standardIcon(QStyle::SP_DialogOpenButton));
    parameter_group_browse_button->setToolTip(QStringLiteral("选择参数组文件"));
    connect(parameter_group_browse_button, &QPushButton::clicked, this, &MainWindow::browseParameterGroupFile);

    form_layout->addRow(QStringLiteral("头文件/配置文件"), createPathSelector(header_file_edit_, header_browse_button));
    form_layout->addRow(QStringLiteral("原始文件"), createPathSelector(raw_file_edit_, raw_browse_button));
    form_layout->addRow(QStringLiteral("参数组文件"),
                        createPathSelector(parameter_group_file_edit_, parameter_group_browse_button));
    form_layout->addRow(QStringLiteral("参数选择"), createParameterSelector(group_box));
    return group_box;
}

QWidget *MainWindow::createParameterSelector(QWidget *parent)
{
    QWidget *container = new QWidget(parent);
    QGridLayout *grid_layout = new QGridLayout(container);
    grid_layout->setContentsMargins(0, 0, 0, 0);
    grid_layout->setHorizontalSpacing(10);
    grid_layout->setVerticalSpacing(6);
    grid_layout->setColumnStretch(0, 1);
    grid_layout->setColumnStretch(2, 1);

    QLabel *config_title_label = new QLabel(QStringLiteral("配置文件参数"), container);
    QLabel *processing_title_label = new QLabel(QStringLiteral("要处理的参数组"), container);
    QFont title_font = config_title_label->font();
    title_font.setBold(true);
    config_title_label->setFont(title_font);
    processing_title_label->setFont(title_font);

    config_parameter_search_edit_ = new QLineEdit(container);
    config_parameter_search_edit_->setPlaceholderText(QStringLiteral("查找配置参数"));
    processing_parameter_search_edit_ = new QLineEdit(container);
    processing_parameter_search_edit_->setPlaceholderText(QStringLiteral("查找处理参数"));

    config_parameter_list_widget_ = new QListWidget(container);
    config_parameter_list_widget_->setAlternatingRowColors(true);
    config_parameter_list_widget_->setSelectionMode(QAbstractItemView::ExtendedSelection);
    config_parameter_list_widget_->setMinimumHeight(180);

    processing_parameter_list_widget_ = new QListWidget(container);
    processing_parameter_list_widget_->setAlternatingRowColors(true);
    processing_parameter_list_widget_->setSelectionMode(QAbstractItemView::ExtendedSelection);
    processing_parameter_list_widget_->setMinimumHeight(180);

    QPushButton *add_selected_button = new QPushButton(QStringLiteral("添加选中 >"), container);
    QPushButton *add_visible_button = new QPushButton(QStringLiteral("添加全部 >"), container);
    QPushButton *remove_selected_button = new QPushButton(QStringLiteral("< 删除选中"), container);
    add_selected_button->setToolTip(QStringLiteral("将左侧选中的配置参数加入要处理的参数组"));
    add_visible_button->setToolTip(QStringLiteral("将左侧当前查找结果全部加入要处理的参数组"));
    remove_selected_button->setToolTip(QStringLiteral("从要处理的参数组中删除选中参数，TIME 不会被删除"));

    QWidget *button_container = new QWidget(container);
    QVBoxLayout *button_layout = new QVBoxLayout(button_container);
    button_layout->setContentsMargins(0, 30, 0, 0);
    button_layout->setSpacing(8);
    button_layout->addWidget(add_selected_button);
    button_layout->addWidget(add_visible_button);
    button_layout->addSpacing(12);
    button_layout->addWidget(remove_selected_button);
    button_layout->addStretch(1);

    config_parameter_count_label_ = new QLabel(QStringLiteral("配置参数：0"), container);
    processing_parameter_count_label_ = new QLabel(QStringLiteral("处理参数：0"), container);

    grid_layout->addWidget(config_title_label, 0, 0);
    grid_layout->addWidget(processing_title_label, 0, 2);
    grid_layout->addWidget(config_parameter_search_edit_, 1, 0);
    grid_layout->addWidget(processing_parameter_search_edit_, 1, 2);
    grid_layout->addWidget(config_parameter_list_widget_, 2, 0);
    grid_layout->addWidget(button_container, 2, 1);
    grid_layout->addWidget(processing_parameter_list_widget_, 2, 2);
    grid_layout->addWidget(config_parameter_count_label_, 3, 0);
    grid_layout->addWidget(processing_parameter_count_label_, 3, 2);

    connect(config_parameter_search_edit_,
            &QLineEdit::textChanged,
            this,
            &MainWindow::updateConfigParameterFilter);
    connect(processing_parameter_search_edit_,
            &QLineEdit::textChanged,
            this,
            &MainWindow::updateProcessingParameterFilter);
    connect(add_selected_button, &QPushButton::clicked, this, &MainWindow::addSelectedConfigParameters);
    connect(add_visible_button, &QPushButton::clicked, this, &MainWindow::addVisibleConfigParameters);
    connect(remove_selected_button, &QPushButton::clicked, this, &MainWindow::removeSelectedProcessingParameters);

    updateParameterCountLabels();
    return container;
}

QGroupBox *MainWindow::createTimeGroup()
{
    QGroupBox *group_box = new QGroupBox(QStringLiteral("采样率与时间段"), this);
    QVBoxLayout *outer_layout = new QVBoxLayout(group_box);
    outer_layout->setSpacing(10);

    QFormLayout *sample_layout = new QFormLayout;
    sample_layout->setLabelAlignment(Qt::AlignRight | Qt::AlignVCenter);
    sample_rate_spin_ = new QDoubleSpinBox(group_box);
    sample_rate_spin_->setDecimals(6);
    sample_rate_spin_->setRange(0.000001, kMaxSampleRate);
    sample_rate_spin_->setValue(1.0);
    sample_rate_spin_->setSuffix(QStringLiteral(" Hz"));
    sample_rate_spin_->setButtonSymbols(QAbstractSpinBox::NoButtons);
    sample_layout->addRow(QStringLiteral("用户输入采样率"), sample_rate_spin_);
    outer_layout->addLayout(sample_layout);

    QHBoxLayout *mode_layout = new QHBoxLayout;
    full_time_radio_ = new QRadioButton(QStringLiteral("全程处理"), group_box);
    single_time_radio_ = new QRadioButton(QStringLiteral("单时间段处理"), group_box);
    multiple_time_radio_ = new QRadioButton(QStringLiteral("多时间段处理"), group_box);
    full_time_radio_->setChecked(true);
    mode_layout->addWidget(full_time_radio_);
    mode_layout->addWidget(single_time_radio_);
    mode_layout->addWidget(multiple_time_radio_);
    mode_layout->addStretch(1);
    outer_layout->addLayout(mode_layout);

    QFormLayout *range_layout = new QFormLayout;
    range_layout->setLabelAlignment(Qt::AlignRight | Qt::AlignVCenter);

    start_time_edit_ = new QTimeEdit(QTime(0, 0, 0), group_box);
    start_time_edit_->setDisplayFormat(QStringLiteral("HH:mm:ss"));
    start_time_edit_->setButtonSymbols(QAbstractSpinBox::NoButtons);

    end_time_edit_ = new QTimeEdit(QTime(23, 59, 59), group_box);
    end_time_edit_->setDisplayFormat(QStringLiteral("HH:mm:ss"));
    end_time_edit_->setButtonSymbols(QAbstractSpinBox::NoButtons);

    time_range_file_edit_ = new QLineEdit(group_box);
    time_range_file_edit_->setReadOnly(true);
    time_range_browse_button_ = new QPushButton(group_box);
    time_range_browse_button_->setIcon(style()->standardIcon(QStyle::SP_DialogOpenButton));
    time_range_browse_button_->setToolTip(QStringLiteral("选择时间段文本文件"));
    connect(time_range_browse_button_, &QPushButton::clicked, this, &MainWindow::browseTimeRangeFile);

    range_layout->addRow(QStringLiteral("起始时间"), start_time_edit_);
    range_layout->addRow(QStringLiteral("结束时间"), end_time_edit_);
    range_layout->addRow(QStringLiteral("时间段文件"), createPathSelector(time_range_file_edit_, time_range_browse_button_));
    outer_layout->addLayout(range_layout);

    connect(full_time_radio_, &QRadioButton::toggled, this, &MainWindow::updateTimeMode);
    connect(single_time_radio_, &QRadioButton::toggled, this, &MainWindow::updateTimeMode);
    connect(multiple_time_radio_, &QRadioButton::toggled, this, &MainWindow::updateTimeMode);
    updateTimeMode();
    return group_box;
}

QGroupBox *MainWindow::createOutputGroup()
{
    QGroupBox *group_box = new QGroupBox(QStringLiteral("输出"), this);
    QFormLayout *form_layout = new QFormLayout(group_box);
    form_layout->setLabelAlignment(Qt::AlignRight | Qt::AlignVCenter);
    form_layout->setFieldGrowthPolicy(QFormLayout::ExpandingFieldsGrow);
    form_layout->setHorizontalSpacing(12);
    form_layout->setVerticalSpacing(10);

    output_source_check_ = new QCheckBox(QStringLiteral("输出源码"), group_box);

    output_dir_edit_ = new QLineEdit(group_box);
    output_dir_edit_->setReadOnly(true);
    QPushButton *output_browse_button = new QPushButton(group_box);
    output_browse_button->setIcon(style()->standardIcon(QStyle::SP_DialogOpenButton));
    output_browse_button->setToolTip(QStringLiteral("指定输出结果文件名"));
    connect(output_browse_button, &QPushButton::clicked, this, &MainWindow::browseOutputDirectory);

    form_layout->addRow(QStringLiteral("源码选项"), output_source_check_);
    form_layout->addRow(QStringLiteral("结果文件"), createPathSelector(output_dir_edit_, output_browse_button));
    return group_box;
}

QWidget *MainWindow::createActionArea()
{
    QFrame *frame = new QFrame(this);
    frame->setFrameShape(QFrame::NoFrame);
    QVBoxLayout *outer_layout = new QVBoxLayout(frame);
    outer_layout->setContentsMargins(0, 0, 0, 0);
    outer_layout->setSpacing(10);

    QHBoxLayout *action_layout = new QHBoxLayout;
    state_label_ = new QLabel(QStringLiteral("就绪"), frame);

    progress_bar_ = new QProgressBar(frame);
    progress_bar_->setRange(0, 100);
    progress_bar_->setValue(0);
    progress_bar_->setTextVisible(true);

    start_button_ = new QPushButton(QStringLiteral("开始处理"), frame);
    start_button_->setIcon(style()->standardIcon(QStyle::SP_MediaPlay));
    start_button_->setMinimumHeight(38);
    connect(start_button_, &QPushButton::clicked, this, &MainWindow::startProcessing);

    interrupt_button_ = new QPushButton(QStringLiteral("中断处理"), frame);
    interrupt_button_->setIcon(style()->standardIcon(QStyle::SP_BrowserStop));
    interrupt_button_->setMinimumHeight(38);
    connect(interrupt_button_, &QPushButton::clicked, this, &MainWindow::interruptProcessing);

    action_layout->addWidget(state_label_);
    action_layout->addWidget(progress_bar_, 1);
    action_layout->addWidget(start_button_);
    action_layout->addWidget(interrupt_button_);
    outer_layout->addLayout(action_layout);

    log_view_ = new QPlainTextEdit(frame);
    log_view_->setReadOnly(true);
    log_view_->setMinimumHeight(130);
    log_view_->setPlaceholderText(QStringLiteral("处理日志"));
    outer_layout->addWidget(log_view_);
    return frame;
}

QWidget *MainWindow::createPathSelector(QLineEdit *line_edit, QPushButton *browse_button)
{
    QWidget *container = new QWidget(this);
    QHBoxLayout *layout = new QHBoxLayout(container);
    layout->setContentsMargins(0, 0, 0, 0);
    layout->setSpacing(6);

    browse_button->setFixedWidth(34);
    line_edit->setSizePolicy(QSizePolicy::Expanding, QSizePolicy::Fixed);
    layout->addWidget(line_edit, 1);
    layout->addWidget(browse_button);
    return container;
}

bool MainWindow::validateTraditionalForm(QString *error_message) const
{
    auto set_error = [error_message](const QString &message) {
        if (error_message != nullptr) {
            *error_message = message;
        }
        return false;
    };

    if (header_file_edit_->text().trimmed().isEmpty()) {
        return set_error(QStringLiteral("请选择头文件/配置文件。"));
    }

    if (raw_file_edit_->text().trimmed().isEmpty()) {
        return set_error(QStringLiteral("请选择原始文件。"));
    }

    if (false && parameter_group_file_edit_->text().trimmed().isEmpty()) {
        return set_error(QStringLiteral("请选择参数组文件。"));
    }

    if (parameter_names_.isEmpty()) {
        return set_error(QStringLiteral("参数组文件中没有可处理的参数。"));
    }

    if (parameter_names_.first().compare(QStringLiteral("TIME"), Qt::CaseInsensitive) != 0) {
        return set_error(QStringLiteral("参数组文件的第一个参数必须是 TIME。"));
    }

    if (parameter_names_.size() <= 1) {
        return set_error(QStringLiteral("请选择至少一个需要处理的参数。"));
    }

    if (sample_rate_spin_->value() <= 0.0) {
        return set_error(QStringLiteral("用户输入采样率必须大于 0。"));
    }

    if (single_time_radio_->isChecked() && end_time_edit_->time() <= start_time_edit_->time()) {
        return set_error(QStringLiteral("结束时间必须大于起始时间。"));
    }

    if (multiple_time_radio_->isChecked() && time_range_file_edit_->text().trimmed().isEmpty()) {
        return set_error(QStringLiteral("请选择多时间段处理使用的文本文件。"));
    }

    if (output_dir_edit_->text().trimmed().isEmpty()) {
        return set_error(QStringLiteral("请指定输出结果文件。"));
    }

    return true;
}

TraditionalProcessingOptions MainWindow::buildTraditionalOptions() const
{
    TraditionalProcessingOptions options;
    options.parameter_group_file_path = parameter_group_file_edit_->text().trimmed();
    options.parameter_names = parameter_names_;
    options.use_multiple_time_ranges = multiple_time_radio_->isChecked();
    options.time_range_file_path = time_range_file_edit_->text().trimmed();
    options.output_file_path = output_dir_edit_->text().trimmed();
    options.output_source_code = output_source_check_->isChecked();

    ParameterGroupSelection parameter_group;
    const QFileInfo parameter_group_file_info(options.parameter_group_file_path);
    parameter_group.group_id = parameter_group_file_info.completeBaseName().isEmpty()
        ? QStringLiteral("ui_selected_parameters")
        : parameter_group_file_info.completeBaseName();
    parameter_group.display_name = parameter_group_file_info.fileName().isEmpty()
        ? QStringLiteral("UI界面选择参数")
        : parameter_group_file_info.fileName();
    for (int index = 1; index < parameter_names_.size(); ++index) {
        parameter_group.parameter_ids.append(parameter_names_.at(index));
    }

    TaskDefinition definition;
    definition.task_id = makeTaskId();
    definition.config_file_path = header_file_edit_->text().trimmed();
    definition.raw_file_path = raw_file_edit_->text().trimmed();
    definition.parameter_groups.append(parameter_group);
    definition.user_sample_rate_hz = sample_rate_spin_->value();
    definition.output_directory = QFileInfo(options.output_file_path).absolutePath();
    if (single_time_radio_->isChecked()) {
        definition.time_range.start_seconds = secondsFromTime(start_time_edit_->time());
        definition.time_range.end_seconds = secondsFromTime(end_time_edit_->time());
    }

    options.task_definition = definition;
    return options;
}

void MainWindow::setProcessingState(bool is_processing)
{
    is_processing_ = is_processing;
    start_button_->setEnabled(!is_processing);
    interrupt_button_->setEnabled(is_processing);
    state_label_->setText(is_processing ? QStringLiteral("处理中") : QStringLiteral("就绪"));
    statusBar()->showMessage(is_processing ? QStringLiteral("处理中") : QStringLiteral("就绪"));
}

void MainWindow::appendLog(const QString &message)
{
    const QString timestamp = QDateTime::currentDateTime().toString(QStringLiteral("yyyy-MM-dd hh:mm:ss"));
    log_view_->appendPlainText(QStringLiteral("[%1] %2").arg(timestamp, message));
}

bool MainWindow::loadConfigParameterFile(const QString &file_path, QString *error_message)
{
    auto set_error = [error_message](const QString &message) {
        if (error_message != nullptr) {
            *error_message = message;
        }
        return false;
    };

    config_parameter_names_.clear();
    refreshConfigParameterList();
    updateParameterCountLabels();

    ConfigFileDocument document;
    if (!document.loadFromFile(file_path, error_message)) {
        config_parameter_names_.clear();
        refreshConfigParameterList();
        updateParameterCountLabels();
        return false;
    }

    config_parameter_names_ = document.parameterNames();

    if (config_parameter_names_.isEmpty()) {
        return set_error(QStringLiteral("未能从配置文件中解析到参数。"));
    }

    refreshConfigParameterList();
    updateParameterCountLabels();
    appendLog(QStringLiteral("已读取配置文件参数：%1，参数数：%2")
                  .arg(QFileInfo(file_path).fileName())
                  .arg(config_parameter_names_.size()));
    return true;
}

bool MainWindow::loadParameterGroupFile(const QString &file_path, QString *error_message)
{
    auto set_error = [error_message](const QString &message) {
        if (error_message != nullptr) {
            *error_message = message;
        }
        return false;
    };

    QFile file(file_path);
    if (!file.open(QIODevice::ReadOnly | QIODevice::Text)) {
        return set_error(QStringLiteral("无法打开参数组文件：%1").arg(file.errorString()));
    }

    const QStringList parameter_names = parseParameterNames(textFromBytes(file.readAll()));
    if (parameter_names.isEmpty()) {
        return set_error(QStringLiteral("参数组文件为空，或未解析到参数名。"));
    }

    QStringList normalized_parameter_names = parameter_names;
    normalized_parameter_names.first().remove(QChar(0xfeff));
    if (normalized_parameter_names.first().compare(QStringLiteral("TIME"), Qt::CaseInsensitive) != 0) {
        return set_error(QStringLiteral("参数组文件的第一个参数必须是 TIME。"));
    }

    parameter_group_file_edit_->setText(normalizedPathText(file_path));
    parameter_names_ = normalized_parameter_names;
    refreshProcessingParameterList();
    updateParameterCountLabels();
    appendLog(QStringLiteral("已加载参数组文件：%1，参数数：%2")
                  .arg(QFileInfo(file_path).fileName())
                  .arg(parameter_names_.size()));
    return true;
}

void MainWindow::refreshConfigParameterList()
{
    if (config_parameter_list_widget_ == nullptr) {
        return;
    }

    config_parameter_list_widget_->clear();
    const QString filter_text = config_parameter_search_edit_ == nullptr
        ? QString()
        : config_parameter_search_edit_->text().trimmed();
    for (int index = 0; index < config_parameter_names_.size(); ++index) {
        const QString parameter_name = config_parameter_names_.at(index);
        if (!filter_text.isEmpty() && !parameter_name.contains(filter_text, Qt::CaseInsensitive)) {
            continue;
        }

        QListWidgetItem *item = new QListWidgetItem(
            QStringLiteral("%1. %2").arg(index + 1).arg(parameter_name),
            config_parameter_list_widget_);
        item->setData(Qt::UserRole, parameter_name);
    }

    updateParameterCountLabels();
}

void MainWindow::refreshProcessingParameterList()
{
    if (processing_parameter_list_widget_ == nullptr) {
        return;
    }

    processing_parameter_list_widget_->clear();
    const QString filter_text = processing_parameter_search_edit_ == nullptr
        ? QString()
        : processing_parameter_search_edit_->text().trimmed();
    for (int index = 0; index < parameter_names_.size(); ++index) {
        const QString parameter_name = parameter_names_.at(index);
        if (!filter_text.isEmpty() && !parameter_name.contains(filter_text, Qt::CaseInsensitive)) {
            continue;
        }

        QListWidgetItem *item = new QListWidgetItem(
            QStringLiteral("%1. %2").arg(index + 1).arg(parameter_name),
            processing_parameter_list_widget_);
        item->setData(Qt::UserRole, parameter_name);
    }

    updateParameterCountLabels();
}

void MainWindow::addProcessingParameters(const QStringList &parameter_names)
{
    if (parameter_names.isEmpty()) {
        return;
    }

    if (parameter_names_.isEmpty()) {
        parameter_names_.append(QStringLiteral("TIME"));
    }

    int added_count = 0;
    for (const QString &parameter_name : parameter_names) {
        const QString normalized_name = stripParameterToken(parameter_name);
        if (normalized_name.isEmpty() ||
            normalized_name.compare(QStringLiteral("TIME"), Qt::CaseInsensitive) == 0 ||
            containsParameterName(parameter_names_, normalized_name)) {
            continue;
        }

        parameter_names_.append(normalized_name);
        ++added_count;
    }

    if (added_count > 0) {
        appendLog(QStringLiteral("已加入 %1 个配置参数到处理参数组。").arg(added_count));
    }

    refreshProcessingParameterList();
    updateParameterCountLabels();
}

void MainWindow::updateParameterCountLabels()
{
    if (config_parameter_count_label_ != nullptr) {
        const int visible_count = config_parameter_list_widget_ == nullptr ? 0 : config_parameter_list_widget_->count();
        config_parameter_count_label_->setText(
            QStringLiteral("配置参数：%1 个，显示 %2 个").arg(config_parameter_names_.size()).arg(visible_count));
    }

    if (processing_parameter_count_label_ != nullptr) {
        const int visible_count = processing_parameter_list_widget_ == nullptr
            ? 0
            : processing_parameter_list_widget_->count();
        processing_parameter_count_label_->setText(
            QStringLiteral("处理参数：%1 个，显示 %2 个").arg(parameter_names_.size()).arg(visible_count));
    }
}

void MainWindow::refreshManagedConfigParameterTable()
{
    if (managed_config_table_ == nullptr) {
        return;
    }

    managed_config_table_->setRowCount(0);
    const QString filter_text = managed_config_search_edit_ == nullptr
        ? QString()
        : managed_config_search_edit_->text().trimmed();
    const QList<ConfigParameterRecord> parameters = managed_config_document_.parameters();
    for (int index = 0; index < parameters.size(); ++index) {
        const ConfigParameterRecord &parameter = parameters.at(index);
        if (!filter_text.isEmpty() &&
            !parameter.searchableText().contains(filter_text, Qt::CaseInsensitive)) {
            continue;
        }

        const int row = managed_config_table_->rowCount();
        managed_config_table_->insertRow(row);
        for (int column = 0; column < ManagedConfigColumnCount; ++column) {
            QTableWidgetItem *item = new QTableWidgetItem(managedConfigCellValue(parameter, column));
            item->setData(Qt::UserRole, index);
            if (column == StreamColumn || column == SerialColumn) {
                item->setTextAlignment(Qt::AlignRight | Qt::AlignVCenter);
            }
            managed_config_table_->setItem(row, column, item);
        }
    }

    updateManagedConfigState();
}

void MainWindow::updateManagedConfigState()
{
    const bool has_document = managed_config_document_.hasContent();
    const bool has_selection = selectedManagedConfigParameterIndex() >= 0;

    if (managed_config_add_button_ != nullptr) {
        managed_config_add_button_->setEnabled(has_document);
    }
    if (managed_config_export_button_ != nullptr) {
        managed_config_export_button_->setEnabled(has_document);
    }
    if (managed_config_edit_button_ != nullptr) {
        managed_config_edit_button_->setEnabled(has_selection);
    }
    if (managed_config_delete_button_ != nullptr) {
        managed_config_delete_button_->setEnabled(has_selection);
    }
    if (managed_config_view_button_ != nullptr) {
        managed_config_view_button_->setEnabled(has_selection);
    }

    if (managed_config_status_label_ != nullptr) {
        if (!has_document) {
            managed_config_status_label_->setText(QStringLiteral("未导入配置文件"));
            return;
        }

        const int total_count = managed_config_document_.parameters().size();
        const int visible_count = managed_config_table_ == nullptr ? 0 : managed_config_table_->rowCount();
        managed_config_status_label_->setText(
            QStringLiteral("参数总数：%1，显示：%2，状态：%3")
                .arg(total_count)
                .arg(visible_count)
                .arg(managed_config_document_.isDirty() ? QStringLiteral("有未导出修改")
                                                        : QStringLiteral("已保存")));
    }
}

int MainWindow::selectedManagedConfigParameterIndex() const
{
    if (managed_config_table_ == nullptr) {
        return -1;
    }

    const QList<QTableWidgetItem *> selected_items = managed_config_table_->selectedItems();
    if (selected_items.isEmpty()) {
        return -1;
    }

    const int row = selected_items.first()->row();
    QTableWidgetItem *first_item = managed_config_table_->item(row, 0);
    if (first_item == nullptr) {
        return -1;
    }

    bool ok = false;
    const int parameter_index = first_item->data(Qt::UserRole).toInt(&ok);
    return ok ? parameter_index : -1;
}

bool MainWindow::openManagedConfigParameterDialog(const QString &title,
                                                  ConfigParameterRecord *record,
                                                  bool is_new,
                                                  bool read_only)
{
    if (record == nullptr) {
        return false;
    }

    QDialog dialog(this);
    dialog.setWindowTitle(title);
    dialog.resize(640, 640);

    QVBoxLayout *dialog_layout = new QVBoxLayout(&dialog);
    QScrollArea *scroll_area = new QScrollArea(&dialog);
    scroll_area->setWidgetResizable(true);

    QWidget *form_container = new QWidget(scroll_area);
    QFormLayout *form_layout = new QFormLayout(form_container);
    form_layout->setLabelAlignment(Qt::AlignRight | Qt::AlignVCenter);
    form_layout->setFieldGrowthPolicy(QFormLayout::ExpandingFieldsGrow);
    form_layout->setHorizontalSpacing(12);
    form_layout->setVerticalSpacing(8);

    QComboBox *stream_combo = new QComboBox(form_container);
    for (int stream_index : managed_config_document_.streamIndexes()) {
        stream_combo->addItem(QString::number(stream_index), stream_index);
    }
    const int current_stream_index = stream_combo->findData(record->stream_index);
    if (current_stream_index >= 0) {
        stream_combo->setCurrentIndex(current_stream_index);
    }
    stream_combo->setEnabled(is_new && !read_only);
    form_layout->addRow(QStringLiteral("流号"), stream_combo);

    QList<QPair<QString, QLineEdit *>> field_edits;
    for (const QString &key : ConfigTemplateKeys::parameterKeys()) {
        if (key == QStringLiteral("description")) {
            continue;
        }

        QLineEdit *line_edit = new QLineEdit(form_container);
        line_edit->setText(record->value(key));
        line_edit->setReadOnly(read_only);
        if (!is_new && key == QStringLiteral("serial_no.")) {
            line_edit->setReadOnly(true);
        }
        form_layout->addRow(key, line_edit);
        field_edits.append(qMakePair(key, line_edit));
    }

    QTextEdit *description_edit = new QTextEdit(form_container);
    description_edit->setPlainText(record->value(QStringLiteral("description")));
    description_edit->setReadOnly(read_only);
    description_edit->setMinimumHeight(70);
    form_layout->addRow(QStringLiteral("description"), description_edit);

    scroll_area->setWidget(form_container);
    dialog_layout->addWidget(scroll_area);

    QDialogButtonBox *button_box = read_only
        ? new QDialogButtonBox(QDialogButtonBox::Close, &dialog)
        : new QDialogButtonBox(QDialogButtonBox::Ok | QDialogButtonBox::Cancel, &dialog);
    connect(button_box, &QDialogButtonBox::accepted, &dialog, &QDialog::accept);
    connect(button_box, &QDialogButtonBox::rejected, &dialog, &QDialog::reject);
    dialog_layout->addWidget(button_box);

    if (dialog.exec() != QDialog::Accepted || read_only) {
        return false;
    }

    record->stream_index = stream_combo->currentData().toInt();
    for (const QPair<QString, QLineEdit *> &field_edit : field_edits) {
        record->setValue(field_edit.first, field_edit.second->text());
    }
    record->setValue(QStringLiteral("description"), description_edit->toPlainText());
    return true;
}

ConfigParameterRecord MainWindow::makeDefaultManagedConfigParameter(int stream_index) const
{
    ConfigParameterRecord record;
    record.stream_index = stream_index;
    record.setValue(QStringLiteral("serial_no."),
                    QString::number(managed_config_document_.nextSerialNo(stream_index)));
    record.setValue(QStringLiteral("id_no."), QStringLiteral("S%1P%2")
                                            .arg(stream_index)
                                            .arg(record.serial_no, 3, 10, QLatin1Char('0')));
    record.setValue(QStringLiteral("name"), QStringLiteral("NEW_PARAMETER"));
    record.setValue(QStringLiteral("unit"), QString());
    record.setValue(QStringLiteral("cal_type"), QStringLiteral("NO"));
    record.setValue(QStringLiteral("code_type"), QStringLiteral("uint"));
    record.setValue(QStringLiteral("cal_date"), QString());
    record.setValue(QStringLiteral("cal_number"), QString());
    record.setValue(QStringLiteral("rate"), QStringLiteral("0"));
    record.setValue(QStringLiteral("word_interval"), QStringLiteral("0"));
    record.setValue(QStringLiteral("sign_bit"), QStringLiteral("0"));
    record.setValue(QStringLiteral("inverse"), QStringLiteral("0"));
    record.setValue(QStringLiteral("pcm_words"), QStringLiteral("1"));
    record.setValue(QStringLiteral("frame_no"), QStringLiteral("0,0,0,0,0,0"));
    record.setValue(QStringLiteral("word_no"), QStringLiteral("0,0,0,0,0,0"));
    record.setValue(QStringLiteral("source_bit"), QStringLiteral("0,0,0,0,0,0"));
    record.setValue(QStringLiteral("bit_length"), QStringLiteral("0,0,0,0,0,0"));
    record.setValue(QStringLiteral("object_bit"), QStringLiteral("0,0,0,0,0,0"));
    record.setValue(QStringLiteral("description"), QString());
    return record;
}
