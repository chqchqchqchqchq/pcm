#include "task_scheduler.h"

#include "logger.h"

#include <QDir>
#include <QFileInfo>
#include <QMutexLocker>
#include <QSet>
#include <QThread>

namespace {

void setError(QString *error_message, const QString &message)
{
    if (error_message != nullptr) {
        *error_message = message;
    }
}

} // namespace

struct TaskScheduler::ActiveTask
{
    TaskDefinition definition;
    QList<StreamDefinition> streams;
    StreamFrameQueues frame_queues;
    QThread *thread = nullptr;
    RawFileReader *reader = nullptr;
    bool failed = false;
    bool stop_requested = false;
    bool reader_finished = false;
    quint64 produced_frame_count = 0;
};

TaskScheduler::TaskScheduler(QObject *parent)
    : QObject(parent)
{
    qRegisterMetaType<TaskScheduler::TaskState>("TaskScheduler::TaskState");
    qRegisterMetaType<TaskDefinition>("TaskDefinition");
    qRegisterMetaType<StreamDefinition>("StreamDefinition");
    qRegisterMetaType<QList<StreamDefinition>>("QList<StreamDefinition>");
    qRegisterMetaType<StreamFrameQueues>("StreamFrameQueues");
    qRegisterMetaType<RawReadRange>("RawReadRange");
}

TaskScheduler::~TaskScheduler()
{
    requestStopAll();

    QList<ActiveTask *> active_tasks;
    {
        QMutexLocker locker(&mutex_);
        active_tasks = active_tasks_.values();
    }

    for (ActiveTask *active_task : active_tasks) {
        if (active_task == nullptr || active_task->reader == nullptr || active_task->reader_finished) {
            continue;
        }

        active_task->reader->requestStop();
    }

    for (ActiveTask *active_task : active_tasks) {
        if (active_task == nullptr || active_task->thread == nullptr) {
            continue;
        }

        active_task->thread->quit();
        active_task->thread->wait();
    }
}

void TaskScheduler::setMaxActiveTasks(int max_active_tasks)
{
    {
        QMutexLocker locker(&mutex_);
        max_active_tasks_ = qMax(1, max_active_tasks);
    }

    startNextTasks();
}

int TaskScheduler::maxActiveTasks() const
{
    QMutexLocker locker(&mutex_);
    return max_active_tasks_;
}

int TaskScheduler::activeTaskCount() const
{
    QMutexLocker locker(&mutex_);
    return active_reader_count_;
}

int TaskScheduler::pendingTaskCount() const
{
    QMutexLocker locker(&mutex_);
    return pending_tasks_.size();
}

void TaskScheduler::setFrameQueueMaxSize(int max_queue_size)
{
    QMutexLocker locker(&mutex_);
    frame_queue_max_size_ = max_queue_size;
}

void TaskScheduler::setReaderOptions(const RawFileReaderOptions &options)
{
    QMutexLocker locker(&mutex_);
    reader_options_ = options;
}

void TaskScheduler::registerConfigStreams(const QString &config_file_path, const QList<StreamDefinition> &streams)
{
    QMutexLocker locker(&mutex_);
    config_streams_.insert(normalizedConfigPath(config_file_path), streams);
}

void TaskScheduler::unregisterConfigStreams(const QString &config_file_path)
{
    QMutexLocker locker(&mutex_);
    config_streams_.remove(normalizedConfigPath(config_file_path));
}

void TaskScheduler::setStreamDefinitionProvider(const StreamDefinitionProvider &provider)
{
    QMutexLocker locker(&mutex_);
    stream_definition_provider_ = provider;
}

void TaskScheduler::setReadRangeResolver(const ReadRangeResolver &resolver)
{
    QMutexLocker locker(&mutex_);
    read_range_resolver_ = resolver;
}

bool TaskScheduler::submitTask(const TaskDefinition &definition, QString *error_message)
{
    QString local_error;
    if (!validateTaskDefinition(definition, &local_error)) {
        setError(error_message, local_error);
        return false;
    }

    {
        QMutexLocker locker(&mutex_);
        if (active_tasks_.contains(definition.task_id)) {
            setError(error_message,
                     QStringLiteral("Task '%1' is already active").arg(definition.task_id));
            return false;
        }

        for (const TaskDefinition &pending_task : pending_tasks_) {
            if (pending_task.task_id == definition.task_id) {
                setError(error_message,
                         QStringLiteral("Task '%1' is already pending").arg(definition.task_id));
                return false;
            }
        }

        pending_tasks_.enqueue(definition);
        idle_emitted_ = false;
    }

    log(LogLevel::Info, definition.task_id, QStringLiteral("Task submitted"));
    emit taskSubmitted(definition.task_id);
    emit stateChanged(definition.task_id, TaskState::Queued);
    startNextTasks();
    return true;
}

