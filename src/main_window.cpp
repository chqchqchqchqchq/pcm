#include "main_window.h"
#include "batch_processing_page.h"
#include "processing_options_support.h"
#include <QCloseEvent>

#include "time_range_parser.h"
#include "traditional_processing_controller.h"

#include <QAbstractItemView>
#include <QAbstractSpinBox>
#include <QApplication>
#include <QCheckBox>
#include <QClipboard>
#include <QComboBox>
#include <QCryptographicHash>
#include <QDateTime>
#include <QDesktopServices>
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
#include <QSettings>
#include <QSignalBlocker>
#include <QTimer>
#include <QtConcurrent>
#include <cmath>
#include <exception>
#include <QSizePolicy>
#include <QSplitter>
#include <QStatusBar>
#include <QStyle>
#include <QTabWidget>
#include <QTableWidget>
#include <QTableWidgetItem>
#include <QTextEdit>
#include <QTime>
#include <QTimeEdit>
#include <QTemporaryFile>
#include <QUrl>
#include <QVBoxLayout>

#include <algorithm>

namespace {

constexpr double kMaxSampleRate = 1000000000.0;
constexpr int kMaxProcessingHistoryEntries = 20;

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
        return record.value(QStringLiteral("Description"));
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
    return static_cast<double>(QTime(0, 0, 0).msecsTo(time)) / 1000.0;
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

/** @brief 参数名按旧规则忽略大小写匹配，供历史恢复和双列表选择共用。 */
bool containsParameterName(const QStringList &names, const QString &name)
{
    return names.contains(name, Qt::CaseInsensitive);
}

QString fileFingerprint(const QString &path)
{
    QFile file(path);
    if (!file.open(QIODevice::ReadOnly)) {
        return QString();
    }

    return QString::fromLatin1(QCryptographicHash::hash(file.readAll(), QCryptographicHash::Sha256).toHex());
}

bool parameterListsEqual(const QStringList &left, const QStringList &right)
{
    if (left.size() != right.size()) {
        return false;
    }

    for (int index = 0; index < left.size(); ++index) {
        if (left.at(index).compare(right.at(index), Qt::CaseInsensitive) != 0) {
            return false;
        }
    }
    return true;
}

QString historyTimeModeText(int time_mode)
{
    switch (time_mode) {
    case 1:
        return QStringLiteral("单时间段");
    case 2:
        return QStringLiteral("多时间段");
    default:
        return QStringLiteral("全程");
    }
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
            &TraditionalProcessingController::rangeFinished,
            this,
            &MainWindow::handleTraditionalRangeFinished);
    connect(traditional_controller_,
            &TraditionalProcessingController::processingFinished,
            this,
            &MainWindow::handleTraditionalProcessingFinished);
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
    const auto refresh_busy = [this]() {
        const bool traditional_busy = is_processing_ || traditional_controller_->isBusy();
        const bool batch_busy = batch_page_->controller()->isRunning();
        batch_page_->setExternalBusy(traditional_busy);
        start_button_->setEnabled(!traditional_busy && !batch_busy);
        if (close_pending_ && !traditional_busy && !batch_busy) {
            QTimer::singleShot(0, this, [this]() { close(); });
        }
    };
    connect(traditional_controller_, &TraditionalProcessingController::busyChanged, this, refresh_busy);
    connect(batch_page_, &BatchProcessingPage::runningChanged, this, refresh_busy);
    header_file_edit_->setObjectName(QStringLiteral("headerFileEdit"));
    raw_file_edit_->setObjectName(QStringLiteral("rawFileEdit"));
    output_dir_edit_->setObjectName(QStringLiteral("outputFileEdit"));
    sample_rate_spin_->setObjectName(QStringLiteral("sampleRateSpin"));
    full_time_radio_->setObjectName(QStringLiteral("fullTimeRadio"));
    single_time_radio_->setObjectName(QStringLiteral("singleTimeRadio"));
    multiple_time_radio_->setObjectName(QStringLiteral("multipleTimeRadio"));
    start_time_edit_->setObjectName(QStringLiteral("startTimeEdit"));
    end_time_edit_->setObjectName(QStringLiteral("endTimeEdit"));
    time_range_file_edit_->setObjectName(QStringLiteral("timeRangeFileEdit"));
    parameter_group_file_edit_->setObjectName(QStringLiteral("parameterGroupFileEdit"));
    output_source_check_->setObjectName(QStringLiteral("includeRawValuesCheck"));
    start_button_->setObjectName(QStringLiteral("startButton"));
    interrupt_button_->setObjectName(QStringLiteral("interruptButton"));
    progress_bar_->setObjectName(QStringLiteral("progressBar"));
    loadSettings();
    setProcessingState(false);
    raw_time_scan_timer_ = new QTimer(this);
    raw_time_scan_timer_->setSingleShot(true);
    raw_time_scan_watcher_ = new QFutureWatcher<RawTimeRangeResult>(this);
    connect(raw_file_edit_, &QLineEdit::textChanged, this, &MainWindow::requestRawTimeScan);
    connect(header_file_edit_, &QLineEdit::textChanged, this, &MainWindow::requestRawTimeScan);
    connect(raw_time_scan_timer_, &QTimer::timeout, this, &MainWindow::startRawTimeScan);
    connect(raw_time_scan_watcher_, &QFutureWatcher<RawTimeRangeResult>::finished,
            this, &MainWindow::finishRawTimeScan);
    connect(start_time_edit_, &QTimeEdit::timeChanged, this, [this]() { ++time_edit_revision_; });
    connect(end_time_edit_, &QTimeEdit::timeChanged, this, [this]() { ++time_edit_revision_; });
    requestRawTimeScan();
}

MainWindow::~MainWindow()
{
    if (raw_time_scan_cancellation_) {
        raw_time_scan_cancellation_->storeRelease(1);
    }
    saveSettings();
}

