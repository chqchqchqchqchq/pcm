#include "processing_config_cache.h"
#include "processing_options_support.h"
#include "traditional_processing_controller.h"
#include <QMutexLocker>

ConstConfigPtr ProcessingConfigCache::load(const QString &path, QString *error)
{
    const QString normalized = ProcessingOptionsSupport::absolutePath(path);
    QMutexLocker lock(&mutex_);
    const auto fingerprint = ProcessingOptionsSupport::fingerprint(normalized);
    if (fingerprint.isEmpty()) {
        if (error) { *error = QStringLiteral("无法读取配置文件：%1").arg(normalized); }
        return {};
    }
    auto it = entries_.constFind(normalized);
    if (it != entries_.constEnd() && it->fingerprint == fingerprint) { return it->config; }
    Config config;
    if (!TraditionalProcessingController::loadConfig(normalized, &config, error)) { return {}; }
    if (ProcessingOptionsSupport::fingerprint(normalized) != fingerprint) {
        if (error) { *error = QStringLiteral("读取期间配置发生变化，请重试：%1").arg(normalized); }
        return {};
    }
    const ConstConfigPtr shared(new Config(config));
    entries_.insert(normalized, {fingerprint, shared});
    return shared;
}

void ProcessingConfigCache::clear()
{
    QMutexLocker lock(&mutex_);
    entries_.clear();
}