bool TaskScheduler::requestStop(const QString &task_id)
{
    ActiveTask *active_task = nullptr;
    bool stopped_pending = false;

    {
        QMutexLocker locker(&mutex_);
        for (int index = 0; index < pending_tasks_.size(); ++index) {
            if (pending_tasks_.at(index).task_id == task_id) {
                pending_tasks_.removeAt(index);
                stopped_pending = true;
                break;
            }
        }

        ActiveTask *candidate = active_tasks_.value(task_id, nullptr);
        if (candidate != nullptr && !candidate->reader_finished) {
            candidate->stop_requested = true;
            active_task = candidate;
        }
    }

    if (stopped_pending) {
        log(LogLevel::Warning, task_id, QStringLiteral("Pending task stopped before start"));
        emit stateChanged(task_id, TaskState::Stopped);
        emit taskStopped(task_id);
        emit taskFinished(task_id, 0);
        emitAllTasksFinishedIfIdle();
        return true;
    }

    if (active_task != nullptr && active_task->reader != nullptr) {
        log(LogLevel::Warning, task_id, QStringLiteral("Stop requested for active task"));
        emit stateChanged(task_id, TaskState::Stopping);
        active_task->reader->requestStop();
        return true;
    }

    return false;
}

void TaskScheduler::requestStopAll()
{
    QList<QString> stopped_pending_ids;
    QList<ActiveTask *> active_tasks;

    {
        QMutexLocker locker(&mutex_);
        while (!pending_tasks_.isEmpty()) {
            stopped_pending_ids.append(pending_tasks_.dequeue().task_id);
        }

        for (ActiveTask *active_task : active_tasks_) {
            if (active_task != nullptr && !active_task->reader_finished) {
                active_task->stop_requested = true;
                active_tasks.append(active_task);
            }
        }
    }

    for (const QString &task_id : stopped_pending_ids) {
        log(LogLevel::Warning, task_id, QStringLiteral("Pending task stopped before start"));
        emit stateChanged(task_id, TaskState::Stopped);
        emit taskStopped(task_id);
        emit taskFinished(task_id, 0);
    }

    for (ActiveTask *active_task : active_tasks) {
        if (active_task == nullptr || active_task->reader == nullptr) {
            continue;
        }

        emit stateChanged(active_task->definition.task_id, TaskState::Stopping);
        active_task->reader->requestStop();
    }

    emitAllTasksFinishedIfIdle();
}

bool TaskScheduler::hasTask(const QString &task_id) const
{
    QMutexLocker locker(&mutex_);
    if (active_tasks_.contains(task_id)) {
        return true;
    }

    for (const TaskDefinition &pending_task : pending_tasks_) {
        if (pending_task.task_id == task_id) {
            return true;
        }
    }

    return false;
}

bool TaskScheduler::validateTaskDefinition(const TaskDefinition &definition, QString *error_message) const
{
    if (definition.task_id.trimmed().isEmpty()) {
        setError(error_message, QStringLiteral("TaskDefinition.task_id is empty"));
        return false;
    }

    if (definition.config_file_path.trimmed().isEmpty()) {
        setError(error_message,
                 QStringLiteral("Task '%1' has an empty config_file_path").arg(definition.task_id));
        return false;
    }

    if (definition.raw_file_path.trimmed().isEmpty()) {
        setError(error_message,
                 QStringLiteral("Task '%1' has an empty raw_file_path").arg(definition.task_id));
        return false;
    }

    if (definition.user_sample_rate_hz < 0.0) {
        setError(error_message,
                 QStringLiteral("Task '%1' user_sample_rate_hz must be >= 0").arg(definition.task_id));
        return false;
    }

    return true;
}

bool TaskScheduler::resolveStreamDefinitions(const TaskDefinition &definition,
                                             QList<StreamDefinition> *streams,
                                             QString *error_message) const
{
    if (streams == nullptr) {
        setError(error_message, QStringLiteral("streams output pointer is null"));
        return false;
    }

    StreamDefinitionProvider provider;
    {
        QMutexLocker locker(&mutex_);
        provider = stream_definition_provider_;
    }

    if (provider) {
        return provider(definition, streams, error_message);
    }

    const QString config_key = normalizedConfigPath(definition.config_file_path);
    {
        QMutexLocker locker(&mutex_);
        const auto iterator = config_streams_.constFind(config_key);
        if (iterator != config_streams_.constEnd()) {
            *streams = iterator.value();
            return true;
        }
    }

    setError(error_message,
             QStringLiteral("No stream definitions registered for config '%1'").arg(definition.config_file_path));
    return false;
}

