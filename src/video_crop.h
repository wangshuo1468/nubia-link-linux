#pragma once
#include <QRect>
#include <memory>
struct AVFrame;

// FFmpeg identifies black padding. Learn and retain one boundary per direction;
// sustained rotation evidence switches between them, not dark page contents.
// Owned by the decoder thread, and reset with every new video session.
class VideoCropDetector {
public:
    VideoCropDetector();
    ~VideoCropDetector();
    QRect detect(AVFrame *frame);
    bool ready() const;
private:
    struct Impl;
    std::unique_ptr<Impl> impl_;
};
