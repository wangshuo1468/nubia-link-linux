#include "protocol.h"
#include <QtEndian>

namespace {
template<class T> void append(QByteArray &out, T value) {
    value = qToBigEndian(value);
    out.append(reinterpret_cast<const char *>(&value), sizeof(value));
}
void position(QByteArray &out, QPoint point, QSize size) {
    append(out, quint32(point.x())); append(out, quint32(point.y()));
    append(out, quint16(size.width())); append(out, quint16(size.height()));
}
QByteArray utf8(const QString &value, qsizetype limit) {
    QByteArray out = value.toUtf8();
    if (out.size() > limit) {
        qsizetype end = limit;
        while (end > 0 && (quint8(out[end]) & 0xc0) == 0x80) --end;
        out.truncate(end);
    }
    return out;
}
}

QByteArray Nubia::text(quint8 type, const QString &value, qsizetype limit) {
    QByteArray out(1, char(type));
    auto bytes = utf8(value, qBound(qsizetype(0), limit, MaxText));
    append(out, quint16(bytes.size())); out += bytes;
    return out;
}
QByteArray Nubia::key(quint8 action, quint32 code, quint32 modifiers) {
    QByteArray out(1, char(0)); out += char(action);
    append(out, code); append(out, modifiers); return out;
}
QByteArray Nubia::touch(quint8 action, QPoint point, QSize screen, quint16 pressure) {
    QByteArray out(1, char(2)); out += char(action);
    append(out, quint64(-1)); position(out, point, screen);
    append(out, pressure); append(out, quint32(action == 1 ? 0 : 1)); return out;
}
QByteArray Nubia::scroll(QPoint point, QSize screen, qint32 horizontal, qint32 vertical) {
    QByteArray out(1, char(3)); position(out, point, screen);
    append(out, quint32(horizontal)); append(out, quint32(vertical)); return out;
}
QByteArray Nubia::clipboard(const QString &value, bool paste) {
    auto out = text(8, value); out.insert(1, char(paste ? 1 : 0)); return out;
}
void VideoFramer::reset() { buffer_.clear(); failed_ = false; }
void VideoFramer::feed(const QByteArray &bytes) {
    if (failed_) return;
    buffer_ += bytes;
    while (buffer_.size() >= 12) {
        auto data = reinterpret_cast<const uchar *>(buffer_.constData());
        auto size = qFromBigEndian<quint32>(data + 8);
        if (size == 0 || size > Nubia::MaxVideoPacket) {
            failed_ = true; buffer_.clear(); emit error(QStringLiteral("画面数据包长度无效")); return;
        }
        if (buffer_.size() < 12 + qsizetype(size)) return;
        qint64 timestamp = qint64(qFromBigEndian<quint64>(data));
        auto payload = buffer_.mid(12, size);
        buffer_.remove(0, 12 + size);
        emit packet(payload, timestamp);
        if (failed_) return;
    }
}
void MessageFramer::reset() { buffer_.clear(); failed_ = false; }
void MessageFramer::feed(const QByteArray &bytes) {
    if (failed_) return;
    buffer_ += bytes;
    while (buffer_.size() >= 3) {
        quint8 type = quint8(buffer_[0]);
        quint16 size = qFromBigEndian<quint16>(reinterpret_cast<const uchar *>(buffer_.constData() + 1));
        if (type > 7 || size > Nubia::MaxText) {
            failed_ = true; buffer_.clear(); emit error(QStringLiteral("手机通知格式不兼容")); return;
        }
        if (buffer_.size() < 3 + size) return;
        auto value = QString::fromUtf8(buffer_.constData() + 3, size);
        buffer_.remove(0, 3 + size); emit message(type, value);
        if (failed_) return;
    }
}
