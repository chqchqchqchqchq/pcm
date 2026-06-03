#pragma once

#include "config_types.h"

#include <QMetaType>
#include <QString>
#include <QStringList>

/**
 * @brief Options collected by the traditional processing page before the backend pipeline is invoked.
 *
 * task_definition keeps the common five-tuple fields used by TaskScheduler. The remaining fields describe GUI choices
 * such as the parameter-group source file, multi-range mode, and the exact single-task output file path.
 */
struct TraditionalProcessingOptions
{
    TaskDefinition task_definition;
    QString parameter_group_file_path;
    QStringList parameter_names;
    bool use_multiple_time_ranges = false;
    QString time_range_file_path;
    QString output_file_path;
    bool output_source_code = false;
};

Q_DECLARE_METATYPE(TraditionalProcessingOptions)
