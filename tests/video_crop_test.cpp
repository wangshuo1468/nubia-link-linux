#include "video_crop.h"
#include "decoder.h"
#include <QtTest>
#include <QTemporaryDir>
#include <QFile>
extern "C" {
#include <libavutil/frame.h>
#include <libavutil/pixfmt.h>
#include <libavutil/log.h>
}

class VideoCropTest:public QObject {
    Q_OBJECT
    static AVFrame *picture(QSize size,QRect content,bool fullRange=false) {
        auto frame=av_frame_alloc(); frame->width=size.width(); frame->height=size.height();
        frame->format=AV_PIX_FMT_YUV420P; frame->color_range=fullRange ? AVCOL_RANGE_JPEG : AVCOL_RANGE_MPEG;
        if(av_frame_get_buffer(frame,32)<0) { av_frame_free(&frame); return nullptr; }
        for(int row=0;row<size.height();++row) {
            memset(frame->data[0]+row*frame->linesize[0],fullRange ? 0 : 16,size.width());
            if(row>=content.top() && row<=content.bottom()) memset(frame->data[0]+row*frame->linesize[0]+content.x(),81,content.width());
        }
        for(int p=1;p<3;++p) for(int row=0;row<size.height()/2;++row) memset(frame->data[p]+row*frame->linesize[p],128,size.width()/2);
        return frame;
    }
private slots:
    void initTestCase() { av_log_set_level(AV_LOG_WARNING); }
    void bordersRotationBlankAndAsymmetric() {
        for(bool fullRange:{false,true}) {
            VideoCropDetector detector;
            const QSize canvas(320,180); const QRect full(QPoint(),canvas), portrait(124,0,72,180);
            auto frame=picture(canvas,portrait,fullRange); QVERIFY(frame);
            QRect result;
            for(int i=0;i<20;++i) result=detector.detect(frame);
            QCOMPARE(result,portrait); av_frame_free(&frame);
            frame=picture(canvas,{},fullRange);
            for(int i=0;i<8;++i) QCOMPARE(detector.detect(frame),portrait);
            av_frame_free(&frame);
            // Same encoded canvas, with a stable landscape phone boundary.
            const QRect letterbox(0,30,320,120);
            frame=picture(canvas,letterbox,fullRange);
            for(int i=0;i<20;++i) result=detector.detect(frame);
            QCOMPARE(result,letterbox); av_frame_free(&frame);
            // Landscape phone: encoded top/bottom padding plus a dark header.
            frame=picture(canvas,QRect(0,50,320,100),fullRange);
            for(int i=0;i<8;++i) result=detector.detect(frame);
            QCOMPARE(result,QRect(0,30,320,120)); av_frame_free(&frame);
            // A brighter page and a darker page retain the landscape border.
            for(const auto content:{full,QRect(0,60,320,60)}) {
                frame=picture(canvas,content,fullRange);
                for(int i=0;i<60;++i) QCOMPARE(detector.detect(frame),letterbox);
                av_frame_free(&frame);
            }
            frame=picture(canvas,portrait,fullRange);
            for(int i=0;i<20;++i) result=detector.detect(frame);
            QCOMPARE(result,portrait); av_frame_free(&frame);
            // A different sized content canvas resets all previous boundaries.
            frame=picture({180,320},QRect(0,0,180,320),fullRange);
            QCOMPARE(detector.detect(frame),QRect(0,0,180,320)); av_frame_free(&frame);
            QVERIFY(!detector.ready());
            VideoCropDetector asymmetric;
            frame=picture(canvas,QRect(80,0,240,180),fullRange);
            for(int i=0;i<8;++i) QCOMPARE(asymmetric.detect(frame),full);
            av_frame_free(&frame);
        }
    }
    void darkPagesTransientRotationAndCachedBoundaries() {
        VideoCropDetector detector;
        const QSize canvas(320,180); const QRect portrait(124,0,72,180), landscape(0,30,320,120);
        auto feed=[&](QRect content,int count,QRect expected) {
            auto frame=picture(canvas,content); QVERIFY(frame);
            for(int i=0;i<count;++i) QCOMPARE(detector.detect(frame),expected);
            av_frame_free(&frame);
        };
        auto frame=picture(canvas,portrait);
        for(int i=0;i<20;++i) detector.detect(frame);
        av_frame_free(&frame); QVERIFY(detector.ready());
        feed(QRect(128,0,64,180),90,portrait); // Narrower dark portrait page.
        feed(QRect(140,60,40,60),90,portrait); // Bright central dialog.
        feed(landscape,14,portrait); // A short animation is not a rotation.
        feed({},5,portrait); // Blank frames break the confirmation run.
        feed(landscape,14,portrait);
        frame=picture(canvas,landscape); QCOMPARE(detector.detect(frame),landscape); av_frame_free(&frame);
        feed(QRect(0,50,320,80),90,landscape); // Symmetric dark landscape page.
        frame=picture(canvas,QRect(128,0,64,180));
        for(int i=0;i<20;++i) detector.detect(frame);
        QCOMPARE(detector.detect(frame),portrait); av_frame_free(&frame);
        // A different page when rotating back must reuse the learned border.
        frame=picture(canvas,QRect(0,50,320,80));
        for(int i=0;i<20;++i) detector.detect(frame);
        QCOMPARE(detector.detect(frame),landscape); av_frame_free(&frame);
        // Color metadata changes reconfigure FFmpeg, not the screen boundary.
        frame=picture(canvas,QRect(0,50,320,80),true);
        QCOMPARE(detector.detect(frame),landscape); QVERIFY(detector.ready()); av_frame_free(&frame);
    }
    void calibrationCollectsOutermostContent() {
        VideoCropDetector detector; const QSize canvas(320,180);
        for(int i=0;i<24;++i) {
            auto frame=picture(canvas,i%2 ? QRect(124,0,72,180) : QRect(128,0,64,180));
            const auto crop=detector.detect(frame);
            if(detector.ready()) QCOMPARE(crop,QRect(124,0,72,180));
            av_frame_free(&frame);
        }
        QVERIFY(detector.ready());
    }
    void realPaddedH264AndSessionIsolation() {
        QTemporaryDir dir; QProcess encoder;
        encoder.start("ffmpeg",{"-v","error","-f","lavfi","-i","color=c=red:s=72x180:r=30,pad=320:180:124:0:black",
            "-frames:v","24","-c:v","libx264","-preset","ultrafast","-tune","zerolatency","-f","h264",dir.filePath("padded.h264")});
        QVERIFY(encoder.waitForFinished(10000)); QCOMPARE(encoder.exitCode(),0);
        QFile file(dir.filePath("padded.h264")); QVERIFY(file.open(QIODevice::ReadOnly)); const auto data=file.readAll();
        // Feed one access unit at a time, preserving FFmpeg's packet boundary.
        QProcess demux;
        demux.start("ffprobe",{"-v","error","-show_entries","packet=pos,size","-of","csv=p=0",dir.filePath("padded.h264")});
        QVERIFY(demux.waitForFinished(5000)); QCOMPARE(demux.exitCode(),0);
        H264Decoder plain,trimmed(true); QString error;
        std::shared_ptr<VideoFrame> plainFrame,croppedFrame;
        QByteArray firstPacket;
        int frames=0;
        for(const auto &line:demux.readAllStandardOutput().trimmed().split('\n')) {
            const auto fields=line.split(','); QCOMPARE(fields.size(),2);
            const auto packet=data.mid(fields[1].toLongLong(),fields[0].toInt());
            if(firstPacket.isEmpty()) firstPacket=packet;
            QVERIFY2(plain.decode(packet,frames,[&](auto f){plainFrame=f;},error),qPrintable(error));
            QVERIFY2(trimmed.decode(packet,frames++,[&](auto f){croppedFrame=f;},error),qPrintable(error));
        }
        QVERIFY(plainFrame && croppedFrame); QCOMPARE(plainFrame->displaySize(),QSize(320,180));
        QCOMPARE(croppedFrame->visibleRect(),QRect(124,0,72,180)); QCOMPARE(croppedFrame->size,QSize(320,180));
        H264Decoder fresh(true); std::shared_ptr<VideoFrame> reset;
        // A fresh session waits for calibration, without flashing full bounds
        // or inheriting the previous connection's crop.
        QVERIFY(fresh.decode(firstPacket,0,[&](auto f){reset=f;},error));
        QVERIFY(!reset);
    }
};
QTEST_GUILESS_MAIN(VideoCropTest)
#include "video_crop_test.moc"
