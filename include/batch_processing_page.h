#pragma once
#include "batch_processing_controller.h"
#include "batch_task_model.h"
#include "raw_time_range_scanner.h"
#include <QFutureWatcher>
#include <QWidget>

class QLineEdit;
class QLabel;
class QDoubleSpinBox;
class QTableView;
class QPlainTextEdit;
class QPushButton;
class QProgressBar;

/** @brief 多任务方案工作区，负责文档交互、选中详情和运行状态展示。 */
class BatchProcessingPage : public QWidget
{
    Q_OBJECT
public:
    /** @brief 构建操作栏、任务表、详情、进度和日志。 */
    explicit BatchProcessingPage(QWidget *parent = nullptr);
    /** @brief 停止仅用于界面提示的后台扫描。 */
    ~BatchProcessingPage() override;
    /** @brief 提供任务模型供主窗口或自动化测试访问。 */
    BatchTaskModel *model() const;
    /** @brief 提供本页面的执行控制器。 */
    BatchProcessingController *controller() const;
    /** @brief 原子读取并替换方案；调用前由 UI 处理未保存修改。 */
    bool loadPlan(const QString &path, QString *error = nullptr);
    /** @brief 保存成功后更新当前文件路径及修改标记。 */
    bool savePlan(const QString &path, QString *error = nullptr);
    /** @brief 主窗口关闭或切换文档前询问未保存设置。 */
    bool confirmSaveChanges();
    /** @brief 单任务仍在执行或清理时禁止本页面启动。 */
    void setExternalBusy(bool busy);
    /** @brief 开始处理中保持稳定快照；失败校验显示完整错误。 */
    void startProcessing();
signals:
    /** @brief 向主窗口报告多任务忙状态，实现跨页面互斥。 */
    void runningChanged(bool running);
private:
    /** @brief 新建空方案。 */
    void newPlan();
    /** @brief 选择并打开方案文件。 */
    void openPlan();
    /** @brief 保存或另存为当前方案。 */
    bool save(bool save_as = false);
    /** @brief 从历史记录勾选任意多条独立任务。 */
    void addHistory();
    /** @brief 打开共用任务编辑器，成功后提交快照。 */
    void editTask(bool adding);
    /** @brief 返回当前选中行。 */
    int selectedRow() const;
    /** @brief 刷新详情字段及结果路径。 */
    void updateDetails();
    /** @brief 刷新按钮状态、修改标记和任务统计。 */
    void updateActions();
    /** @brief 编辑或打开后检查全部任务，不要求输出根目录已填写。 */
    void validateRows();
    /** @brief 带时间戳追加有界界面日志。 */
    void appendLog(const QString &text);
    /** @brief 为当前选中任务扫描时间，旧结果不覆盖新任务。 */
    void requestDetailScan();
    /** @brief 前一个扫描完成后启动最新请求。 */
    void startDetailScan();
    BatchTaskModel *model_ = nullptr;
    BatchProcessingController *controller_ = nullptr;
    QTableView *table_ = nullptr;
    QLineEdit *name_ = nullptr;
    QLineEdit *output_root_ = nullptr;
    QLineEdit *raw_ = nullptr;
    QDoubleSpinBox *rate_ = nullptr;
    QLabel *file_label_ = nullptr;
    QLabel *details_ = nullptr;
    QLabel *scan_label_ = nullptr;
    QLabel *summary_ = nullptr;
    QLabel *state_ = nullptr;
    QPlainTextEdit *log_ = nullptr;
    QProgressBar *progress_ = nullptr;
    QPushButton *start_ = nullptr;
    QPushButton *stop_ = nullptr;
    QPushButton *edit_ = nullptr;
    QPushButton *remove_ = nullptr;
    QPushButton *up_ = nullptr;
    QPushButton *down_ = nullptr;
    QPushButton *change_raw_ = nullptr;
    QPushButton *results_ = nullptr;
    QList<QPushButton *> mutation_buttons_;
    QString file_path_;
    QString last_results_directory_;
    bool external_busy_ = false;
    QFutureWatcher<RawTimeRangeResult> *scan_watcher_ = nullptr;
    RawTimeScanCancellation cancellation_;
    QString scan_key_;
    bool scan_running_ = false;
    quint64 scan_version_ = 0;
    quint64 active_scan_version_ = 0;
};
