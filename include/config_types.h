#pragma once

#include <QByteArray>
#include <QDate>
#include <QDateTime>
#include <QHash>
#include <QList>
#include <QMetaType>
#include <QSharedPointer>
#include <QString>
#include <QStringList>
#include <QVariant>
#include <QtGlobal>

/**
 * @brief 当前 CFTE 配置模板使用的默认参数拼字映射列数。
 *
 * 模板会为 frame_no、word_no、source_bit、bit_length 和 object_bit 写出 6 个逗号分隔的位置。
 * pcm_words 表示每个参数实际使用前几个位置。
 */
inline constexpr int kDefaultParameterWordColumns = 6;

/**
 * @brief 配置文件模板中的固定段名和字段名。
 *
 * 中英文括号内的文字只属于模板注释，实际配置文件不包含。解析和导出时必须严格使用这里列出的键名，
 * 大小写、下划线和标点均不可替换；参数单位字段只使用 unit。
 */
namespace ConfigTemplateKeys {

/** @brief 可选校准字段，基础模板字段集合保持不变。 */
inline QStringList calibrationKeys()
{
    return {QStringLiteral("coefficient"), QStringLiteral("segment_number"), QStringLiteral("code,phy"),
            QStringLiteral("segment_point"), QStringLiteral("first_segment"), QStringLiteral("second_segment")};
}

/**
 * @brief 返回配置文件第一行固定签名。
 */
inline QString fileSignature()
{
    return QStringLiteral("#CFTE/filemain/rec01.00");
}

/**
 * @brief 返回文件头信息段名。
 */
inline QString headerSection()
{
    return QStringLiteral("Header_informations");
}

/**
 * @brief 返回指定流的格式描述段名。
 */
inline QString streamSectionTitle(int stream_index)
{
    return QStringLiteral("The format description of stream %1").arg(stream_index);
}

/**
 * @brief 返回指定流的参数描述段名。
 */
inline QString parameterSectionTitle(int stream_index)
{
    return QStringLiteral("the_parameter_description_of_stream %1").arg(stream_index);
}

/**
 * @brief 返回文件头段允许出现的严格键名列表。
 */
inline QStringList headerKeys()
{
    return {
        QStringLiteral("Create_date"),
        QStringLiteral("test_id"),
        QStringLiteral("stream_number"),
        QStringLiteral("derpar_number"),
    };
}

/**
 * @brief 返回流格式段允许出现的严格键名列表。
 */
inline QStringList streamKeys()
{
    return {
        QStringLiteral("aircraft_name"),
        QStringLiteral("test_name"),
        QStringLiteral("parameter_number"),
        QStringLiteral("bit_rate"),
        QStringLiteral("pcm_code"),
        QStringLiteral("loop_width"),
        QStringLiteral("word_length"),
        QStringLiteral("parity"),
        QStringLiteral("Onboard_acq._equipments_type"),
        QStringLiteral("frame_length"),
        QStringLiteral("subframe_deepth"),
        QStringLiteral("major_frame_rate"),
        QStringLiteral("sync_word_length"),
        QStringLiteral("sync_pattern"),
        QStringLiteral("id_position"),
        QStringLiteral("id_lsb"),
        QStringLiteral("dating_type"),
        QStringLiteral("time_interpolation"),
        QStringLiteral("output_polarity"),
        QStringLiteral("input_source"),
        QStringLiteral("input_polarity"),
        QStringLiteral("bits_order"),
        QStringLiteral("save_last_frame"),
        QStringLiteral("sync_strategy"),
        QStringLiteral("sync_location"),
        QStringLiteral("subframe_mode"),
        QStringLiteral("direction"),
        QStringLiteral("first_value"),
        QStringLiteral("pattern_loop"),
    };
}

/**
 * @brief 返回参数描述段允许出现的严格键名列表。
 */
inline QStringList parameterKeys()
{
    return {
        QStringLiteral("serial_no."),
        QStringLiteral("id_no."),
        QStringLiteral("name"),
        QStringLiteral("unit"),
        QStringLiteral("cal_type"),
        QStringLiteral("code_type"),
        QStringLiteral("cal_date"),
        QStringLiteral("cal_number"),
        QStringLiteral("rate"),
        QStringLiteral("word_interval"),
        QStringLiteral("sign_bit"),
        QStringLiteral("inverse"),
        QStringLiteral("pcm_words"),
        QStringLiteral("frame_no"),
        QStringLiteral("word_no"),
        QStringLiteral("source_bit"),
        QStringLiteral("bit_length"),
        QStringLiteral("object_bit"),
        QStringLiteral("Description"),
    };
}

/**
 * @brief 判断 key 是否为文件头段的合法键名，大小写敏感。
 */
inline bool isHeaderKey(const QString &key)
{
    return headerKeys().contains(key, Qt::CaseSensitive);
}

/**
 * @brief 判断 key 是否为流格式段的合法键名，大小写敏感。
 */
inline bool isStreamKey(const QString &key)
{
    return streamKeys().contains(key, Qt::CaseSensitive);
}

/**
 * @brief 判断 key 是否为参数描述段的合法键名，大小写敏感。
 */
inline bool isParameterKey(const QString &key)
{
    return parameterKeys().contains(key, Qt::CaseSensitive) || calibrationKeys().contains(key);
}

} // namespace ConfigTemplateKeys

