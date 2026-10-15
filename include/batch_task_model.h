#pragma once
#include "batch_plan.h"
#include <QAbstractTableModel>
#include <QHash>

/** @brief 任务方案表格模型；运行状态不触发文档修改标记。 */
class BatchTaskModel : public QAbstractTableModel
{
    Q_OBJECT
public:
    enum Column { Order, Name, ConfigFile, RawFile, SampleRate, Parameters, TimeRange, State, Progress, ColumnCount };
    /** @brief 创建空方案模型。 */
    explicit BatchTaskModel(QObject *parent = nullptr);
    /** @brief 平面任务行数。 */
    int rowCount(const QModelIndex &parent = {}) const override;
    /** @brief 固定信息列数。 */
    int columnCount(const QModelIndex &parent = {}) const override;
    /** @brief 返回摘要、完整路径及运行明细。 */
    QVariant data(const QModelIndex &index, int role = Qt::DisplayRole) const override;
    /** @brief 中文列标题。 */
    QVariant headerData(int section, Qt::Orientation orientation, int role) const override;
    /** @brief 当前方案的值快照。 */
    BatchPlan plan() const;
    /** @brief 成功打开后替换整个文档，并清除运行状态。 */
    void setPlan(const BatchPlan &plan);
    /** @brief 追加独立快照。 */
    void appendTask(const BatchTaskItem &item);
    /** @brief 替换一条任务并清除该行旧运行结果。 */
    void replaceTask(int row, const BatchTaskItem &item);
    /** @brief 移除选中任务。 */
    void removeTask(int row);
    /** @brief 按列表位置上下移动任务。 */
    void moveTask(int row, int destination);
    /** @brief 更新方案名称或根目录。 */
    void setMetadata(const QString &name, const QString &output_root);
    /** @brief 读取一条任务，不存在则返回空默认项。 */
    BatchTaskItem task(int row) const;
    /** @brief 读取指定任务本次运行状态。 */
    BatchTaskRuntime runtime(const QString &id) const;
    /** @brief 更新状态，不影响任务设置及文档修改标记。 */
    void setRuntime(const QString &id, const BatchTaskRuntime &runtime);
    /** @brief 查询是否包含未保存设置。 */
    bool isModified() const;
    /** @brief 原子保存成功后清除修改标记。 */
    void markSaved();
    /** @brief 运行期间阻止任务及元信息修改。 */
    void setLocked(bool locked);
    /** @brief 当前是否锁定编辑。 */
    bool isLocked() const;
signals:
    /** @brief 文档内容改变或保存状态刷新。 */
    void documentChanged();
private:
    /** @brief 标记用户设置已改变。 */
    void changed();
    BatchPlan plan_;
    QHash<QString, BatchTaskRuntime> runtime_;
    bool modified_ = false;
    bool locked_ = false;
};