bool TaskScheduler::resolveReadRange(const TaskDefinition &definition,
                                     RawReadRange *range,
                                     QString *error_message) const
{
    if (range == nullptr) {
        setError(error_message, QStringLiteral("range output pointer is null"));
        return false;
    }

    ReadRangeResolver resolver;
    {
        QMutexLocker locker(&mutex_);
        resolver = read_range_resolver_;
    }

    if (resolver) {
        return resolver(definition, range, error_message);
    }

    Q_UNUSED(definition)
    range->start_byte_offset = 0;
    range->end_byte_offset = -1;
    return true;
}

bool TaskScheduler::buildRawReadTask(const TaskDefinition &definition,
                                     const QList<StreamDefinition> &streams,
                                     const RawReadRange &range,
                                     RawReadTask *raw_task,
                                     StreamFrameQueues *frame_queues,
                                     QString *error_message) const
{
    if (raw_task == nullptr || frame_queues == nullptr) {
        setError(error_message, QStringLiteral("raw task output pointer is null"));
        return false;
    }

    if (streams.isEmpty()) {
        setError(error_message,
                 QStringLiteral("Task '%1' has no stream definitions").arg(definition.task_id));
        return false;
    }

    int queue_max_size = 0;
    {
        QMutexLocker locker(&mutex_);
        queue_max_size = frame_queue_max_size_;
    }

    QSet<QString> stream_ids;
    StreamFrameQueues queues;
    for (const StreamDefinition &stream : streams) {
        if (stream.stream_id.trimmed().isEmpty()) {
            setError(error_message,
                     QStringLiteral("Task '%1' contains a stream with an empty stream_id").arg(definition.task_id));
            return false;
        }

        if (stream_ids.contains(stream.stream_id)) {
            setError(error_message,
                     QStringLiteral("Task '%1' contains duplicate stream_id '%2'")
                         .arg(definition.task_id, stream.stream_id));
            return false;
        }

        stream_ids.insert(stream.stream_id);
        queues.insert(stream.stream_id, FrameTaskQueuePtr::create(queue_max_size));
    }

    RawReadTask generated_task;
    generated_task.task_id = definition.task_id;
    generated_task.raw_file_path = definition.raw_file_path;
    generated_task.sample_rate_hz = definition.user_sample_rate_hz;
    generated_task.range = range;
    generated_task.streams = streams;
    generated_task.frame_queues = queues;

    *raw_task = generated_task;
    *frame_queues = queues;
    return true;
}

bool TaskScheduler::startTask(const TaskDefinition &definition, QString *error_message)
{
    QList<StreamDefinition> streams;
    if (!resolveStreamDefinitions(definition, &streams, error_message)) {
        return false;
    }

    RawReadRange range;
    if (!resolveReadRange(definition, &range, error_message)) {
        return false;
    }

    RawReadTask raw_task;
    StreamFrameQueues frame_queues;
    if (!buildRawReadTask(definition, streams, range, &raw_task, &frame_queues, error_message)) {
        return false;
    }

    RawFileReaderOptions reader_options;
    {
        QMutexLocker locker(&mutex_);
        reader_options = reader_options_;
    }

    ActiveTask *active_task = new ActiveTask;
    active_task->definition = definition;
    active_task->streams = streams;
    active_task->frame_queues = frame_queues;
    active_task->thread = new QThread;
    active_task->reader = new RawFileReader(raw_task, reader_options);
    active_task->thread->setObjectName(QStringLiteral("RawFileReader-%1").arg(definition.task_id));
    active_task->reader->moveToThread(active_task->thread);

    {
        QMutexLocker locker(&mutex_);
        if (active_tasks_.contains(definition.task_id)) {
            active_task->reader->deleteLater();
            active_task->thread->deleteLater();
            delete active_task;
            setError(error_message,
                     QStringLiteral("Task '%1' became active before it could be started").arg(definition.task_id));
            return false;
        }

        active_tasks_.insert(definition.task_id, active_task);
        ++active_reader_count_;
    }

    connect(active_task->thread, &QThread::started, active_task->reader, &RawFileReader::readFrames);
    connect(active_task->reader, &RawFileReader::started, this, [this](const QString &task_id) {
        emit stateChanged(task_id, TaskState::Running);
        emit taskStarted(task_id);
    });
    connect(active_task->reader,
            &RawFileReader::progressChanged,
            this,
            &TaskScheduler::taskProgressChanged);
    connect(active_task->reader, &RawFileReader::failed, this, &TaskScheduler::handleReaderFailed);
    connect(active_task->reader, &RawFileReader::finished, this, &TaskScheduler::handleReaderFinished);
    connect(active_task->reader, &RawFileReader::finished, active_task->thread, &QThread::quit);
    connect(active_task->reader, &RawFileReader::finished, active_task->reader, &QObject::deleteLater);
    connect(active_task->reader, &QObject::destroyed, this, [active_task]() {
        active_task->reader = nullptr;
    });
    connect(active_task->thread, &QThread::finished, active_task->thread, &QObject::deleteLater);
    connect(active_task->thread, &QThread::finished, this, [this, definition, active_task]() {
        handleThreadFinished(definition.task_id, active_task);
    });

    log(LogLevel::Info, definition.task_id, QStringLiteral("Task activated"));
    emit taskQueuesReady(definition, streams, frame_queues);
    emit activeTaskCountChanged(activeTaskCount());
    active_task->thread->start();
    return true;
}

