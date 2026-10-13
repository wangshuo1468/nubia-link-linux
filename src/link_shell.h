#pragma once
#include <QWidget>
#include <QHostAddress>
#include <QIcon>
#include <QSizePolicy>
#include <QPointer>

class QLabel;
class QPushButton;
class QVBoxLayout;
class QHBoxLayout;
class QStackedLayout;

// Shared native layout; media and protocol ownership stays with each window.
class LinkShell : public QWidget {
    Q_OBJECT
public:
    struct Interface { QHostAddress address; int prefix=0; QString name; };
    static Interface receiverInterface();
    explicit LinkShell(bool collaboration, QWidget *parent=nullptr);
    void setReceiving(bool enabled);
    void setMediaActive(bool active);
    void setMessageControls(QWidget *widget);
    void setWaitingVisible(bool visible);
    void setFrameSize(QSize size);
    bool focused() const { return focused_; }
    static QIcon icon(const QString &name);
    QWidget *sidebar, *advanced, *stage;
    QVBoxLayout *advancedLayout, *contentLayout;
    QHBoxLayout *controls;
    QLabel *placeholder, *status, *connectionState, *receiverState, *computerName, *help;
    QPushButton *files, *receiveButton;
signals:
    void modeRequested(bool collaboration);
private:
    void resizeEvent(QResizeEvent *event) override;
    bool eventFilter(QObject *object,QEvent *event) override;
    void arrangeMessageControls();
    void setFocusMode(bool enabled);
    void resizeFocusWindow();
    void constrainFocusSize();
    void focusScreen();
    Qt::Edges resizeEdges(QPoint position) const;
    QStackedLayout *pages_;
    QWidget *normalPage_, *focusPage_;
    QVBoxLayout *focusLayout_;
    bool focused_=false, waitingVisible_=true;
    Qt::WindowStates previousWindowState_;
    QRect previousGeometry_;
    Qt::WindowFlags previousWindowFlags_;
    QSize previousMinimumSize_, previousStageMinimum_, frameSize_{1080,2400};
    QSizePolicy previousCentralPolicy_;
    QPointer<QWidget> screenInput_;
    bool previousInputTracking_=false, previousStageTracking_=false;
    bool constrainPending_=false;
    bool collaboration_, receiving_=false;
    QWidget *incomingPanel_;
    QWidget *steps_;
    QWidget *message_=nullptr;
    QPushButton *advancedToggle_, *focus_;
};
