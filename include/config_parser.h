#pragma once

#include "config_types.h"

#include <QList>
#include <QString>

/**
 * @brief 根据可编辑配置对象生成并校验每个流的参数布局表。
 *
 * 布局算法读取每个流的 ParameterDescription 列表，为每个参数保留前 pcm_words 个
 * ParameterWordSegment，校验帧号、字号、源位和目标位范围，并展开成 StreamLayoutEntry。
 * 生成的 StreamLayoutTable 持有快速 id/name 索引，供 GUI 和帧解算模块查询；原始 Config 仍保持可编辑。
 */
class ConfigParser
{
public:
    /**
     * @brief 为单个流生成可查询的布局表。
     * @return 布局合法并写入 layout_table 时返回 true。
     */
    static bool generateLayoutTable(const StreamFormatDescription &stream,
                                    StreamLayoutTable *layout_table,
                                    QString *error_message = nullptr);

    /**
     * @brief 按顺序为所有流生成布局表。
     * @return 所有流布局均合法时返回 true。
     */
    static bool generateLayoutTables(const QList<StreamFormatDescription> &streams,
                                     QList<StreamLayoutTable> *layout_tables,
                                     QString *error_message = nullptr);

    /**
     * @brief 在加载或编辑参数后重建 Config::layout_tables。
     * @return 所有流布局表重建成功时返回 true。
     */
    static bool rebuildLayoutTables(Config *config, QString *error_message = nullptr);
};