void MainWindow::loadSettings()
{
    QSettings settings;
    const QByteArray geometry = settings.value(QStringLiteral("window/geometry")).toByteArray();
    if (!geometry.isEmpty()) {
        restoreGeometry(geometry);
    }
    const QByteArray state = settings.value(QStringLiteral("window/state")).toByteArray();
    if (!state.isEmpty()) {
        restoreState(state);
    }

    header_file_edit_->setText(settings.value(QStringLiteral("paths/config")).toString());
    raw_file_edit_->setText(settings.value(QStringLiteral("paths/raw")).toString());
    parameter_group_file_edit_->setText(settings.value(QStringLiteral("paths/parameterGroup")).toString());
    time_range_file_edit_->setText(settings.value(QStringLiteral("paths/timeRanges")).toString());
    output_dir_edit_->setText(settings.value(QStringLiteral("paths/output")).toString());
    sample_rate_spin_->setValue(settings.value(QStringLiteral("processing/sampleRate"), 1024.0).toDouble());
    start_time_edit_->setTime(settings.value(QStringLiteral("processing/startTime"), QTime(0, 0)).toTime());
    end_time_edit_->setTime(settings.value(QStringLiteral("processing/endTime"), QTime(23, 59, 59)).toTime());
    output_source_check_->setChecked(settings.value(QStringLiteral("processing/includeRawValues"), false).toBool());

    const int time_mode = settings.value(QStringLiteral("processing/timeMode"), 0).toInt();
    full_time_radio_->setChecked(time_mode == 0);
    single_time_radio_->setChecked(time_mode == 1);
    multiple_time_radio_->setChecked(time_mode == 2);
    updateTimeMode();

    parameter_names_ = settings.value(QStringLiteral("processing/parameterNames")).toStringList();
    const QString config_path = header_file_edit_->text().trimmed();
    if (!config_path.isEmpty()) {
        if (QFileInfo(config_path).isReadable()) {
            QString error_message;
            if (!loadConfigParameterFile(config_path, &error_message)) {
                appendLog(QStringLiteral("最近配置文件已失效：%1").arg(error_message));
            }
        } else {
            appendLog(QStringLiteral("最近配置文件路径已失效，请重新选择：%1").arg(config_path));
        }
    }

    const QString parameter_group_path = parameter_group_file_edit_->text().trimmed();
    if (!parameter_group_path.isEmpty() && QFileInfo(parameter_group_path).isReadable()) {
        QString error_message;
        if (!loadParameterGroupFile(parameter_group_path, &error_message)) {
            appendLog(QStringLiteral("最近参数组文件已失效：%1").arg(error_message));
        }
    } else {
        refreshProcessingParameterList();
        updateParameterCountLabels();
        if (!parameter_group_path.isEmpty()) {
            appendLog(QStringLiteral("最近参数组文件路径已失效，请重新选择：%1").arg(parameter_group_path));
        }
    }

    const QStringList remembered_files = {raw_file_edit_->text().trimmed(), time_range_file_edit_->text().trimmed()};
    for (const QString &path : remembered_files) {
        if (!path.isEmpty() && !QFileInfo(path).isReadable()) {
            appendLog(QStringLiteral("最近文件路径已失效，请重新选择：%1").arg(path));
        }
    }

    loadProcessingHistory();
    const QByteArray splitter_state = settings.value(QStringLiteral("window/historyLogSplitter")).toByteArray();
    if (history_log_splitter_ != nullptr && !splitter_state.isEmpty()) {
        history_log_splitter_->restoreState(splitter_state);
    }
}

void MainWindow::saveSettings() const
{
    QSettings settings;
    settings.setValue(QStringLiteral("window/geometry"), saveGeometry());
    settings.setValue(QStringLiteral("window/state"), saveState());
    settings.setValue(QStringLiteral("paths/config"), header_file_edit_->text().trimmed());
    settings.setValue(QStringLiteral("paths/raw"), raw_file_edit_->text().trimmed());
    settings.setValue(QStringLiteral("paths/parameterGroup"), parameter_group_file_edit_->text().trimmed());
    settings.setValue(QStringLiteral("paths/timeRanges"), time_range_file_edit_->text().trimmed());
    settings.setValue(QStringLiteral("paths/output"), output_dir_edit_->text().trimmed());
    settings.setValue(QStringLiteral("processing/sampleRate"), sample_rate_spin_->value());
    settings.setValue(QStringLiteral("processing/startTime"), start_time_edit_->time());
    settings.setValue(QStringLiteral("processing/endTime"), end_time_edit_->time());
    settings.setValue(QStringLiteral("processing/includeRawValues"), output_source_check_->isChecked());
    settings.setValue(QStringLiteral("processing/parameterNames"), parameter_names_);
    settings.setValue(QStringLiteral("processing/timeMode"),
                      single_time_radio_->isChecked() ? 1 : (multiple_time_radio_->isChecked() ? 2 : 0));
    if (history_log_splitter_ != nullptr) {
        settings.setValue(QStringLiteral("window/historyLogSplitter"), history_log_splitter_->saveState());
    }
    settings.sync();
    saveProcessingHistory();
}

void MainWindow::browseHeaderFile()
{
    const QString file_path = QFileDialog::getOpenFileName(
        this,
        QStringLiteral("选择头文件/配置文件"),
        QString(),
        QStringLiteral("配置文件 (*.hea *.cfg *.ini *.txt *.json *.xml);;所有文件 (*.*)"));

    if (!file_path.isEmpty()) {
        header_file_edit_->setText(normalizedPathText(file_path));
        QString error_message;
        if (!loadConfigParameterFile(file_path, &error_message)) {
            QMessageBox::warning(this, QStringLiteral("配置参数读取失败"), error_message);
        }
        requestRawTimeScan();
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
        requestRawTimeScan(); // 重新选择同一路径时也刷新文件内容。
    }
}

void MainWindow::requestRawTimeScan()
{
    ++raw_time_scan_generation_;
    requested_time_edit_revision_ = time_edit_revision_;
    raw_time_range_ = RawTimeRangeResult();
    if (raw_time_scan_cancellation_) {
        raw_time_scan_cancellation_->storeRelease(1);
    }
    raw_time_range_label_->setProperty("scanReady", false);
    raw_time_range_label_->setToolTip(QString());
    raw_time_scan_timer_->stop();
    if (raw_file_edit_->text().trimmed().isEmpty()) {
        raw_time_range_label_->setText(QStringLiteral("请选择原始数据文件"));
    } else if (header_file_edit_->text().trimmed().isEmpty()) {
        raw_time_range_label_->setText(QStringLiteral("请先选择配置文件，以识别数据时间"));
    } else {
        raw_time_range_label_->setText(QStringLiteral("正在扫描原始数据时间范围…"));
        raw_time_scan_timer_->start(120);
    }
}

void MainWindow::startRawTimeScan()
{
    raw_time_scan_timer_->stop();
    if (raw_time_scan_running_ || raw_file_edit_->text().trimmed().isEmpty() ||
        header_file_edit_->text().trimmed().isEmpty()) {
        return;
    }
    scanning_raw_path_ = raw_file_edit_->text().trimmed();
    scanning_config_path_ = header_file_edit_->text().trimmed();
    active_time_scan_generation_ = raw_time_scan_generation_;
    raw_time_scan_cancellation_ = RawTimeScanCancellation::create(0);
    const QString raw_path = scanning_raw_path_;
    const QString config_path = scanning_config_path_;
    const RawTimeScanCancellation cancellation = raw_time_scan_cancellation_;
    raw_time_scan_running_ = true;
    raw_time_scan_watcher_->setFuture(QtConcurrent::run([raw_path, config_path, cancellation]() {
        try {
            return RawTimeRangeScanner::scan(config_path, raw_path, cancellation);
        } catch (const std::exception &exception) {
            RawTimeRangeResult result;
            result.error = QStringLiteral("扫描失败：%1").arg(QString::fromUtf8(exception.what()));
            return result;
        } catch (...) {
            RawTimeRangeResult result;
            result.error = QStringLiteral("扫描原始数据时发生异常");
            return result;
        }
    }));
}