/**
 * @brief 单个参数使用的校准算法类型。
 */
enum class CalibrationType
{
    Unknown,
    None,
    Linear,
    Polynomial,
    Segmented,
    Hyperbolic
};

/**
 * @brief 参数原始 PCM 编码类型，用于参数提取和工程值转换。
 */
enum class ParameterCodeType
{
    Unknown,
    Bcd,
    UnsignedInteger,
    SignedInteger,
    FloatingPoint,
    SignedMagnitude,
    Fcs618
};

/**
 * @brief 将配置模板中的校准类型文本转换为强类型枚举。
 */
inline CalibrationType calibrationTypeFromToken(const QString &token)
{
    const QString normalized_token = token.trimmed().toLower();
    if (normalized_token == QStringLiteral("no") || normalized_token == QStringLiteral("none")) {
        return CalibrationType::None;
    }
    if (normalized_token == QStringLiteral("line") || normalized_token == QStringLiteral("linear")) {
        return CalibrationType::Linear;
    }
    if (normalized_token == QStringLiteral("poly") || normalized_token == QStringLiteral("polynomial")) {
        return CalibrationType::Polynomial;
    }
    if (normalized_token == QStringLiteral("segm") || normalized_token == QStringLiteral("segmented") ||
        normalized_token == QStringLiteral("fillcode")) {
        return CalibrationType::Segmented;
    }

    if (normalized_token == QStringLiteral("hyper")) {
        return CalibrationType::Hyperbolic;
    }
    return CalibrationType::Unknown;
}

/**
 * @brief 将强类型校准枚举转换为导出配置文件时使用的文本。
 */
inline QString calibrationTypeToToken(CalibrationType calibration_type)
{
    switch (calibration_type) {
    case CalibrationType::None:
        return QStringLiteral("NO");
    case CalibrationType::Linear:
        return QStringLiteral("line");
    case CalibrationType::Polynomial:
        return QStringLiteral("poly");
    case CalibrationType::Segmented:
        return QStringLiteral("segm");
    case CalibrationType::Hyperbolic:
        return QStringLiteral("hyper");
    case CalibrationType::Unknown:
    default:
        return QString();
    }
}

/**
 * @brief 将配置模板中的 code_type 文本转换为强类型枚举。
 */
inline ParameterCodeType parameterCodeTypeFromToken(const QString &token)
{
    const QString normalized_token = token.trimmed().toLower();
    if (normalized_token == QStringLiteral("xint")) {
        return ParameterCodeType::SignedMagnitude;
    }
    if (normalized_token == QStringLiteral("fcs618")) {
        return ParameterCodeType::Fcs618;
    }
    if (normalized_token == QStringLiteral("bcd")) {
        return ParameterCodeType::Bcd;
    }
    if (normalized_token == QStringLiteral("uint")) {
        return ParameterCodeType::UnsignedInteger;
    }
    if (normalized_token == QStringLiteral("sint")) {
        return ParameterCodeType::SignedInteger;
    }
    if (normalized_token == QStringLiteral("fcsf") || normalized_token == QStringLiteral("float")) {
        return ParameterCodeType::FloatingPoint;
    }

    return ParameterCodeType::Unknown;
}

/**
 * @brief 将强类型 code_type 枚举转换为导出配置文件时使用的文本。
 */
inline QString parameterCodeTypeToToken(ParameterCodeType code_type)
{
    switch (code_type) {
    case ParameterCodeType::Bcd:
        return QStringLiteral("bcd");
    case ParameterCodeType::UnsignedInteger:
        return QStringLiteral("uint");
    case ParameterCodeType::SignedInteger:
        return QStringLiteral("sint");
    case ParameterCodeType::FloatingPoint:
        return QStringLiteral("fcsf");
    case ParameterCodeType::SignedMagnitude:
        return QStringLiteral("xint");
    case ParameterCodeType::Fcs618:
        return QStringLiteral("fcs618");
    case ParameterCodeType::Unknown:
    default:
        return QString();
    }
}

