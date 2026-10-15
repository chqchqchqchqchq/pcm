#include "batch_task_model.h"
#include "processing_options_support.h"
#include <QFileInfo>

BatchTaskModel::BatchTaskModel(QObject *parent) : QAbstractTableModel(parent) {}
int BatchTaskModel::rowCount(const QModelIndex &parent) const { return parent.isValid() ? 0 : plan_.tasks.size(); }
int BatchTaskModel::columnCount(const QModelIndex &parent) const { return parent.isValid() ? 0 : ColumnCount; }

QVariant BatchTaskModel::data(const QModelIndex &index, int role) const
{
    if (!index.isValid() || index.row() < 0 || index.row() >= plan_.tasks.size()) { return {}; }
    const auto &item = plan_.tasks.at(index.row());
    const auto state = runtime_.value(item.id);
    if (role == Qt::UserRole) { return item.id; }
    if (role == Qt::ToolTipRole) {
        if (index.column() == ConfigFile) { return item.options.task_definition.config_file_path; }
        if (index.column() == RawFile) { return item.options.task_definition.raw_file_path; }
        if (index.column() == Parameters) { return item.options.parameter_names.join(QStringLiteral("、")); }
        if (index.column() == TimeRange) { return item.timeDescription(); }
        return state.detail;
    }
    if (role != Qt::DisplayRole) { return {}; }
    switch (index.column()) {
    case Order: return index.row() + 1;
    case Name: return item.name;
    case ConfigFile: return QFileInfo(item.options.task_definition.config_file_path).fileName();
    case RawFile: return QFileInfo(item.options.task_definition.raw_file_path).fileName();
    case SampleRate: return QStringLiteral("%1 Hz").arg(item.options.task_definition.user_sample_rate_hz, 0, 'g', 12);
    case Parameters: return QStringLiteral("%1 个").arg(item.options.parameter_names.size());
    case TimeRange:
        return item.time_mode == 2 && item.time_ranges_resolved
            ? QStringLiteral("%1 个时间段").arg(item.options.time_ranges.size()) : item.timeDescription();
    case State: return batchTaskStateText(state.state);
    case Progress: return state.progress;
    }
    return {};
}

QVariant BatchTaskModel::headerData(int section, Qt::Orientation orientation, int role) const
{
    if (orientation != Qt::Horizontal || role != Qt::DisplayRole) { return {}; }
    const QStringList names{QStringLiteral("序号"), QStringLiteral("任务名称"), QStringLiteral("配置文件"),
        QStringLiteral("原始数据"), QStringLiteral("采样率"), QStringLiteral("参数"), QStringLiteral("时间段"),
        QStringLiteral("状态"), QStringLiteral("进度")};
    return section >= 0 && section < names.size() ? QVariant(names.at(section)) : QVariant();
}

BatchPlan BatchTaskModel::plan() const { return plan_; }
BatchTaskItem BatchTaskModel::task(int row) const { return plan_.tasks.value(row); }
BatchTaskRuntime BatchTaskModel::runtime(const QString &id) const { return runtime_.value(id); }
bool BatchTaskModel::isModified() const { return modified_; }
bool BatchTaskModel::isLocked() const { return locked_; }
void BatchTaskModel::setLocked(bool locked) { locked_ = locked; }
void BatchTaskModel::markSaved() { modified_ = false; emit documentChanged(); }
void BatchTaskModel::changed() { modified_ = true; emit documentChanged(); }

void BatchTaskModel::setPlan(const BatchPlan &plan)
{
    if (locked_) { return; }
    beginResetModel();
    plan_ = plan;
    runtime_.clear();
    modified_ = false;
    endResetModel();
    emit documentChanged();
}

void BatchTaskModel::appendTask(const BatchTaskItem &item)
{
    if (locked_) { return; }
    beginInsertRows({}, plan_.tasks.size(), plan_.tasks.size());
    auto added = item;
    added.options = added.processingOptions();
    plan_.tasks.append(added);
    endInsertRows();
    changed();
}

void BatchTaskModel::replaceTask(int row, const BatchTaskItem &item)
{
    if (locked_ || row < 0 || row >= plan_.tasks.size()) { return; }
    auto replacement = item;
    replacement.id = plan_.tasks.at(row).id;
    replacement.options = replacement.processingOptions();
    plan_.tasks[row] = replacement;
    runtime_.remove(replacement.id);
    emit dataChanged(index(row, 0), index(row, ColumnCount - 1));
    changed();
}

void BatchTaskModel::removeTask(int row)
{
    if (locked_ || row < 0 || row >= plan_.tasks.size()) { return; }
    beginRemoveRows({}, row, row);
    runtime_.remove(plan_.tasks.at(row).id);
    plan_.tasks.removeAt(row);
    endRemoveRows();
    changed();
}

void BatchTaskModel::moveTask(int row, int destination)
{
    if (locked_ || row < 0 || destination < 0 || row >= rowCount() || destination >= rowCount() || row == destination) {
        return;
    }
    beginMoveRows({}, row, row, {}, destination > row ? destination + 1 : destination);
    plan_.tasks.move(row, destination);
    endMoveRows();
    emit dataChanged(index(qMin(row, destination), Order), index(qMax(row, destination), Order));
    changed();
}

void BatchTaskModel::setMetadata(const QString &name, const QString &output_root)
{
    if (locked_) { return; }
    const auto normalized = ProcessingOptionsSupport::absolutePath(output_root);
    if (plan_.name == name && plan_.output_root == normalized) { return; }
    plan_.name = name;
    plan_.output_root = normalized;
    changed();
}

void BatchTaskModel::setRuntime(const QString &id, const BatchTaskRuntime &runtime)
{
    for (int row = 0; row < plan_.tasks.size(); ++row) {
        if (plan_.tasks.at(row).id != id) { continue; }
        runtime_.insert(id, runtime);
        emit dataChanged(index(row, State), index(row, Progress));
        return;
    }
}
