#pragma once
#include <QByteArray>
#include <QString>
#include <QTextCodec>

/** @brief UTF-8 优先，旧 Windows CFTE 文件按 GB18030 解码，避免依赖系统区域设置。 */
inline QString decodeConfigText(const QByteArray &bytes)
{
    QString text = QString::fromUtf8(bytes);
    if (text.contains(QChar::ReplacementCharacter)) {
        QTextCodec *codec = QTextCodec::codecForName("GB18030");
        if (codec != nullptr) {
            text = codec->toUnicode(bytes);
        }
    }
    return text;
}
