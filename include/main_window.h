#pragma once

#include "config_file_document.h"
#include "processing_history_store.h"
#include "config_types.h"
#include "logger.h"
#include "raw_time_range_scanner.h"
#include <QFutureWatcher>
#include "traditional_processing_types.h"

#include <QDateTime>
#include <QList>
#include <QMainWindow>
#include <QStringList>
#include <QTime>

class QCheckBox;
class QDoubleSpinBox;
class QGroupBox;
class QLabel;
class QLineEdit;
class QListWidget;
class QPlainTextEdit;
class QProgressBar;
class QPushButton;
class QRadioButton;
class QSplitter;
class QTabWidget;
class QTableWidget;
class QTimeEdit;
class QTimer;
class QWidget;
class TraditionalProcessingController;
class BatchProcessingPage;
class QCloseEvent;

/**
 * @brief Main Qt Widgets shell for PCM post-processing workflows.
 *
 * The window exposes the traditional single-task form now and reserves tab entries for the later multi-task scheduler
 * and configuration-file management screens. Form submission is emitted as a structured signal so the processing
 * pipeline can be connected without changing the UI contract.
 */
class MainWindow : public QMainWindow
{
    Q_OBJECT

public:
    /**
     * @brief Creates the main application window and builds all page widgets.
     */
    explicit MainWindow(QWidget *parent = nullptr);

    /**
     * @brief Persists user-facing window and processing settings.
     */
    ~MainWindow() override;

signals:
    /**
     * @brief Emitted after the traditional processing form passes validation.
     */
    void traditionalProcessingRequested(const TraditionalProcessingOptions &options);

    /**
     * @brief Emitted when the user requests cooperative interruption of the current processing task.
     */
    void processingInterrupted();

protected:
    /** @brief 未保存方案提示；执行期间先协作中断再关闭窗口。 */
    void closeEvent(QCloseEvent *event) override;

private slots:
    /**
     * @brief Opens a file picker for the task header/configuration file.
     */
    void browseHeaderFile();

    /**
     * @brief Opens a file picker for the raw PCM data file.
     */
    void browseRawFile();
    /** @brief 文件或配置改变时作废旧结果并合并短时间内的扫描请求。 */
    void requestRawTimeScan();
    /** @brief 启动唯一后台扫描；较新的请求等待旧任务取消结束。 */
    void startRawTimeScan();
    /** @brief 在 GUI 线程接收当前文件的时间范围，拒绝过期回调。 */
    void finishRawTimeScan();

    /**
     * @brief Opens a file picker for the parameter group file and loads its parameter list.
     */
    void browseParameterGroupFile();

    /**
     * @brief Opens a file picker for the multi-time-range text file.
     */
    void browseTimeRangeFile();

    /**
     * @brief Opens a save-file picker for the result output file path.
     */
    void browseOutputDirectory();

    /**
     * @brief Enables the widgets that match the selected time processing mode.
     */
    void updateTimeMode();

    /**
     * @brief Refreshes the config parameter list after the search text changes.
     */
    void updateConfigParameterFilter();

    /**
     * @brief Refreshes the processing parameter list after the search text changes.
     */
    void updateProcessingParameterFilter();

    /**
     * @brief Adds selected config parameters to the processing parameter group.
     */
    void addSelectedConfigParameters();

    /**
     * @brief Adds every currently visible config parameter to the processing parameter group.
     */
    void addVisibleConfigParameters();

    /**
     * @brief Removes selected parameters from the processing parameter group.
     */
    void removeSelectedProcessingParameters();

    /**
     * @brief Validates the form and emits traditionalProcessingRequested().
     */
    void startProcessing();

    /**
     * @brief Marks the current task as interrupted and emits processingInterrupted().
     */
    void interruptProcessing();

    /**
     * @brief Imports a configuration file into the configuration management page.
     */
    void browseManagedConfigFile();

    /**
     * @brief Rebuilds the managed configuration parameter table after search text changes.
     */
    void filterManagedConfigParameters();

    /**
     * @brief Opens the parameter dialog and appends a new parameter to the selected stream.
     */
    void addManagedConfigParameter();

    /**
     * @brief Opens the parameter dialog for the selected parameter and applies edits.
     */
    void editManagedConfigParameter();

    /**
     * @brief Removes the selected managed configuration parameter after confirmation.
     */
    void deleteManagedConfigParameter();

    /**
     * @brief Opens a read-only detail dialog for the selected managed configuration parameter.
     */
    void viewManagedConfigParameter();

    /**
     * @brief Exports the modified managed configuration as a new file.
     */
    void exportManagedConfigFile();

    /**
     * @brief Updates the GUI after the traditional backend starts reading.
     */
    void handleTraditionalTaskStarted(const QString &task_id);

    /**
     * @brief Updates the progress bar and status label from backend progress.
     */
    void handleTraditionalTaskProgress(const QString &task_id, int progress_percent, const QString &detail);

