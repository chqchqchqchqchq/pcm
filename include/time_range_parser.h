#pragma once

#include "config_types.h"

#include <QList>
#include <QString>
#include <QStringList>

/**
 * @brief Parses user-facing single and multiple PCM processing time ranges.
 *
 * Each non-empty input line contains a start and end value. Values can be decimal seconds or
 * HH:mm:ss[.zzz]. Commas, tabs, and whitespace are accepted as separators; text after '#' is ignored.
 * Parsed ranges use an inclusive start and exclusive end.
 */
class TimeRangeParser
{
public:
    /**
     * @brief Parses a time-range text file and reports line-specific validation errors.
     */
    static bool parseFile(const QString &file_path,
                          QList<TaskTimeRange> *ranges,
                          QStringList *warnings = nullptr,
                          QString *error_message = nullptr);

    /**
     * @brief Parses time ranges from text, primarily for GUI validation and tests.
     */
    static bool parseText(const QString &content,
                          QList<TaskTimeRange> *ranges,
                          QStringList *warnings = nullptr,
                          QString *error_message = nullptr);

    /**
     * @brief Parses decimal seconds or HH:mm:ss[.zzz] into seconds.
     */
    static bool parseTimeValue(const QString &text, double *seconds, QString *error_message = nullptr);
};