void MainWindow::finishRawTimeScan()
{
    raw_time_scan_running_ = false;
    const RawTimeRangeResult result = raw_time_scan_watcher_->result();
    if (active_time_scan_generation_ != raw_time_scan_generation_ ||
        scanning_raw_path_ != raw_file_edit_->text().trimmed() ||
        scanning_config_path_ != header_file_edit_->text().trimmed()) {
        raw_time_scan_timer_->start(0);
        return;
    }
    if (result.cancelled) {
        return;
    }
    if (!result.success) {
        raw_time_range_label_->setText(QStringLiteral("无法识别时间范围：%1").arg(result.error));
        raw_time_range_label_->setToolTip(result.error);
        appendLog(QStringLiteral("原始数据时间扫描失败：%1").arg(result.error));
        return;
    }
    raw_time_range_ = result;
    QString text = QStringLiteral("%1 ～ %2")
        .arg(RawTimeRangeScanner::displayTime(result.start_seconds),
             RawTimeRangeScanner::displayTime(result.end_seconds));
    if (result.skipped_startup) {
        text += QStringLiteral("\n开头 %1 秒为跳变前计时，显示范围从正常时间起点开始")
            .arg(result.skipped_seconds, 0, 'f', 3);
    }
    raw_time_range_label_->setText(text);
    raw_time_range_label_->setProperty("scanReady", true);
    raw_time_range_label_->setToolTip(QStringLiteral("流 %1；结束时间为最后完整长周的结束边界。"
                                                   "\n探测 %2 个长周，读取 %3 KiB。")
        .arg(result.stream_index).arg(result.probed_frames).arg(result.bytes_read / 1024));
    // 扫描期间已经手工编辑的范围优先；跨日范围在 QLabel 中完整显示，不塞入单日控件。
    if (!is_processing_ && requested_time_edit_revision_ == time_edit_revision_ &&
        result.start_seconds >= 0 && std::ceil(result.end_seconds * 1000 - 1e-6) < 86400000) {
        const QSignalBlocker start_blocker(start_time_edit_);
        const QSignalBlocker end_blocker(end_time_edit_);
        start_time_edit_->setTime(QTime::fromMSecsSinceStartOfDay(int(std::floor(result.start_seconds * 1000))));
        end_time_edit_->setTime(QTime::fromMSecsSinceStartOfDay(int(std::ceil(result.end_seconds * 1000 - 1e-6))));
    }
    appendLog(QStringLiteral("原始数据时间范围：%1").arg(text.replace('\n', QStringLiteral("；"))));
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
        QStringLiteral("文本文件 (*.txt)"));

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
        parameter_group_modified_ = true;
        appendLog(QStringLiteral("已从处理参数组删除 %1 个参数。").arg(removed_parameter_names.size()));
    }

    refreshProcessingParameterList();
    updateParameterCountLabels();
}

