#pragma once
#include <QIODevice>
#include <QAudioSink>
#include <QMutex>
#include <memory>

class PcmBuffer : public QIODevice {
public:
    explicit PcmBuffer(QObject *parent = nullptr);
    void append(const QByteArray &bytes);
    void clear();
    qint64 bytesAvailable() const override;
    bool isSequential() const override { return true; }
    bool atEnd() const override { return false; }
    static constexpr qsizetype Capacity = 48000 * 4 / 4; // 250 ms, whole stereo frames
protected:
    qint64 readData(char *data, qint64 length) override;
    qint64 writeData(const char *, qint64) override { return -1; }
private:
    QByteArray data_;
    mutable QMutex mutex_;
};

class PhoneAudio : public QObject {
    Q_OBJECT
public:
    explicit PhoneAudio(QObject *parent = nullptr);
    void start();
    void stop();
    void append(const QByteArray &bytes);
    void setMuted(bool muted);
signals:
    void warning(QString text);
private:
    PcmBuffer buffer_;
    std::unique_ptr<QAudioSink> sink_;
    bool muted_ = false;
};