/**
 * @brief 分段校准或查表工程转换曲线中的一个点。
 */
struct CalibrationPoint
{
    double raw_value = 0.0;
    double engineering_value = 0.0;
};

/**
 * @brief 单个参数的校准元数据和可选转换数据。
 *
 * type_token 保留文件中的原始文本，便于原样导出。type 是处理代码使用的规范化值。
 * coefficients 用于线性/多项式转换，points 用于分段转换。
 */
struct CalibrationDefinition
{
    double segment_point = 0.0;
    CalibrationType type = CalibrationType::None;
    QString type_token = QStringLiteral("NO");
    QDate cal_date;
    QString cal_number;
    QList<double> coefficients;
    QList<CalibrationPoint> points;
};

/**
 * @brief 描述一个逻辑参数字在 PCM 子帧中的位段位置。
 *
 * frame_no 是 0 基子帧序号，word_no 是该子帧内 0 基字偏移。source_bit 和 bit_length 指定源位范围。
 * 沿用旧版：目标位全零时按数组顺序拼接，否则按 object_bit 放置各段。
 */
struct ParameterWordSegment
{
    int frame_no = 0;
    int word_no = 0;
    int source_bit = 0;
    int bit_length = 0;
    int object_bit = 0;

    /**
     * @brief 当该位段至少包含 1 个有效位时返回 true。
     */
    bool isActive() const
    {
        return bit_length > 0;
    }
};

/**
 * @brief 来自 the_parameter_description_of_stream_N 的可编辑参数定义。
 *
 * id_no 以文本保存，因为模板会把流号和参数序号拼接，未来文件也可能需要保留前导零。
 * word_segments 通常保留 6 个条目以匹配导出格式，pcm_words 控制帧解算实际使用前几个条目。
 */
struct ParameterDescription
{
    int serial_no = 0;
    QString id_no;
    QString name;
    QString unit;
    ParameterCodeType code_type = ParameterCodeType::UnsignedInteger;
    QString code_type_token = QStringLiteral("uint");
    CalibrationDefinition calibration;
    double rate_hz = 0.0;
    int word_interval = 0;
    int sign_bit = 0;
    bool inverse = false;
    int pcm_words = 1;
    QList<ParameterWordSegment> word_segments;
    QString description;

    /**
     * @brief 返回 pcm_words 选中的前若干个位段。
     */
    QList<ParameterWordSegment> activeWordSegments() const
    {
        QList<ParameterWordSegment> active_segments;
        const int segment_count = static_cast<int>(word_segments.size());
        const int active_count = qBound(0, pcm_words, segment_count);
        active_segments.reserve(active_count);
        for (int index = 0; index < active_count; ++index) {
            active_segments.append(word_segments.at(index));
        }

        return active_segments;
    }
};

/**
 * @brief 流描述开始之前的配置文件头字段。
 */
struct ConfigHeader
{
    QString file_signature = ConfigTemplateKeys::fileSignature();
    QString section_name = ConfigTemplateKeys::headerSection();
    QDateTime create_date;
    QString test_id;
    int stream_number = 0;
    int derived_parameter_number = 0;
};

/**
 * @brief 一个流格式描述块，对应 "The format description of stream N"。
 *
 * 字符串字段用于保持导出文本稳定，规范化数值字段用于原始数据读取、帧提取、校验和布局表生成。
 * sync_pattern_bytes 是 RawFileReader 分流时使用的同步字节序列。
 */
struct StreamFormatDescription
{
    bool legacy_semantics = false;
    int stream_index = 0;
    QString stream_id;
    QString format_section_title;
    QString parameter_section_title;
    QString aircraft_name;
    QString test_name;
    int parameter_number = 0;
    double bit_rate_bps = 0.0;
    QString pcm_code = QStringLiteral("NRZ-L");
    double loop_width = 0.1;
    int word_length_bits = 16;
    QString parity = QStringLiteral("NONE");
    QString onboard_acq_equipments_type;
    int frame_length_words = 0;
    int subframe_depth = 0;
    double major_frame_rate_hz = 0.0;
    int sync_word_length_bits = 0;
    quint64 sync_pattern_value = 0;
    QString sync_pattern_text;
    QByteArray sync_pattern_bytes;
    int id_position_word = -1;
    int id_lsb = 0;
    QString dating_type = QStringLiteral("counter");
    int time_interpolation = 0;
    QString output_polarity = QStringLiteral("normal");
    QString input_source = QStringLiteral("internal_bs");
    QString input_polarity = QStringLiteral("normal");
    QString bits_order = QStringLiteral("normal");
    bool save_last_frame = false;
    QString sync_strategy = QStringLiteral("normal");
    int sync_location = 0;
    QString subframe_mode = QStringLiteral("sfid");
    QString direction = QStringLiteral("increasing counter");
    int first_value = 0;
    int pattern_loop = 0;
    QList<ParameterDescription> parameters;