void MainWindow::startProcessing()
{
    if (traditional_controller_->isBusy() || (batch_page_ && batch_page_->controller()->isRunning())) {
        appendLog(QStringLiteral("已有任务正在处理或清理，请等待结束。"));
        return;
    }
    QString error_message;
    if (!validateTraditionalForm(&error_message)) {
        QMessageBox::warning(this, QStringLiteral("参数不完整"), error_message);
        return;
    }

    const TraditionalProcessingOptions options = buildTraditionalOptions();
    if (!traditional_controller_->validateOptions(options, &error_message)) {
        QMessageBox::warning(this, QStringLiteral("启动前检查失败"), error_message);
        return;
    }

    if (multiple_time_radio_->isChecked()) {
        QList<TaskTimeRange> parsed_ranges;
        QStringList warnings;
        if (TimeRangeParser::parseFile(time_range_file_edit_->text().trimmed(),
                                       &parsed_ranges,
                                       &warnings,
                                       &error_message) &&
            !warnings.isEmpty()) {
            QMessageBox::warning(this, QStringLiteral("时间段重叠提示"), warnings.join(QLatin1Char('\n')));
        }
    }

    QStringList derived_output_paths;
    if (options.time_ranges.size() <= 1) {
        derived_output_paths.append(options.output_file_path);
    } else {
        const QFileInfo output_info(options.output_file_path);
        for (int index = 0; index < options.time_ranges.size(); ++index) {
            derived_output_paths.append(
                QDir(output_info.absolutePath())
                    .filePath(QStringLiteral("%1_range%2.txt")
                                  .arg(output_info.completeBaseName())
                                  .arg(index + 1, 3, 10, QLatin1Char('0'))));
        }
    }

    const QStringList protected_input_paths = {
        options.task_definition.config_file_path,
        options.task_definition.raw_file_path,
        options.parameter_group_file_path,
        multiple_time_radio_->isChecked() ? time_range_file_edit_->text().trimmed() : QString(),
    };
    auto paths_equal = [](const QString &left, const QString &right) {
        if (left.isEmpty() || right.isEmpty()) {
            return false;
        }
#ifdef Q_OS_WIN
        return QFileInfo(left).absoluteFilePath().compare(QFileInfo(right).absoluteFilePath(),
                                                        Qt::CaseInsensitive) == 0;
#else
        return QFileInfo(left).absoluteFilePath() == QFileInfo(right).absoluteFilePath();
#endif
    };
    for (const QString &output_path : derived_output_paths) {
        const QFileInfo output_info(output_path);
        if (output_info.exists() && (!output_info.isFile() || !output_info.isWritable())) {
            QMessageBox::warning(this,
                                 QStringLiteral("启动前检查失败"),
                                 QStringLiteral("派生输出路径不是可覆盖文件：%1").arg(output_path));
            return;
        }
        for (const QString &input_path : protected_input_paths) {
            if (paths_equal(output_path, input_path)) {
                QMessageBox::warning(this,
                                     QStringLiteral("启动前检查失败"),
                                     QStringLiteral("输出路径不能覆盖输入文件：%1").arg(output_path));
                return;
            }
        }
    }
    QStringList existing_output_paths;
    for (const QString &output_path : derived_output_paths) {
        if (QFileInfo::exists(output_path)) {
            existing_output_paths.append(output_path);
        }
    }
    if (!existing_output_paths.isEmpty() &&
        QMessageBox::question(this,
                              QStringLiteral("确认覆盖结果文件"),
                              QStringLiteral("以下结果文件已存在，将统一覆盖：\n%1")
                                  .arg(existing_output_paths.join(QLatin1Char('\n')))) != QMessageBox::Yes) {
        return;
    }

    current_traditional_task_id_ = options.task_definition.task_id;
    completed_output_paths_.clear();
    addProcessingHistory(options, derived_output_paths);
    saveSettings();
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
    state_label_->setText(QStringLiteral("正在中断"));
    statusBar()->showMessage(QStringLiteral("正在中断"));
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

void MainWindow::handleTraditionalRangeFinished(const QString &task_id,
                                                int range_index,
                                                int range_count,
                                                const QString &output_path)
{
    if (!current_traditional_task_id_.isEmpty() && task_id != current_traditional_task_id_) {
        return;
    }

    if (!completed_output_paths_.contains(output_path)) {
        completed_output_paths_.append(output_path);
    }
    appendLog(QStringLiteral("第 %1/%2 段完成，输出：%3")
                  .arg(range_index)
                  .arg(range_count)
                  .arg(output_path));
}

void MainWindow::handleTraditionalProcessingFinished(const QString &task_id, const QStringList &output_paths)
{
    if (!current_traditional_task_id_.isEmpty() && task_id != current_traditional_task_id_) {
        return;
    }

    completed_output_paths_ = output_paths;
    progress_bar_->setValue(100);
    updateProcessingHistoryStatus(task_id, QStringLiteral("完成"), output_paths);
    appendLog(QStringLiteral("传统处理任务完成，共生成 %1 个文件。")
                  .arg(output_paths.size()));
    current_traditional_task_id_.clear();
    setProcessingState(false);

    QMessageBox result_box(this);
    result_box.setIcon(QMessageBox::Information);
    result_box.setWindowTitle(QStringLiteral("处理完成"));
    result_box.setText(QStringLiteral("已生成 %1 个结果文件。").arg(output_paths.size()));
    result_box.setDetailedText(output_paths.join(QLatin1Char('\n')));
    QPushButton *open_button = result_box.addButton(QStringLiteral("打开所在目录"), QMessageBox::ActionRole);
    QPushButton *copy_button = result_box.addButton(QStringLiteral("复制路径"), QMessageBox::ActionRole);
    result_box.addButton(QMessageBox::Ok);
    result_box.exec();
    if (result_box.clickedButton() == open_button && !output_paths.isEmpty()) {
        QDesktopServices::openUrl(QUrl::fromLocalFile(QFileInfo(output_paths.first()).absolutePath()));
    } else if (result_box.clickedButton() == copy_button) {
        QApplication::clipboard()->setText(output_paths.join(QLatin1Char('\n')));
    }
}

void MainWindow::handleTraditionalTaskFailed(const QString &task_id, const QString &reason)
{
    if (!current_traditional_task_id_.isEmpty() && task_id != current_traditional_task_id_) {
        return;
    }

    appendLog(QStringLiteral("传统处理任务失败：%1").arg(reason));
    updateProcessingHistoryStatus(task_id, QStringLiteral("失败"), completed_output_paths_);
    QMessageBox::critical(this, QStringLiteral("处理失败"), reason);
    current_traditional_task_id_.clear();
    setProcessingState(false);
}

void MainWindow::handleTraditionalTaskStopped(const QString &task_id)
{
    if (!current_traditional_task_id_.isEmpty() && task_id != current_traditional_task_id_) {
        return;
    }

    appendLog(QStringLiteral("传统处理任务已中断：%1").arg(task_id));
    if (!completed_output_paths_.isEmpty()) {
        appendLog(QStringLiteral("中断前已完成文件：\n%1")
                      .arg(completed_output_paths_.join(QLatin1Char('\n'))));
    }
    updateProcessingHistoryStatus(task_id, QStringLiteral("中断"), completed_output_paths_);
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
        QStringLiteral("配置文件 (*.hea *.cfg *.ini *.txt);;所有文件 (*.*)"));
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
        QStringLiteral("配置文件 (*.hea *.cfg *.ini *.txt);;所有文件 (*.*)"));
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

    QWidget *form_widget = new QWidget(page);
    QGridLayout *form_grid = new QGridLayout(form_widget);
    form_grid->setContentsMargins(0, 0, 0, 0);
    form_grid->setSizeConstraint(QLayout::SetMinimumSize);
    form_grid->setColumnStretch(0, 1);
    form_grid->setColumnStretch(1, 1);
    form_grid->setHorizontalSpacing(14);
    form_grid->setVerticalSpacing(14);
    form_grid->addWidget(createInputGroup(), 0, 0, 1, 2);
    form_grid->addWidget(createTimeGroup(), 1, 0);
    form_grid->addWidget(createOutputGroup(), 1, 1);

    // 表单内容超过窗口高度时允许滚动，避免新增时间提示和参数列表被强行压扁。
    QScrollArea *form_scroll = new QScrollArea(page);
    form_scroll->setObjectName(QStringLiteral("traditionalFormScroll"));
    form_scroll->setFrameShape(QFrame::NoFrame);
    form_scroll->setWidgetResizable(true);
    form_scroll->setWidget(form_widget);
    page_layout->addWidget(form_scroll, 3);
    page_layout->addWidget(createActionArea(), 2);
    return page;
}

QWidget *MainWindow::createMultiTaskPage()
{
    batch_page_ = new BatchProcessingPage(this);
    return batch_page_;
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
    raw_time_range_label_ = new QLabel(QStringLiteral("请选择原始数据文件"), group_box);
    raw_time_range_label_->setObjectName(QStringLiteral("rawTimeRangeLabel"));
    raw_time_range_label_->setWordWrap(true);
    raw_time_range_label_->setSizePolicy(QSizePolicy::Expanding, QSizePolicy::Minimum);
    raw_time_range_label_->setMinimumHeight(raw_time_range_label_->fontMetrics().lineSpacing() * 2 + 4);
    raw_time_range_label_->setTextInteractionFlags(Qt::TextSelectableByMouse);
    form_layout->addRow(QStringLiteral("数据时间范围"), raw_time_range_label_);
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
    config_parameter_list_widget_->setObjectName(QStringLiteral("configParameterList"));
    config_parameter_list_widget_->setAlternatingRowColors(true);
    config_parameter_list_widget_->setSelectionMode(QAbstractItemView::ExtendedSelection);
    config_parameter_list_widget_->setMinimumHeight(180);

    processing_parameter_list_widget_ = new QListWidget(container);
    processing_parameter_list_widget_->setObjectName(QStringLiteral("processingParameterList"));
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
    container->setMinimumHeight(grid_layout->minimumSize().height());
    container->setSizePolicy(QSizePolicy::Expanding, QSizePolicy::MinimumExpanding);
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
    sample_rate_spin_->setDecimals(0);
    sample_rate_spin_->setRange(1.0, kMaxSampleRate);
    sample_rate_spin_->setValue(1.0);
    sample_rate_spin_->setSuffix(QStringLiteral(" Hz"));
    sample_rate_spin_->setButtonSymbols(QAbstractSpinBox::NoButtons);
    sample_rate_spin_->setToolTip(QStringLiteral("按旧版规则控制输出抽取步长；时间轴来自原始数据中的时间码。"));
    sample_layout->addRow(QStringLiteral("处理采样率"), sample_rate_spin_);
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
    start_time_edit_->setDisplayFormat(QStringLiteral("HH:mm:ss.zzz"));
    start_time_edit_->setButtonSymbols(QAbstractSpinBox::NoButtons);

    end_time_edit_ = new QTimeEdit(QTime(23, 59, 59), group_box);
    end_time_edit_->setDisplayFormat(QStringLiteral("HH:mm:ss.zzz"));
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

    output_source_check_ = new QCheckBox(QStringLiteral("输出原始码值"), group_box);

    output_dir_edit_ = new QLineEdit(group_box);
    output_dir_edit_->setReadOnly(true);
    QPushButton *output_browse_button = new QPushButton(group_box);
    output_browse_button->setIcon(style()->standardIcon(QStyle::SP_DialogOpenButton));
    output_browse_button->setToolTip(QStringLiteral("指定输出结果文件名"));
    connect(output_browse_button, &QPushButton::clicked, this, &MainWindow::browseOutputDirectory);

    form_layout->addRow(QStringLiteral("数值选项"), output_source_check_);
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

    history_log_splitter_ = new QSplitter(Qt::Horizontal, frame);
    history_log_splitter_->setObjectName(QStringLiteral("historyLogSplitter"));
    history_log_splitter_->setChildrenCollapsible(false);
    history_log_splitter_->addWidget(createProcessingHistoryArea(history_log_splitter_));

    QFrame *log_container = new QFrame(history_log_splitter_);
    QVBoxLayout *log_layout = new QVBoxLayout(log_container);
    log_layout->setContentsMargins(0, 0, 0, 0);
    log_layout->setSpacing(6);
    QLabel *log_title = new QLabel(QStringLiteral("处理日志"), log_container);
    QFont log_title_font = log_title->font();
    log_title_font.setBold(true);
    log_title->setFont(log_title_font);
    log_view_ = new QPlainTextEdit(log_container);
    log_view_->setObjectName(QStringLiteral("logView"));
    log_view_->setReadOnly(true);
    log_view_->setMinimumHeight(130);
    log_view_->setPlaceholderText(QStringLiteral("处理日志"));
    log_layout->addWidget(log_title);
    log_layout->addWidget(log_view_, 1);
    history_log_splitter_->addWidget(log_container);
    history_log_splitter_->setStretchFactor(0, 45);
    history_log_splitter_->setStretchFactor(1, 55);
    history_log_splitter_->setSizes({450, 550});
    outer_layout->addWidget(history_log_splitter_, 1);
    return frame;
}

