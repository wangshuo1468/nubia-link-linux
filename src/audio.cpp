#include "audio.h"
#include <QAudioDevice>
#include <QMediaDevices>
#include <QMutexLocker>
#include <cstring>

PcmBuffer::PcmBuffer(QObject *parent) : QIODevice(parent) { open(QIODevice::ReadOnly | QIODevice::Unbuffered); }
void PcmBuffer::append(const QByteArray &bytes) {
    {
        QMutexLocker lock(&mutex_); data_ += bytes;
        if (data_.size() > Capacity) data_.remove(0, ((data_.size() - Capacity + 3) / 4) * 4);
    }
    emit readyRead();
}
void PcmBuffer::clear() { QMutexLocker lock(&mutex_); data_.clear(); }
qint64 PcmBuffer::bytesAvailable() const { QMutexLocker lock(&mutex_); return data_.size() + QIODevice::bytesAvailable(); }
qint64 PcmBuffer::readData(char *out, qint64 length) {
    QMutexLocker lock(&mutex_);
    const auto count = qMin(length, qint64(data_.size())) / 4 * 4;
    std::memcpy(out, data_.constData(), size_t(count)); data_.remove(0, count); return count;
}
PhoneAudio::PhoneAudio(QObject *parent) : QObject(parent) {}
void PhoneAudio::start() {
    stop();
    QAudioFormat format; format.setSampleRate(48000); format.setChannelConfig(QAudioFormat::ChannelConfigStereo); format.setSampleFormat(QAudioFormat::Int16);
    const auto device = QMediaDevices::defaultAudioOutput();
    if (device.isNull() || !device.isFormatSupported(format)) {
        emit warning(QStringLiteral("当前音频设备不支持 48 kHz 双声道 PCM，声音未开启")); return;
    }
    sink_ = std::make_unique<QAudioSink>(device, format);
    sink_->setBufferSize(48000 * 4 / 10);
    sink_->setVolume(muted_ ? 0 : 1);
    connect(sink_.get(), &QAudioSink::stateChanged, this, [this](QtAudio::State state) {
        if (sink_ && state == QtAudio::StoppedState && sink_->error() != QtAudio::NoError)
            emit warning(QStringLiteral("声音播放失败，请检查系统音频输出"));
    });
    sink_->start(&buffer_);
}
void PhoneAudio::stop() { if (sink_) { sink_->disconnect(this); sink_->stop(); sink_.reset(); } buffer_.clear(); }
void PhoneAudio::append(const QByteArray &bytes) { if (sink_) buffer_.append(bytes); }
void PhoneAudio::setMuted(bool muted) { muted_ = muted; if (sink_) sink_->setVolume(muted ? 0 : 1); }
