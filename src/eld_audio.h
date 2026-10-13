#pragma once
#include <QByteArray>
#include <memory>

// Raw AAC-ELD access units used by the native Lelink v1 sender.
// Output matches PhoneAudio: 48 kHz stereo, signed little-endian PCM16.
class EldAudio {
public:
    EldAudio();
    ~EldAudio();
    bool configure(const QByteArray &sdp);
    QByteArray decode(const QByteArray &packet);
    void reset();
    bool ready() const;
private:
    struct State;
    std::unique_ptr<State> state_;
};