QWidget *MainWindow::createProcessingHistoryArea(QWidget *parent)
{
    QFrame *container = new QFrame(parent);
    QVBoxLayout *layout = new QVBoxLayout(container);
    layout->setContentsMargins(0, 0, 0, 0);
    layout->setSpacing(6);

    QLabel *title = new QLabel(QStringLiteral("最近处理记录"), container);
    QFont title_font = title->font();
    title_font.setBold(true);
    title->setFont(title_font);

    processing_history_list_ = new QListWidget(container);
    processing_history_list_->setObjectName(QStringLiteral("processingHistoryList"));
    processing_history_list_->setSelectionMode(QAbstractItemView::SingleSelection);
    processing_history_list_->setEditTriggers(QAbstractItemView::NoEditTriggers);
    processing_history_list_->setHorizontalScrollBarPolicy(Qt::ScrollBarAlwaysOff);
    processing_history_list_->setWordWrap(true);
    processing_history_list_->setMinimumHeight(130);
    processing_history_list_->setToolTip(QStringLiteral("显示最近 20 条处理记录"));

    history_apply_button_ = new QPushButton(QStringLiteral("使用选中记录"), container);
    history_apply_button_->setObjectName(QStringLiteral("historyApplyButton"));
    history_delete_button_ = new QPushButton(QStringLiteral("删除"), container);
    history_delete_button_->setObjectName(QStringLiteral("historyDeleteButton"));
    history_clear_button_ = new QPushButton(QStringLiteral("清空"), container);
    history_clear_button_->setObjectName(QStringLiteral("historyClearButton"));

    QHBoxLayout *button_layout = new QHBoxLayout;
    button_layout->setContentsMargins(0, 0, 0, 0);
    button_layout->addWidget(history_apply_button_);
    button_layout->addStretch(1);
    button_layout->addWidget(history_delete_button_);
    button_layout->addWidget(history_clear_button_);

    layout->addWidget(title);
    layout->addWidget(processing_history_list_, 1);
    layout->addLayout(button_layout);

    connect(processing_history_list_,
            &QListWidget::itemSelectionChanged,
            this,
            &MainWindow::updateProcessingHistoryActions);
    connect(processing_history_list_, &QListWidget::itemActivated, this, [this](QListWidgetItem *) {
        applySelectedProcessingHistory();
    });
    connect(history_apply_button_, &QPushButton::clicked, this, &MainWindow::applySelectedProcessingHistory);
    connect(history_delete_button_, &QPushButton::clicked, this, &MainWindow::deleteSelectedProcessingHistory);
    connect(history_clear_button_, &QPushButton::clicked, this, &MainWindow::clearProcessingHistory);
    updateProcessingHistoryActions();
    return container;
}

void MainWindow::loadProcessingHistory()
{
    processing_history_ = ProcessingHistoryStore::load();
    refreshProcessingHistoryList();
}

void MainWindow::saveProcessingHistory() const
{
    if (!ProcessingHistoryStore::save(processing_history_)) {
        Logger::instance().log(LogLevel::Error, QStringLiteral("MainWindow"), QString(), QString(),
                               QStringLiteral("保存最近处理记录失败"));
    }
}
void MainWindow::addProcessingHistory(const TraditionalProcessingOptions &options,
                                      const QStringList &derived_output_paths)
{
    for (int index = processing_history_.size() - 1; index >= 0; --index) {
        if (processing_history_.at(index).task_id == options.task_definition.task_id) {
            processing_history_.removeAt(index);
        }
    }

    ProcessingHistoryEntry entry;
    entry.task_id = options.task_definition.task_id;
    entry.started_at = QDateTime::currentDateTime();
    entry.status = QStringLiteral("处理中");
    entry.config_path = options.task_definition.config_file_path;
    entry.config_fingerprint = fileFingerprint(entry.config_path);
    entry.parameter_group_from_file = currentParametersMatchSourceGroup();
    if (entry.parameter_group_from_file) {
        entry.parameter_group_path = parameter_group_source_path_;
        entry.parameter_group_fingerprint = parameter_group_source_fingerprint_;
    }
    entry.parameter_names = parameter_names_;
    entry.sample_rate_hz = sample_rate_spin_->value();
    entry.time_mode = single_time_radio_->isChecked() ? 1 : (multiple_time_radio_->isChecked() ? 2 : 0);
    entry.start_time = start_time_edit_->time();
    entry.end_time = end_time_edit_->time();
    entry.raw_file_path = options.task_definition.raw_file_path;
    entry.output_paths = derived_output_paths;
    entry.time_range_file_path = time_range_file_edit_->text().trimmed();
    entry.include_raw_values = options.include_raw_values;
    entry.time_ranges = options.time_ranges;
    entry.has_time_ranges_snapshot = true;

    processing_history_.prepend(entry);
    while (processing_history_.size() > kMaxProcessingHistoryEntries) {
        processing_history_.removeLast();
    }
    refreshProcessingHistoryList();
    saveProcessingHistory();
}