    /**
     * @brief Restores the GUI after successful result output.
     */
    void handleTraditionalRangeFinished(const QString &task_id,
                                        int range_index,
                                        int range_count,
                                        const QString &output_path);
    void handleTraditionalProcessingFinished(const QString &task_id, const QStringList &output_paths);

    /**
     * @brief Restores the GUI after a backend failure.
     */
    void handleTraditionalTaskFailed(const QString &task_id, const QString &reason);

    /**
     * @brief Restores the GUI after cooperative interruption completes.
     */
    void handleTraditionalTaskStopped(const QString &task_id);

    /**
     * @brief Appends structured backend log records to the GUI log area.
     */
    void handleBackendLogRecord(const LogRecord &record);

    /**
     * @brief Applies the selected recent processing record to the existing traditional form.
     */
    void applySelectedProcessingHistory();

    /**
     * @brief Removes the selected recent processing record after confirmation.
     */
    void deleteSelectedProcessingHistory();

    /**
     * @brief Removes all recent processing records after confirmation.
     */
    void clearProcessingHistory();

    /**
     * @brief Updates recent-history button states after the selection or processing state changes.
     */
    void updateProcessingHistoryActions();

private:
    /**
     * @brief Persistent snapshot of one validated traditional processing run.
     */

    /**
     * @brief Creates the tabs for traditional processing, multi-task processing, and config management.
     */
    QWidget *createCentralWidget();

    /**
     * @brief Creates the currently implemented traditional processing form.
     */
    QWidget *createTraditionalPage();

    /**
     * @brief Creates a reserved page for multi-task processing.
     */
    QWidget *createMultiTaskPage();

    /**
     * @brief Creates a reserved page for configuration-file management.
     */
    QWidget *createConfigManagementPage();

    /**
     * @brief Creates the file and parameter selection group.
     */
    QGroupBox *createInputGroup();

    /**
     * @brief Creates the sampling rate and time range group.
     */
    QGroupBox *createTimeGroup();

    /**
     * @brief Creates the output option group.
     */
    QGroupBox *createOutputGroup();

    /**
     * @brief Creates the dual-list parameter selector.
     */
    QWidget *createParameterSelector(QWidget *parent);

    /**
     * @brief Creates the bottom action and status area.
     */
    QWidget *createActionArea();

    /**
     * @brief Creates the recent-processing list shown beside the existing log view.
     */
    QWidget *createProcessingHistoryArea(QWidget *parent);

    /**
     * @brief Adds a read-only path line edit and browse button to a form row.
     */
    QWidget *createPathSelector(QLineEdit *line_edit, QPushButton *browse_button);

    /**
     * @brief Returns true when all required traditional processing fields are valid.
     */
    bool validateTraditionalForm(QString *error_message) const;

    /**
     * @brief Converts the traditional page widgets into a DTO for downstream processing.
     */
    TraditionalProcessingOptions buildTraditionalOptions() const;

    /**
     * @brief Restores persistent window, paths, sample rate, time mode, and output preferences.
     */
    void loadSettings();

    /**
     * @brief Persists settings that should survive normal application restarts.
     */
    void saveSettings() const;

    /**
     * @brief Loads up to twenty recent processing snapshots from QSettings.
     */
    void loadProcessingHistory();

    /**
     * @brief Persists the current recent processing snapshots to QSettings.
     */
    void saveProcessingHistory() const;

    /**
     * @brief Adds the validated form options as a running history entry.
     */
    void addProcessingHistory(const TraditionalProcessingOptions &options,
                              const QStringList &derived_output_paths);

    /**
     * @brief Updates the status and generated files of an existing history entry.
     */
    void updateProcessingHistoryStatus(const QString &task_id,
                                       const QString &status,
                                       const QStringList &output_paths = QStringList());

    /**
     * @brief Rebuilds the visible recent-processing list from the persisted snapshots.
     */
    void refreshProcessingHistoryList();

    /**
     * @brief Returns the history model index represented by the selected list item.
     */
    int selectedProcessingHistoryIndex() const;

    /**
     * @brief Returns true when the active parameter set still matches its source group file.
     */
    bool currentParametersMatchSourceGroup() const;

    /**
     * @brief Updates enabled states and status text while a task is running or idle.
     */
    void setProcessingState(bool is_processing);

    /**
     * @brief Appends one timestamped line to the local GUI log area.
     */
    void appendLog(const QString &message);

    /**
     * @brief Reads and displays all parameter names available in a configuration file.
     */
    bool loadConfigParameterFile(const QString &file_path, QString *error_message);

    /**
     * @brief Reads and displays parameter names from a parameter group file.
     */
    bool loadParameterGroupFile(const QString &file_path, QString *error_message);

