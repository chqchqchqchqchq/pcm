#pragma once

#include "config_types.h"

#include <QList>
#include <QMap>
#include <QString>
#include <QStringList>

/**
 * @brief Editable row model for one parameter entry inside a CFTE parameter description section.
 *
 * The record keeps all template fields as text so the configuration management UI can edit and export values without
 * losing formatting-specific tokens that are not yet consumed by the frame processing pipeline.
 */
struct ConfigParameterRecord
{
    int stream_index = 0;
    int serial_no = 0;
    QMap<QString, QString> fields;

    /**
     * @brief Returns the stored value for a template parameter key, or an empty string when absent.
     */
    QString value(const QString &key) const;

    /**
     * @brief Stores a template parameter field and synchronizes serial_no when the serial key changes.
     */
    void setValue(const QString &key, const QString &value);

    /**
     * @brief Returns the id_no. field used by duplicate checks and task parameter selection.
     */
    QString idNo() const;

    /**
     * @brief Returns the user-visible parameter name.
     */
    QString name() const;

    /**
     * @brief Returns a searchable text blob containing the main identifying fields.
     */
    QString searchableText() const;
};

/**
 * @brief Loads, edits, and exports CFTE configuration parameter sections for the GUI configuration manager.
 *
 * The document preserves all original non-parameter-section lines. Parameter sections are parsed into editable
 * records, then regenerated in the canonical ConfigTemplateKeys::parameterKeys() order during saveAs().
 */
class ConfigFileDocument
{
public:
    /**
     * @brief Loads a configuration document from disk and parses all parameter description sections.
     */
    bool loadFromFile(const QString &file_path, QString *error_message = nullptr);

    /**
     * @brief Loads a configuration document from text, primarily for tests and in-memory validation.
     */
    bool loadFromText(const QString &content,
                      const QString &source_path = QString(),
                      QString *error_message = nullptr);

    /**
     * @brief Exports the current document to a new path and clears the dirty flag on success.
     */
    bool saveAs(const QString &file_path, QString *error_message = nullptr);

    /**
     * @brief Serializes the current document using regenerated parameter sections.
     */
    QString toText(QString *error_message = nullptr) const;

    /**
     * @brief Returns true after a document has been loaded successfully.
     */
    bool hasContent() const;

    /**
     * @brief Returns whether parameter records have changed since load or the last successful saveAs().
     */
    bool isDirty() const;

    /**
     * @brief Returns the current source path, updated after loadFromFile() and saveAs().
     */
    QString sourcePath() const;

    /**
     * @brief Returns all editable parameter records in their document order.
     */
    QList<ConfigParameterRecord> parameters() const;

    /**
     * @brief Returns all parameter names in document order, without duplicates.
     */
    QStringList parameterNames() const;

    /**
     * @brief Returns the stream indexes that can receive new parameters.
     */
    QList<int> streamIndexes() const;

    /**
     * @brief Returns the next serial_no. value for a stream.
     */
    int nextSerialNo(int stream_index) const;

    /**
     * @brief Adds a parameter to an existing stream and updates the dirty flag.
     */
    bool addParameter(const ConfigParameterRecord &record, QString *error_message = nullptr);

    /**
     * @brief Replaces an existing parameter record and updates the dirty flag.
     */
    bool updateParameter(int parameter_index,
                         const ConfigParameterRecord &record,
                         QString *error_message = nullptr);

    /**
     * @brief Removes a parameter by document-order index and updates the dirty flag.
     */
    bool removeParameter(int parameter_index, QString *error_message = nullptr);

private:
    /**
     * @brief Marks one stream or parameter section range in the original line array.
     */
    struct SectionRange
    {
        enum class Type
        {
            StreamFormat,
            ParameterDescription
        };

        Type type = Type::StreamFormat;
        int stream_index = 0;
        int title_line = 0;
        int content_begin = 0;
        int content_end = 0;
    };

    /**
     * @brief Parses section ranges and editable parameter records from lines_.
     */
    bool parse(QString *error_message);

    /**
     * @brief Parses one parameter description section into records.
     */
    bool parseParameterSection(const SectionRange &section, QString *error_message);

    /**
     * @brief Validates a record before add, update, or export.
     */
    bool validateRecord(const ConfigParameterRecord &record,
                        int ignored_parameter_index,
                        QString *error_message) const;

    /**
     * @brief Validates every current parameter record.
     */
    bool validateAll(QString *error_message) const;

    /**
     * @brief Returns a normalized record with all template keys present.
     */
    ConfigParameterRecord normalizedRecord(const ConfigParameterRecord &record) const;

    /**
     * @brief Returns whether a parameter section exists for the stream.
     */
    bool hasParameterSection(int stream_index) const;

    /**
     * @brief Counts current records in one stream for parameter_number export.
     */
    int parameterCountForStream(int stream_index) const;

    /**
     * @brief Serializes all records for a parameter section stream.
     */
    QStringList serializedParameterLines(int stream_index) const;

    QString source_path_;
    QStringList lines_;
    QString newline_ = QStringLiteral("\n");
    QList<SectionRange> sections_;
    QList<ConfigParameterRecord> parameters_;
    QList<int> stream_indexes_;
    bool has_content_ = false;
    bool dirty_ = false;
};