void MainWindow::updateProcessingHistoryStatus(const QString &task_id,
                                               const QString &status,
                                               const QStringList &output_paths)
{
    for (ProcessingHistoryEntry &entry : processing_history_) {
        if (entry.task_id != task_id) {
            continue;
        }
        entry.status = status;
        entry.output_paths = output_paths;
        refreshProcessingHistoryList();
        saveProcessingHistory();
        return;
    }
}

void MainWindow::refreshProcessingHistoryList()
{
    if (processing_history_list_ == nullptr) {
        return;
    }

    const QString selected_task_id = processing_history_list_->currentItem() == nullptr
        ? QString()
        : processing_history_list_->currentItem()->data(Qt::UserRole).toString();
    processing_history_list_->clear();
    for (const ProcessingHistoryEntry &entry : processing_history_) {
        const QFileInfo config_info(entry.config_path);
        const QString config_name = config_info.fileName().isEmpty() ? entry.config_path : config_info.fileName();
        QString parameter_group_name = QStringLiteral("临时参数组");
        if (entry.parameter_group_from_file) {
            const QString file_name = QFileInfo(entry.parameter_group_path).fileName();
            if (!file_name.isEmpty()) {
                parameter_group_name = file_name;
            }
        }

        QString display_status = entry.status;
        if (!config_info.isReadable()) {
            display_status += QStringLiteral(" · 配置失效");
        }
        QListWidgetItem *item = new QListWidgetItem(
            QStringLiteral("%1 | %2\n%3 | %4 | %5")
                .arg(entry.started_at.toString(QStringLiteral("yyyy-MM-dd HH:mm")),
                     config_name,
                     parameter_group_name,
                     historyTimeModeText(entry.time_mode),
                     display_status),
            processing_history_list_);
        item->setData(Qt::UserRole, entry.task_id);
        item->setToolTip(QStringLiteral("配置文件：%1\n参数组：%2\n原始数据：%3\n结果文件：%4")
                             .arg(entry.config_path,
                                  entry.parameter_group_from_file
                                      ? entry.parameter_group_path
                                      : QStringLiteral("临时参数组"),
                                  entry.raw_file_path,
                                  entry.output_paths.join(QLatin1Char('\n'))));
        if (!config_info.isReadable()) {
            item->setForeground(palette().brush(QPalette::Disabled, QPalette::Text));
        }
        if (entry.task_id == selected_task_id) {
            processing_history_list_->setCurrentItem(item);
        }
    }
    updateProcessingHistoryActions();
}

int MainWindow::selectedProcessingHistoryIndex() const
{
    if (processing_history_list_ == nullptr || processing_history_list_->currentItem() == nullptr) {
        return -1;
    }
    const QString task_id = processing_history_list_->currentItem()->data(Qt::UserRole).toString();
    for (int index = 0; index < processing_history_.size(); ++index) {
        if (processing_history_.at(index).task_id == task_id) {
            return index;
        }
    }
    return -1;
}

bool MainWindow::currentParametersMatchSourceGroup() const
{
    return !parameter_group_modified_ && !parameter_group_source_path_.isEmpty() &&
        parameterListsEqual(parameter_names_, parameter_group_source_parameters_);
}

void MainWindow::updateProcessingHistoryActions()
{
    if (history_apply_button_ == nullptr) {
        return;
    }
    const int index = selectedProcessingHistoryIndex();
    const bool has_selection = index >= 0;
    const bool config_available = has_selection && QFileInfo(processing_history_.at(index).config_path).isReadable();
    history_apply_button_->setEnabled(!is_processing_ && config_available);
    history_delete_button_->setEnabled(!is_processing_ && has_selection);
    history_clear_button_->setEnabled(!is_processing_ && !processing_history_.isEmpty());
    if (has_selection && !config_available) {
        history_apply_button_->setToolTip(
            QStringLiteral("配置文件不存在或不可读：%1").arg(processing_history_.at(index).config_path));
    } else {
        history_apply_button_->setToolTip(QString());
    }
}

void MainWindow::applySelectedProcessingHistory()
{
    if (is_processing_) {
        return;
    }
    const int index = selectedProcessingHistoryIndex();
    if (index < 0) {
        return;
    }
    const ProcessingHistoryEntry entry = processing_history_.at(index);
    if (!QFileInfo(entry.config_path).isReadable()) {
        QMessageBox::warning(this,
                             QStringLiteral("历史记录不可用"),
                             QStringLiteral("配置文件不存在或不可读：\n%1").arg(entry.config_path));
        updateProcessingHistoryActions();
        return;
    }

    QString error_message;
    if (!loadConfigParameterFile(entry.config_path, &error_message)) {
        QMessageBox::warning(this, QStringLiteral("历史记录恢复失败"), error_message);
        return;
    }
    header_file_edit_->setText(normalizedPathText(entry.config_path));

    QStringList restored_parameters = {QStringLiteral("TIME")};
    QStringList missing_parameters;
    for (const QString &history_name : entry.parameter_names) {
        if (history_name.compare(QStringLiteral("TIME"), Qt::CaseInsensitive) == 0) {
            continue;
        }
        QString matched_name;
        for (const QString &config_name : config_parameter_names_) {
            if (config_name.compare(history_name, Qt::CaseInsensitive) == 0) {
                matched_name = config_name;
                break;
            }
        }
        if (matched_name.isEmpty()) {
            missing_parameters.append(history_name);
        } else if (!containsParameterName(restored_parameters, matched_name)) {
            restored_parameters.append(matched_name);
        }
    }
    parameter_names_ = restored_parameters;

    QStringList warnings;
    if (!entry.config_fingerprint.isEmpty() && fileFingerprint(entry.config_path) != entry.config_fingerprint) {
        warnings.append(QStringLiteral("配置文件内容已变化，参数已按当前配置重新匹配。"));
    }
    if (!missing_parameters.isEmpty()) {
        warnings.append(QStringLiteral("当前配置中未找到以下历史参数：%1")
                            .arg(missing_parameters.join(QStringLiteral("、"))));
    }

    if (entry.parameter_group_from_file) {
        parameter_group_source_path_ = entry.parameter_group_path;
        parameter_group_source_fingerprint_ = entry.parameter_group_fingerprint;
        parameter_group_source_parameters_ = entry.parameter_names;
        parameter_group_modified_ = !missing_parameters.isEmpty();
        if (QFileInfo(entry.parameter_group_path).isReadable()) {
            parameter_group_file_edit_->setText(normalizedPathText(entry.parameter_group_path));
            if (!entry.parameter_group_fingerprint.isEmpty() &&
                fileFingerprint(entry.parameter_group_path) != entry.parameter_group_fingerprint) {
                warnings.append(QStringLiteral("参数组文件已变化，本次使用历史参数快照。"));
            }
        } else {
            parameter_group_file_edit_->clear();
            warnings.append(QStringLiteral("原参数组文件不可用，本次使用历史参数快照。"));
        }
    } else {
        parameter_group_file_edit_->clear();
        parameter_group_source_path_.clear();
        parameter_group_source_fingerprint_.clear();
        parameter_group_source_parameters_.clear();
        parameter_group_modified_ = true;
    }

    sample_rate_spin_->setValue(entry.sample_rate_hz);
    if (entry.time_mode == 1) {
        single_time_radio_->setChecked(true);
        start_time_edit_->setTime(entry.start_time);
        end_time_edit_->setTime(entry.end_time);
    } else if (entry.time_mode == 2) {
        multiple_time_radio_->setChecked(true);
        time_range_file_edit_->clear();
        warnings.append(QStringLiteral("多时间段记录已恢复，请重新选择时间段 TXT 文件。"));
    } else {
        full_time_radio_->setChecked(true);
    }
    updateTimeMode();
    refreshProcessingParameterList();
    updateParameterCountLabels();
    saveSettings();
    appendLog(QStringLiteral("已应用历史处理记录：%1").arg(entry.task_id));
    if (!warnings.isEmpty()) {
        QMessageBox::warning(this, QStringLiteral("历史记录恢复提示"), warnings.join(QLatin1Char('\n')));
    }
}