void TaskScheduler::startNextTasks()
{
    while (true) {
        TaskDefinition next_definition;
        {
            QMutexLocker locker(&mutex_);
            if (pending_tasks_.isEmpty() || active_reader_count_ >= max_active_tasks_) {
                break;
            }

            next_definition = pending_tasks_.dequeue();
        }

        QString error_message;
        if (!startTask(next_definition, &error_message)) {
            log(LogLevel::Error, next_definition.task_id, error_message);
            emit taskFailed(next_definition.task_id, error_message);
            emit stateChanged(next_definition.task_id, TaskState::Failed);
            emit taskFinished(next_definition.task_id, 0);
        }
    }

    emitAllTasksFinishedIfIdle();
}

void TaskScheduler::handleReaderFailed(const QString &task_id, const QString &reason)
{
    {
        QMutexLocker locker(&mutex_);
        ActiveTask *active_task = active_tasks_.value(task_id, nullptr);
        if (active_task != nullptr) {
            active_task->failed = true;
        }
    }

    log(LogLevel::Error, task_id, reason);
    emit taskFailed(task_id, reason);
    emit stateChanged(task_id, TaskState::Failed);
}

void TaskScheduler::handleReaderFinished(const QString &task_id, quint64 produced_frame_count)
{
    bool failed = false;
    bool stopped = false;
    bool known_task = false;
    int active_count = 0;

    {
        QMutexLocker locker(&mutex_);
        ActiveTask *active_task = active_tasks_.value(task_id, nullptr);
        if (active_task != nullptr && !active_task->reader_finished) {
            active_task->reader_finished = true;
            active_task->produced_frame_count = produced_frame_count;
            failed = active_task->failed;
            stopped = active_task->stop_requested && !active_task->failed;
            known_task = true;
            if (active_reader_count_ > 0) {
                --active_reader_count_;
            }
            active_count = active_reader_count_;
        }
    }

    if (!known_task) {
        return;
    }

    if (stopped) {
        emit taskStopped(task_id);
        emit stateChanged(task_id, TaskState::Stopped);
    } else if (!failed) {
        emit stateChanged(task_id, TaskState::Finished);
    }

    emit taskFinished(task_id, produced_frame_count);
    emit activeTaskCountChanged(active_count);
    startNextTasks();
}

void TaskScheduler::handleThreadFinished(const QString &task_id, ActiveTask *active_task)
{
    {
        QMutexLocker locker(&mutex_);
        active_tasks_.remove(task_id);
    }

    delete active_task;
    emitAllTasksFinishedIfIdle();
}

void TaskScheduler::emitAllTasksFinishedIfIdle()
{
    bool should_emit = false;
    {
        QMutexLocker locker(&mutex_);
        if (!idle_emitted_ && pending_tasks_.isEmpty() && active_reader_count_ == 0 && active_tasks_.isEmpty()) {
            idle_emitted_ = true;
            should_emit = true;
        }
    }

    if (should_emit) {
        emit allTasksFinished();
    }
}

void TaskScheduler::log(LogLevel level, const QString &task_id, const QString &message) const
{
    Logger::instance().log(level, QStringLiteral("TaskScheduler"), task_id, QString(), message);
}

QString TaskScheduler::normalizedConfigPath(const QString &path)
{
    const QFileInfo file_info(path);
    if (file_info.exists()) {
        return QDir::cleanPath(file_info.canonicalFilePath());
    }

    return QDir::cleanPath(file_info.absoluteFilePath());
}