    /**
     * @brief Rebuilds the visible config parameter list using the current search text.
     */
    void refreshConfigParameterList();

    /**
     * @brief Rebuilds the visible processing parameter list using the current search text.
     */
    void refreshProcessingParameterList();

    /**
     * @brief Adds parameters to the processing group while preserving TIME as the first item.
     */
    void addProcessingParameters(const QStringList &parameter_names);

    /**
     * @brief Updates count labels for both parameter lists.
     */
    void updateParameterCountLabels();

    /**
     * @brief Rebuilds the configuration management table using the active search filter.
     */
    void refreshManagedConfigParameterTable();

    /**
     * @brief Updates button enabled states and the configuration management status label.
     */
    void updateManagedConfigState();

    /**
     * @brief Returns the document-order index stored in the selected table row.
     */
    int selectedManagedConfigParameterIndex() const;

    /**
     * @brief Opens the editable or read-only parameter form.
     */
    bool openManagedConfigParameterDialog(const QString &title,
                                          ConfigParameterRecord *record,
                                          bool is_new,
                                          bool read_only);

    /**
     * @brief Builds default values for a newly inserted parameter.
     */
    ConfigParameterRecord makeDefaultManagedConfigParameter(int stream_index) const;

    BatchProcessingPage *batch_page_ = nullptr;
    bool close_pending_ = false;
    QTabWidget *mode_tabs_ = nullptr;
    QLineEdit *header_file_edit_ = nullptr;
    QLineEdit *raw_file_edit_ = nullptr;
    QLabel *raw_time_range_label_ = nullptr;
    QTimer *raw_time_scan_timer_ = nullptr;
    QFutureWatcher<RawTimeRangeResult> *raw_time_scan_watcher_ = nullptr;
    RawTimeScanCancellation raw_time_scan_cancellation_;
    RawTimeRangeResult raw_time_range_;
    quint64 raw_time_scan_generation_ = 0;
    quint64 active_time_scan_generation_ = 0;
    quint64 time_edit_revision_ = 0;
    quint64 requested_time_edit_revision_ = 0;
    bool raw_time_scan_running_ = false;
    QString scanning_raw_path_;
    QString scanning_config_path_;
    QLineEdit *parameter_group_file_edit_ = nullptr;
    QLineEdit *config_parameter_search_edit_ = nullptr;
    QLineEdit *processing_parameter_search_edit_ = nullptr;
    QListWidget *config_parameter_list_widget_ = nullptr;
    QListWidget *processing_parameter_list_widget_ = nullptr;
    QLabel *config_parameter_count_label_ = nullptr;
    QLabel *processing_parameter_count_label_ = nullptr;
    QDoubleSpinBox *sample_rate_spin_ = nullptr;
    QRadioButton *full_time_radio_ = nullptr;
    QRadioButton *single_time_radio_ = nullptr;
    QRadioButton *multiple_time_radio_ = nullptr;
    QTimeEdit *start_time_edit_ = nullptr;
    QTimeEdit *end_time_edit_ = nullptr;
    QLineEdit *time_range_file_edit_ = nullptr;
    QPushButton *time_range_browse_button_ = nullptr;
    QCheckBox *output_source_check_ = nullptr;
    QLineEdit *output_dir_edit_ = nullptr;
    QPushButton *start_button_ = nullptr;
    QPushButton *interrupt_button_ = nullptr;
    QProgressBar *progress_bar_ = nullptr;
    QLabel *state_label_ = nullptr;
    QSplitter *history_log_splitter_ = nullptr;
    QListWidget *processing_history_list_ = nullptr;
    QPushButton *history_apply_button_ = nullptr;
    QPushButton *history_delete_button_ = nullptr;
    QPushButton *history_clear_button_ = nullptr;
    QPlainTextEdit *log_view_ = nullptr;
    QLineEdit *managed_config_path_edit_ = nullptr;
    QLineEdit *managed_config_search_edit_ = nullptr;
    QTableWidget *managed_config_table_ = nullptr;
    QPushButton *managed_config_add_button_ = nullptr;
    QPushButton *managed_config_edit_button_ = nullptr;
    QPushButton *managed_config_delete_button_ = nullptr;
    QPushButton *managed_config_view_button_ = nullptr;
    QPushButton *managed_config_export_button_ = nullptr;
    QLabel *managed_config_status_label_ = nullptr;
    ConfigFileDocument managed_config_document_;
    TraditionalProcessingController *traditional_controller_ = nullptr;
    QStringList config_parameter_names_;
    QStringList parameter_names_;
    QString parameter_group_source_path_;
    QString parameter_group_source_fingerprint_;
    QStringList parameter_group_source_parameters_;
    bool parameter_group_modified_ = false;
    QList<ProcessingHistoryEntry> processing_history_;
    QString current_traditional_task_id_;
    QStringList completed_output_paths_;
    bool is_processing_ = false;
};