    /**
     * @brief 返回该流导出时使用的流格式段名。
     */
    QString resolvedFormatSectionTitle() const
    {
        if (!format_section_title.trimmed().isEmpty()) {
            return format_section_title.trimmed();
        }

        return ConfigTemplateKeys::streamSectionTitle(stream_index);
    }

    /**
     * @brief 返回该流导出时使用的参数描述段名。
     */
    QString resolvedParameterSectionTitle() const
    {
        if (!parameter_section_title.trimmed().isEmpty()) {
            return parameter_section_title.trimmed();
        }

        return ConfigTemplateKeys::parameterSectionTitle(stream_index);
    }

    /**
     * @brief 返回一个子帧包含的比特数。
     */
    qint64 bitsPerSubframe() const
    {
        return static_cast<qint64>(frame_length_words) * word_length_bits;
    }

    /**
     * @brief 返回一个主帧包含的字数。
     */
    qint64 wordsPerMajorFrame() const
    {
        return static_cast<qint64>(frame_length_words) * subframe_depth;
    }

    /**
     * @brief 根据帧布局字段计算期望的流比特率。
     */
    double calculatedBitRate() const
    {
        return static_cast<double>(wordsPerMajorFrame()) * word_length_bits * major_frame_rate_hz;
    }

    /**
     * @brief 计算模板中参数使用的 word_interval 值。
     */
    int calculatedWordInterval(double parameter_rate) const
    {
        if (parameter_rate <= 0.0) {
            return 0;
        }

        return static_cast<int>((static_cast<double>(wordsPerMajorFrame()) / parameter_rate) + 0.5);
    }
};

/**
 * @brief 帧解算模块使用的扁平化参数布局项。
 */
struct StreamLayoutEntry
{
    QString parameter_id;
    QString parameter_name;
    QString unit;
    int serial_no = 0;
    double rate_hz = 0.0;
    ParameterCodeType code_type = ParameterCodeType::Unknown;
    int sign_bit = 0;
    bool inverse = false;
    QList<ParameterWordSegment> word_segments;
    CalibrationDefinition calibration;
    QString description;
};

/**
 * @brief 根据 StreamFormatDescription 生成的单流查询布局表。
 *
 * ConfigParser 在加载或编辑参数后应重建 id/name 索引。帧解算模块可以直接遍历 entries，
 * GUI 也可以按 id 或名称查询参数，而不需要重新解析原始文本。
 */
struct StreamLayoutTable
{
    int id_position_word = -1;
    int id_lsb = 0;
    bool legacy_semantics = false;
    double major_frame_rate_hz = 0.0;
    QString time_code_type;
    int stream_index = 0;
    QString stream_id;
    int word_length_bits = 16;
    int frame_length_words = 0;
    int subframe_depth = 0;
    QList<StreamLayoutEntry> entries;
    QHash<QString, int> entry_index_by_id;
    QHash<QString, int> entry_index_by_name;

    /**
     * @brief 根据当前 entries 列表重建参数 id 和名称索引。
     */
    void rebuildIndexes()
    {
        entry_index_by_id.clear();
        entry_index_by_name.clear();
        for (int index = 0; index < entries.size(); ++index) {
            const StreamLayoutEntry &entry = entries.at(index);
            if (!entry.parameter_id.isEmpty()) {
                entry_index_by_id.insert(entry.parameter_id, index);
            }
            if (!entry.parameter_name.isEmpty()) {
                entry_index_by_name.insert(entry.parameter_name, index);
            }
        }
    }

    /**
     * @brief 按参数 id 查找布局项。
     */
    const StreamLayoutEntry *findById(const QString &parameter_id) const
    {
        const auto iterator = entry_index_by_id.constFind(parameter_id);
        if (iterator == entry_index_by_id.constEnd()) {
            return nullptr;
        }

        return &entries.at(iterator.value());
    }

