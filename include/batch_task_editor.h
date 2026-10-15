#pragma once
#include "batch_plan.h"
#include "raw_time_range_scanner.h"
#include <QDialog>
#include <QFutureWatcher>

class QLineEdit;
class QLabel;
class QListWidget;
class QDoubleSpinBox;
class QComboBox;
class QPlainTextEdit;
class QCheckBox;
class QTimer;

/** @brief 新增/编辑任务共用对话框，取消时不写回；时间扫描持有独立取消标记。 */
class BatchTaskEditor : public QDialog
{
    Q_OBJECT
public:
    /** @brief 从快照填充编辑器；新建任务使用空快照。 */
    explicit BatchTaskEditor(const BatchTaskItem &item, bool adding, QWidget *parent = nullptr);
    /** @brief 取消仍在进行的时间扫描，不让后台线程引用窗口。 */
    ~BatchTaskEditor() override;
    /** @brief 只有接受后才使用的编辑结果。 */
    BatchTaskItem task() const;
public slots:
    /** @brief 读取字段及时间段快照，允许待修正的文件路径保存为草稿。 */
    void accept() override;
private:
    /** @brief 加载当前配置的可选参数；不静默丢弃已选参数。 */
    void loadConfig();
    /** @brief 按搜索条件显示两侧参数列表。 */
    void refreshParameters();
    /** @brief 参数组文件导入和已有快照保持一致。 */
    void importGroup();
    /** @brief 导入时间段文本并保存解析结果。 */
    void importRanges();
    /** @brief 合并扫描请求，取消旧请求，并拒绝旧版本回调。 */
    void scheduleScan();
    /** @brief 启动唯一后台扫描。 */
    void startScan();
    BatchTaskItem item_;
    QLineEdit *name_ = nullptr;
    QLineEdit *config_ = nullptr;
    QLineEdit *raw_ = nullptr;
    QLineEdit *group_ = nullptr;
    QLineEdit *config_search_ = nullptr;
    QLineEdit *selected_search_ = nullptr;
    QListWidget *available_ = nullptr;
    QListWidget *selected_ = nullptr;
    QDoubleSpinBox *rate_ = nullptr;
    QComboBox *time_mode_ = nullptr;
    QLineEdit *start_ = nullptr;
    QLineEdit *end_ = nullptr;
    QLineEdit *ranges_path_ = nullptr;
    QPlainTextEdit *ranges_ = nullptr;
    QCheckBox *raw_values_ = nullptr;
    QLabel *scan_label_ = nullptr;
    QLabel *error_ = nullptr;
    QTimer *scan_timer_ = nullptr;
    QFutureWatcher<RawTimeRangeResult> *scan_watcher_ = nullptr;
    RawTimeScanCancellation cancellation_;
    bool scan_running_ = false;
    quint64 scan_version_ = 0;
    quint64 active_scan_version_ = 0;
    QStringList config_names_;
    QStringList selected_names_;
};
