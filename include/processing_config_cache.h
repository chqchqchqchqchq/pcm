#pragma once

#include "config_types.h"
#include <QHash>
#include <QMutex>

/** @brief 批次生命周期内按路径和内容指纹共享只读配置及布局；不缓存原始数据。 */
class ProcessingConfigCache
{
public:
    /** @brief 同步加载或复用解析对象；文件变化时替换该路径缓存。 */
    ConstConfigPtr load(const QString &path, QString *error = nullptr);
    /** @brief 释放缓存持有的引用；在途任务持有的快照继续有效。 */
    void clear();
private:
    struct Entry { QString fingerprint; ConstConfigPtr config; };
    QMutex mutex_;
    QHash<QString, Entry> entries_;
};