void MainWindow::deleteSelectedProcessingHistory()
{
    if (is_processing_) {
        return;
    }
    const int index = selectedProcessingHistoryIndex();
    if (index < 0) {
        return;
    }
    if (QMessageBox::question(this,
                              QStringLiteral("删除处理记录"),
                              QStringLiteral("确定删除选中的处理记录吗？")) != QMessageBox::Yes) {
        return;
    }
    processing_history_.removeAt(index);
    refreshProcessingHistoryList();
    saveProcessingHistory();
}

void MainWindow::clearProcessingHistory()
{
    if (is_processing_ || processing_history_.isEmpty()) {
        return;
    }
    if (QMessageBox::question(this,
                              QStringLiteral("清空处理记录"),
                              QStringLiteral("确定清空最近 20 条处理记录吗？")) != QMessageBox::Yes) {
        return;
    }
    processing_history_.clear();
    refreshProcessingHistoryList();
    saveProcessingHistory();
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
    const QFileInfo config_info(header_file_edit_->text().trimmed());
    if (!config_info.exists() || !config_info.isFile() || !config_info.isReadable()) {
        return set_error(QStringLiteral("头文件/配置文件不存在或不可读。"));
    }

    if (raw_file_edit_->text().trimmed().isEmpty()) {
        return set_error(QStringLiteral("请选择原始文件。"));
    }
    const QFileInfo raw_info(raw_file_edit_->text().trimmed());
    if (!raw_info.exists() || !raw_info.isFile() || !raw_info.isReadable()) {
        return set_error(QStringLiteral("原始数据文件不存在或不可读。"));
    }

    const QString parameter_group_path = parameter_group_file_edit_->text().trimmed();
    if (!parameter_group_path.isEmpty()) {
        const QFileInfo parameter_group_info(parameter_group_path);
        if (!parameter_group_info.exists() || !parameter_group_info.isFile() ||
            !parameter_group_info.isReadable()) {
            return set_error(QStringLiteral("参数组文件不存在或不可读。"));
        }
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
    if (multiple_time_radio_->isChecked()) {
        QList<TaskTimeRange> ranges;
        if (!TimeRangeParser::parseFile(time_range_file_edit_->text().trimmed(),
                                        &ranges,
                                        nullptr,
                                        error_message)) {
            return false;
        }
    }

    if (output_dir_edit_->text().trimmed().isEmpty()) {
        return set_error(QStringLiteral("请指定输出结果文件。"));
    }
    const QFileInfo output_info(output_dir_edit_->text().trimmed());
    if (output_info.suffix().compare(QStringLiteral("txt"), Qt::CaseInsensitive) != 0) {
        return set_error(QStringLiteral("结果文件必须使用 .txt 扩展名。"));
    }
    const QDir output_directory(output_info.absolutePath());
    if (!output_directory.exists()) {
        return set_error(QStringLiteral("输出目录不存在：%1").arg(output_directory.absolutePath()));
    }
    QTemporaryFile permission_probe(output_directory.filePath(QStringLiteral(".pcm_write_check_XXXXXX")));
    permission_probe.setAutoRemove(true);
    if (!permission_probe.open()) {
        return set_error(QStringLiteral("输出目录不可写：%1").arg(output_directory.absolutePath()));
    }
    permission_probe.close();

    return true;
}

TraditionalProcessingOptions MainWindow::buildTraditionalOptions() const
{
    TraditionalProcessingOptions options;
    options.parameter_group_file_path = parameter_group_file_edit_->text().trimmed();
    options.parameter_names = parameter_names_;
    options.output_file_path = output_dir_edit_->text().trimmed();
    options.include_raw_values = output_source_check_->isChecked();
    if (single_time_radio_->isChecked()) {
        TaskTimeRange range;
        range.start_seconds = secondsFromTime(start_time_edit_->time());
        range.end_seconds = secondsFromTime(end_time_edit_->time());
        options.time_ranges.append(range);
    } else if (multiple_time_radio_->isChecked()) {
        TimeRangeParser::parseFile(time_range_file_edit_->text().trimmed(), &options.time_ranges);
    }

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
    if (!options.time_ranges.isEmpty()) {
        definition.time_range = options.time_ranges.first();
    }

    options.task_definition = definition;
    ProcessingOptionsSupport::normalize(&options);
    return options;
}

void MainWindow::setProcessingState(bool is_processing)
{
    is_processing_ = is_processing;
    const bool backend_busy = traditional_controller_->isBusy();
    start_button_->setEnabled(!is_processing && !backend_busy &&
                             (!batch_page_ || !batch_page_->controller()->isRunning()));
    if (batch_page_) { batch_page_->setExternalBusy(is_processing || backend_busy); }
    interrupt_button_->setEnabled(is_processing);
    const QList<QGroupBox *> input_groups = mode_tabs_->widget(0)->findChildren<QGroupBox *>();
    for (QGroupBox *group : input_groups) {
        group->setEnabled(!is_processing);
    }
    updateTimeMode();
    state_label_->setText(is_processing ? QStringLiteral("处理中") : QStringLiteral("就绪"));
    statusBar()->showMessage(is_processing ? QStringLiteral("处理中") : QStringLiteral("就绪"));
    updateProcessingHistoryActions();
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
    QStringList normalized_parameter_names;
    if (!ProcessingOptionsSupport::readParameterGroup(file_path, &normalized_parameter_names, error_message)) {
        return false;
    }
    parameter_group_file_edit_->setText(normalizedPathText(file_path));
    parameter_names_ = normalized_parameter_names;
    parameter_group_source_path_ = normalizedPathText(file_path);
    parameter_group_source_fingerprint_ = fileFingerprint(file_path);
    parameter_group_source_parameters_ = normalized_parameter_names;
    parameter_group_modified_ = false;
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
        parameter_group_modified_ = true;
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
    dialog.resize(700, 700);

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
    QComboBox *cal_type_combo = nullptr;
    QComboBox *code_type_combo = nullptr;
    QTableWidget *code_phy_table = nullptr;

    auto set_combo_value = [](QComboBox *combo, const QString &value) {
        const int index = combo->findText(value.trimmed(), Qt::MatchFixedString | Qt::MatchCaseSensitive);
        if (index >= 0) {
            combo->setCurrentIndex(index);
        }
    };

    auto add_code_phy_row = [&code_phy_table](const ConfigCodePhyPoint &point) {
        const int row = code_phy_table->rowCount();
        code_phy_table->insertRow(row);
        code_phy_table->setItem(row, 0, new QTableWidgetItem(point.code));
        code_phy_table->setItem(row, 1, new QTableWidgetItem(point.phy));
    };

    auto add_code_phy_editor = [&]() {
        QWidget *container = new QWidget(form_container);
        QVBoxLayout *container_layout = new QVBoxLayout(container);
        container_layout->setContentsMargins(0, 0, 0, 0);
        container_layout->setSpacing(6);

        code_phy_table = new QTableWidget(container);
        code_phy_table->setColumnCount(2);
        code_phy_table->setHorizontalHeaderLabels({QStringLiteral("code"), QStringLiteral("phy")});
        code_phy_table->horizontalHeader()->setStretchLastSection(true);
        code_phy_table->horizontalHeader()->setSectionResizeMode(QHeaderView::Stretch);
        code_phy_table->verticalHeader()->setVisible(false);
        code_phy_table->setSelectionBehavior(QAbstractItemView::SelectRows);
        code_phy_table->setSelectionMode(QAbstractItemView::SingleSelection);
        code_phy_table->setEditTriggers(read_only
            ? QAbstractItemView::NoEditTriggers
            : (QAbstractItemView::DoubleClicked | QAbstractItemView::EditKeyPressed |
               QAbstractItemView::AnyKeyPressed));
        code_phy_table->setMinimumHeight(120);
        for (const ConfigCodePhyPoint &point : record->code_phy_points) {
            add_code_phy_row(point);
        }
        container_layout->addWidget(code_phy_table);

        QHBoxLayout *button_layout = new QHBoxLayout;
        QPushButton *add_row_button = new QPushButton(QStringLiteral("添加 code/phy"), container);
        QPushButton *remove_row_button = new QPushButton(QStringLiteral("删除选中"), container);
        add_row_button->setEnabled(!read_only);
        remove_row_button->setEnabled(!read_only);
        add_row_button->setVisible(!read_only);
        remove_row_button->setVisible(!read_only);
        connect(add_row_button, &QPushButton::clicked, &dialog, [&]() {
            ConfigCodePhyPoint point;
            add_code_phy_row(point);
        });
        connect(remove_row_button, &QPushButton::clicked, &dialog, [&]() {
            const int row = code_phy_table->currentRow();
            if (row >= 0) {
                code_phy_table->removeRow(row);
            }
        });
        button_layout->addWidget(add_row_button);
        button_layout->addWidget(remove_row_button);
        button_layout->addStretch(1);
        container_layout->addLayout(button_layout);
        form_layout->addRow(QStringLiteral("code,phy"), container);
    };

    for (const QString &key : ConfigTemplateKeys::parameterKeys() + ConfigTemplateKeys::calibrationKeys()) {
        if (key == QStringLiteral("Description") || key == QStringLiteral("segment_number") ||
            key == QStringLiteral("code,phy")) {
            continue;
        }

        if (key == QStringLiteral("cal_type")) {
            cal_type_combo = new QComboBox(form_container);
            cal_type_combo->addItems({QStringLiteral("NO"), QStringLiteral("line"), QStringLiteral("poly"),
                                      QStringLiteral("segm")});
            set_combo_value(cal_type_combo, record->value(key));
            cal_type_combo->setEnabled(!read_only);
            form_layout->addRow(key, cal_type_combo);
        } else if (key == QStringLiteral("code_type")) {
            code_type_combo = new QComboBox(form_container);
            code_type_combo->addItems({QStringLiteral("bcd"), QStringLiteral("uint"), QStringLiteral("sint"),
                                       QStringLiteral("fcsf")});
            set_combo_value(code_type_combo, record->value(key));
            code_type_combo->setEnabled(!read_only);
            form_layout->addRow(key, code_type_combo);
        } else {
            QLineEdit *line_edit = new QLineEdit(form_container);
            line_edit->setText(record->value(key));
            line_edit->setReadOnly(read_only);
            if (!is_new && key == QStringLiteral("serial_no.")) {
                line_edit->setReadOnly(true);
            }
            form_layout->addRow(key, line_edit);
            field_edits.append(qMakePair(key, line_edit));
        }

        if (key == QStringLiteral("object_bit")) {
            add_code_phy_editor();
        }
    }

    QTextEdit *description_edit = new QTextEdit(form_container);
    description_edit->setPlainText(record->value(QStringLiteral("Description")));
    description_edit->setReadOnly(read_only);
    description_edit->setMinimumHeight(70);
    form_layout->addRow(QStringLiteral("Description"), description_edit);

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
    if (cal_type_combo != nullptr) {
        record->setValue(QStringLiteral("cal_type"), cal_type_combo->currentText());
    }
    if (code_type_combo != nullptr) {
        record->setValue(QStringLiteral("code_type"), code_type_combo->currentText());
    }

    record->code_phy_points.clear();
    if (code_phy_table != nullptr) {
        for (int row = 0; row < code_phy_table->rowCount(); ++row) {
            const QTableWidgetItem *code_item = code_phy_table->item(row, 0);
            const QTableWidgetItem *phy_item = code_phy_table->item(row, 1);
            ConfigCodePhyPoint point;
            point.code = code_item == nullptr ? QString() : code_item->text().trimmed();
            point.phy = phy_item == nullptr ? QString() : phy_item->text().trimmed();
            if (point.code.isEmpty() && point.phy.isEmpty()) {
                continue;
            }
            record->code_phy_points.append(point);
        }
    }
    record->setValue(QStringLiteral("Description"), description_edit->toPlainText());
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
    record.setValue(QStringLiteral("Description"), QString());
    return record;
}

void MainWindow::closeEvent(QCloseEvent *event)
{
    if (traditional_controller_->isBusy() || batch_page_->controller()->isRunning()) {
        if (!close_pending_) {
            QMessageBox box(QMessageBox::Question, QStringLiteral("任务正在运行"),
                QStringLiteral("中断当前处理并关闭窗口？已生成的结果会保留。"), QMessageBox::NoButton, this);
            auto *stop = box.addButton(QStringLiteral("中断并关闭"), QMessageBox::AcceptRole);
            box.addButton(QStringLiteral("继续处理"), QMessageBox::RejectRole);
            box.exec();
            if (box.clickedButton() != stop) { event->ignore(); return; }
        }
        close_pending_ = true;
        traditional_controller_->requestStop();
        batch_page_->controller()->requestStop();
        event->ignore();
        return;
    }
    close_pending_ = false;
    if (!batch_page_->confirmSaveChanges()) { event->ignore(); return; }
    saveSettings();
    event->accept();
}