    /**
     * @brief 按参数名称查找布局项。
     */
    const StreamLayoutEntry *findByName(const QString &parameter_name) const
    {
        const auto iterator = entry_index_by_name.constFind(parameter_name);
        if (iterator == entry_index_by_name.constEnd()) {
            return nullptr;
        }

        return &entries.at(iterator.value());
    }
};

/**
 * @brief 已解析且可编辑的配置文档。
 *
 * streams 是 ConfigManager 和 GUI 参数增删改查使用的标准编辑模型。layout_tables 是生成后的缓存，
 * 可在编辑后刷新并共享给任务执行代码。content_hash 用于让 ConfigManager 识别相同文件内容。
 */
struct Config
{
    QString source_path;
    ConfigHeader header;
    QList<StreamFormatDescription> streams;
    QList<StreamLayoutTable> layout_tables;
    QDateTime loaded_at;
    QString content_hash;

    /**
     * @brief 按流序号查找可编辑的流描述。
     */
    StreamFormatDescription *streamByIndex(int stream_index)
    {
        for (StreamFormatDescription &stream : streams) {
            if (stream.stream_index == stream_index) {
                return &stream;
            }
        }

        return nullptr;
    }

    /**
     * @brief 按流序号查找只读流描述。
     */
    const StreamFormatDescription *streamByIndex(int stream_index) const
    {
        for (const StreamFormatDescription &stream : streams) {
            if (stream.stream_index == stream_index) {
                return &stream;
            }
        }

        return nullptr;
    }

    /**
     * @brief 按流序号查找已生成的布局表。
     */
    const StreamLayoutTable *layoutByStreamIndex(int stream_index) const
    {
        for (const StreamLayoutTable &layout_table : layout_tables) {
            if (layout_table.stream_index == stream_index) {
                return &layout_table;
            }
        }

        return nullptr;
    }
};

using ConfigPtr = QSharedPointer<Config>;
using ConstConfigPtr = QSharedPointer<const Config>;

/**
 * @brief ConfigManager 使用的缓存记录，包含引用计数和文件身份信息。
 */
struct ConfigCacheEntry
{
    QString canonical_path;
    ConfigPtr config;
    QDateTime last_modified;
    qint64 file_size_bytes = 0;
    int reference_count = 0;
    QDateTime cached_at;
};

/**
 * @brief 用户为处理任务选择的时间范围。
 */
struct TaskTimeRange
{
    double start_seconds = 0.0;
    double end_seconds = -1.0;

    /**
     * @brief 当任务设置了明确结束时间时返回 true。
     */
    bool hasEnd() const
    {
        return end_seconds >= 0.0;
    }
};

/**
 * @brief 用户为单个流选择的参数组。
 */
struct ParameterGroupSelection
{
    QString group_id;
    QString display_name;
    int stream_index = 0;
    QList<QString> parameter_ids;
};

/**
 * @brief 匹配系统五元组的顶层任务定义。
 */
struct TaskDefinition
{
    QString task_id;
    QString config_file_path;
    QString raw_file_path;
    QList<ParameterGroupSelection> parameter_groups;
    TaskTimeRange time_range;
    double user_sample_rate_hz = 0.0;
    QString output_directory;
};

/**
 * @brief 已解算的工程值，携带任务和流信息用于结果路由。
 */
struct ParameterValue
{
    QString task_id;
    int stream_index = 0;
    QString parameter_id;
    QString parameter_name;
    double timestamp_seconds = 0.0;
    QVariant raw_value;
    QVariant value;
    QString unit;
};

Q_DECLARE_METATYPE(CalibrationType)
Q_DECLARE_METATYPE(ParameterCodeType)
Q_DECLARE_METATYPE(CalibrationPoint)
Q_DECLARE_METATYPE(CalibrationDefinition)
Q_DECLARE_METATYPE(ParameterWordSegment)
Q_DECLARE_METATYPE(ParameterDescription)
Q_DECLARE_METATYPE(ConfigHeader)
Q_DECLARE_METATYPE(StreamFormatDescription)
Q_DECLARE_METATYPE(StreamLayoutEntry)
Q_DECLARE_METATYPE(StreamLayoutTable)
Q_DECLARE_METATYPE(Config)
Q_DECLARE_METATYPE(ConfigPtr)
Q_DECLARE_METATYPE(ConstConfigPtr)
Q_DECLARE_METATYPE(ConfigCacheEntry)
Q_DECLARE_METATYPE(TaskTimeRange)
Q_DECLARE_METATYPE(ParameterGroupSelection)
Q_DECLARE_METATYPE(TaskDefinition)
Q_DECLARE_METATYPE(ParameterValue)
Q_DECLARE_METATYPE(QList<ParameterValue>)
