#include "link_shell.h"
#include <QButtonGroup>
#include <QFrame>
#include <QNetworkInterface>
#include <QLabel>
#include <QPainter>
#include <QPushButton>
#include <QResizeEvent>
#include <QScrollArea>
#include <QShortcut>
#include <QStackedLayout>
#include <QScreen>
#include <QWindow>
#include <QMainWindow>
#include <QApplication>
#include <QMouseEvent>
#include <QTimer>
#include <QVBoxLayout>

namespace {
QLabel *label(QString text, QWidget *parent=nullptr, const char *name=nullptr) {
    auto result=new QLabel(std::move(text),parent); result->setWordWrap(true);
    if(name) result->setObjectName(name);
    result->setSizePolicy(QSizePolicy::Ignored,QSizePolicy::Preferred);
    return result;
}
// Drawn locally at device resolution; no fonts, image downloads or SVG module needed.
QPixmap waitingIllustration(bool collaboration, qreal ratio) {
    QPixmap image(QSize(184,128)*ratio); image.setDevicePixelRatio(ratio); image.fill(Qt::transparent);
    QPainter p(&image); p.setRenderHint(QPainter::Antialiasing);
    p.setPen(QPen(QColor("#adb1bd"),1.8)); p.setBrush(QColor("#f4f4f7"));
    const int monitorX=collaboration ? 14 : 34;
    p.drawRoundedRect(QRectF(monitorX,10,136,82),6,6);
    p.drawLine(monitorX+14,79,monitorX+122,79); p.drawLine(monitorX+65,92,monitorX+65,108); p.drawLine(monitorX+39,108,monitorX+91,108);
    if(collaboration) {
        p.setPen(QPen(QColor("#8993b6"),1.8));
        p.drawLine(79,37,115,37); p.drawLine(109,31,115,37); p.drawLine(109,43,115,37);
        p.drawLine(79,51,115,51); p.drawLine(79,51,85,45); p.drawLine(79,51,85,57);
    } else {
        p.setPen(QPen(QColor("#8993b6"),1.8));
        p.drawArc(QRectF(79,31,40,30),45*16,90*16); p.drawArc(QRectF(87,39,24,20),45*16,90*16);
        p.setBrush(QColor("#8993b6")); p.drawEllipse(QPointF(99,55),1.5,1.5);
    }
    p.setPen(QPen(QColor("#788198"),1.8)); p.setBrush(QColor("#fafafa"));
    const int phoneX=collaboration ? 128 : 14;
    p.drawRoundedRect(QRectF(phoneX,36,46,78),7,7); p.drawLine(phoneX+16,44,phoneX+30,44); p.drawLine(phoneX+18,106,phoneX+28,106);
    return image;
}
QString paths(const QString &name) {
    if(name=="cast") return "<rect x='3' y='3' width='18' height='14' rx='2'/><path d='M3 21h.01M3 17a4 4 0 0 1 4 4M3 13a8 8 0 0 1 8 8'/>";
    if(name=="panels") return "<rect x='3' y='3' width='18' height='18' rx='2'/><path d='M3 9h18M9 9v12'/>";
    if(name=="files") return "<path d='M20 20H4a2 2 0 0 1-2-2V6a2 2 0 0 1 2-2h5l2 3h9a2 2 0 0 1 2 2v9a2 2 0 0 1-2 2ZM12 17v-6m-3 3 3-3 3 3'/>";
    if(name=="settings") return "<path d='M3 7h5m4 0h9M3 17h9m4 0h5'/><circle cx='10' cy='7' r='2'/><circle cx='14' cy='17' r='2'/>";
    if(name=="help") return "<circle cx='12' cy='12' r='9'/><path d='M9 9a3 3 0 0 1 6 0c0 2-3 2-3 4m0 3h.01'/>";
    if(name=="focus") return "<path d='M8 3H3v5m13-5h5v5M3 16v5h5m8 0h5v-5'/>";
    if(name=="pause") return "<rect x='6' y='4' width='4' height='16' rx='1'/><rect x='14' y='4' width='4' height='16' rx='1'/>";
    if(name=="play") return "<path d='m7 4 14 8-14 8Z'/>";
    if(name=="back") return "<path d='M20 12H4m6-6-6 6 6 6'/>";
    if(name=="home") return "<path d='m3 10 9-7 9 7v10a1 1 0 0 1-1 1h-5v-8H9v8H4a1 1 0 0 1-1-1Z'/>";
    if(name=="recent") return "<rect x='3' y='3' width='12' height='12' rx='2'/><path d='M9 19v-4m6-6h4a2 2 0 0 1 2 2v8a2 2 0 0 1-2 2h-8a2 2 0 0 1-2-2'/>";
    if(name=="app") return "<rect x='3' y='3' width='18' height='18' rx='2'/><path d='M3 8h18M7 5.5h.01m3 0h.01'/>";
    if(name=="volume") return "<path d='m11 4-6 5H2v6h3l6 5ZM15 8a6 6 0 0 1 0 8m3-11a10 10 0 0 1 0 14'/>";
    if(name=="muted") return "<path d='m11 4-6 5H2v6h3l6 5Zm6 5 5 6m0-6-5 6'/>";
    return "<circle cx='5' cy='12' r='1'/><circle cx='12' cy='12' r='1'/><circle cx='19' cy='12' r='1'/>";
}
}
QIcon LinkShell::icon(const QString &name) {
    QIcon result;
    for(auto mode:{QIcon::Normal,QIcon::Disabled}) {
        const QString svg=QString("<svg xmlns='http://www.w3.org/2000/svg' width='40' height='40' viewBox='0 0 24 24' fill='none' stroke='%1' stroke-width='1.7' stroke-linecap='round' stroke-linejoin='round'>%2</svg>")
            .arg(mode==QIcon::Disabled ? "#b7bdca" : "#647089",paths(name));
        QPixmap pixmap; pixmap.loadFromData(svg.toUtf8(),"SVG"); result.addPixmap(pixmap,mode);
    }
    return result;
}
LinkShell::LinkShell(bool collaboration,QWidget *parent):QWidget(parent),collaboration_(collaboration) {
    setObjectName("linkShell");
    QFont font(QStringLiteral("Noto Sans CJK SC")); font.setPixelSize(14); setFont(font);
    QPalette light=palette(); light.setColor(QPalette::Window,QColor("#fafafa"));
    light.setColor(QPalette::Base,Qt::white); light.setColor(QPalette::Text,QColor("#222738"));
    light.setColor(QPalette::WindowText,QColor("#222738")); light.setColor(QPalette::Highlight,QColor("#4568e8"));
    light.setColor(QPalette::HighlightedText,Qt::white); setPalette(light);
    setStyleSheet(QStringLiteral(
        "QWidget#linkShell { background:#fafafa; color:#222738; }"
        "QWidget#mainContent { background:#fafafa; }"
        "QLabel { color:#222738; background:transparent; }"
        "QLabel#muted, QLabel#sessionStatus, QLabel#detailStatus { color:#7b8292; font-size:12px; }"
        "QLabel#brand { font-size:16px; font-weight:600; }"
        "QLabel#pageTitle { font-size:20px; font-weight:600; }"
        "QLabel#waitingTitle { font-size:20px; font-weight:600; }"
        "QLabel#pageSubtitle, QLabel#waitingInstruction { color:#7b8292; font-size:14px; }"
        "QLabel#connectionState { color:#66718c; font-size:12px; padding:4px 9px; background:#eef0f5; border-radius:9px; }"
        "QLabel#receiverState { color:#39804d; font-size:12px; }"
        "QPushButton { padding:7px 10px; border:1px solid #e0e3eb; border-radius:6px; background:white; color:#30384b; min-height:20px; }"
        "QPushButton:hover { border-color:#b9c4e4; background:#f0f3fd; }"
        "QPushButton:pressed { background:#e5ebff; }"
        "QPushButton:disabled { color:#abb2c0; border-color:#e9ebf0; background:#fafafa; }"
        "QPushButton:checked { background:#edf1ff; border-color:#cbd6ff; color:#3459d6; }"
        "QPushButton[primary=\"true\"] { background:#4568e8; border-color:#4568e8; color:white; }"
        "QPushButton[primary=\"true\"]:disabled { background:#e5e9f6; border-color:#e5e9f6; color:#a1accd; }"
        "QPushButton[nav=\"true\"] { text-align:left; padding:6px 8px; border:none; background:transparent; color:#737d91; }"
        "QPushButton[nav=\"true\"]:hover { background:#f0f2f7; }"
        "QPushButton[nav=\"true\"]:checked { background:#f0f0f3; color:#222738; font-weight:500; }"
        "QLineEdit, QComboBox { padding:7px 9px; border:1px solid #dfe3eb; border-radius:6px; background:white; color:#30384b; min-height:20px; selection-background-color:#4568e8; }"
        "QLineEdit:disabled { background:#f5f6f9; color:#a5adbd; border-color:#e5e8ef; }"
        "QLineEdit:focus, QComboBox:focus { border-color:#4568e8; }"
        "QCheckBox { spacing:7px; color:#66718c; padding:6px; }"
        "QCheckBox:disabled { color:#abb2c0; }"
        "QCheckBox::indicator { width:14px; height:14px; border:1px solid #c4cada; border-radius:3px; background:white; }"
        "QCheckBox::indicator:checked { background:#4568e8; border-color:#4568e8; image:url(none); }"
        "QWidget#connectionSidebar { background:#fafafa; }"
        "QScrollArea#sidebarScroll { border:none; border-right:1px solid #e4e7ee; background:#fafafa; }"
        "QWidget#phoneStage { background:#f3f4f7; border:1px solid #eceef3; border-radius:10px; }"
        "QFrame#toolbar { border:none; border-top:1px solid #e4e7ee; }"
        "QFrame#toolbar QPushButton { background:transparent; border-color:transparent; }"
        "QFrame#toolbar QPushButton:hover { background:#eef1f8; }"
        "QFrame#toolbar QPushButton:checked { background:#e5ebff; border-color:#cbd6ff; }"
        "QFrame#toolbar QPushButton#collaborationApplication, QFrame#toolbar QPushButton#collaborationBack { background:white; border-color:#e4e7ee; }"
        "QFrame#toolbar QPushButton[primary=\"true\"] { background:#4568e8; border-color:#4568e8; color:white; }"
        "QFrame#toolbar QPushButton[primary=\"true\"]:disabled { background:#e5e9f6; border-color:#e5e9f6; color:#a1accd; }"
        "QScrollBar:vertical { background:transparent; width:5px; margin:0; }"
        "QScrollBar::handle:vertical { background:#d2d7e2; min-height:24px; border-radius:2px; }"
        "QScrollBar::add-line:vertical,QScrollBar::sub-line:vertical { height:0; }"
    ));
    pages_=new QStackedLayout(this); pages_->setContentsMargins(0,0,0,0);
    normalPage_=new QWidget; pages_->addWidget(normalPage_);
    pages_->setSizeConstraint(QLayout::SetNoConstraint);
    focusPage_=new QWidget; focusPage_->setObjectName("focusPage");
    focusPage_->setStyleSheet("QWidget#focusPage { background:black; }");
    focusLayout_=new QVBoxLayout(focusPage_); focusLayout_->setContentsMargins(0,0,0,0); focusLayout_->setSpacing(0);
    pages_->addWidget(focusPage_);
    auto root=new QHBoxLayout(normalPage_); root->setContentsMargins(0,0,0,0); root->setSpacing(0);
    sidebar=new QWidget; sidebar->setObjectName("connectionSidebar");
    auto left=new QVBoxLayout(sidebar); left->setContentsMargins(8,26,8,20); left->setSpacing(6);
    auto brand=label(QStringLiteral("努比亚连接"),nullptr,"brand"); brand->setContentsMargins(8,0,8,0); left->addWidget(brand);
    auto brandNote=label(QStringLiteral("手机与电脑，轻松连接"),nullptr,"muted"); brandNote->setContentsMargins(8,0,8,0); left->addWidget(brandNote); left->addSpacing(20);
    auto group=new QButtonGroup(this);
    for(bool collab:{true,false}) {
        auto button=new QPushButton(collab ? QStringLiteral("多屏协同") : QStringLiteral("智慧投屏"));
        button->setObjectName(collab ? "collaborationMode" : "smartcastMode"); button->setProperty("nav",true);
        button->setIcon(icon(collab ? "panels" : "cast")); button->setIconSize({18,18});
        button->setCheckable(true); button->setChecked(collab==collaboration); group->addButton(button); left->addWidget(button);
        connect(button,&QPushButton::clicked,this,[this,group,collab] {
            if(collab!=collaboration_) {
                for(auto current:group->buttons()) current->setChecked(current->objectName()==(collaboration_ ? "collaborationMode" : "smartcastMode"));
                emit modeRequested(collab);
            }
        });
    }
    left->addSpacing(16);
    files=new QPushButton(QStringLiteral("文件互传")); files->setProperty("nav",true); files->setIcon(icon("files")); files->setIconSize({18,18}); left->addWidget(files);
    left->addStretch(1);
    incomingPanel_=new QWidget; auto incomingLayout=new QVBoxLayout(incomingPanel_);
    incomingLayout->setContentsMargins(8,0,8,22); incomingLayout->setSpacing(9);
    incomingLayout->addWidget(label(QStringLiteral("这台电脑"),nullptr,"muted"));
    computerName=label(QStringLiteral("Nubia Link Linux")); incomingLayout->addWidget(computerName);
    receiverState=label(QStringLiteral("接收尚未开启"),nullptr,"receiverState"); incomingLayout->addWidget(receiverState);
    receiveButton=new QPushButton(QStringLiteral("开启接收")); receiveButton->setObjectName("receiveButton");
    receiveButton->setIcon(icon("play")); incomingLayout->addWidget(receiveButton,0,Qt::AlignLeft); left->addWidget(incomingPanel_);
    advancedToggle_=new QPushButton(QStringLiteral("高级选项")); advancedToggle_->setProperty("nav",true); advancedToggle_->setIcon(icon("settings")); advancedToggle_->setCheckable(true); left->addWidget(advancedToggle_);
    advancedToggle_->setVisible(collaboration);
    advanced=new QWidget; advancedLayout=new QVBoxLayout(advanced); advancedLayout->setContentsMargins(8,6,8,8); advancedLayout->setSpacing(8); left->addWidget(advanced); advanced->hide();
    auto helpToggle=new QPushButton(QStringLiteral("使用说明")); helpToggle->setProperty("nav",true); helpToggle->setIcon(icon("help")); helpToggle->setCheckable(true); left->addWidget(helpToggle);
    help=label({},nullptr,"muted"); help->hide(); left->addWidget(help);
    auto sidebarScroll=new QScrollArea; sidebarScroll->setObjectName("sidebarScroll"); sidebarScroll->setFrameShape(QFrame::NoFrame);
    sidebarScroll->setWidgetResizable(true); sidebarScroll->setHorizontalScrollBarPolicy(Qt::ScrollBarAlwaysOff);
    sidebarScroll->setFixedWidth(240); sidebarScroll->setWidget(sidebar); root->addWidget(sidebarScroll);
    auto main=new QWidget; main->setObjectName("mainContent"); main->setAttribute(Qt::WA_StyledBackground);
    contentLayout=new QVBoxLayout(main); contentLayout->setContentsMargins(32,28,32,24); contentLayout->setSpacing(14); root->addWidget(main,1);
    auto header=new QHBoxLayout; header->setSpacing(12);
    auto titles=new QVBoxLayout; titles->setSpacing(5);
    titles->addWidget(label(collaboration ? QStringLiteral("多屏协同") : QStringLiteral("智慧投屏"),nullptr,"pageTitle"));
    titles->addWidget(label(collaboration ? QStringLiteral("在电脑上操作手机，键鼠输入更方便") : QStringLiteral("将手机画面投到电脑，清晰查看"),nullptr,"pageSubtitle")); header->addLayout(titles,1);
    connectionState=label(QStringLiteral("未连接"),nullptr,"connectionState"); connectionState->setWordWrap(false); connectionState->setSizePolicy(QSizePolicy::Minimum,QSizePolicy::Fixed); header->addWidget(connectionState);
    focus_=new QPushButton(QStringLiteral("专注画面")); focus_->setObjectName("focusMode"); focus_->setCheckable(true); focus_->setIcon(icon("focus")); header->addWidget(focus_); contentLayout->addLayout(header);
    focus_->setToolTip(QStringLiteral("竖屏画面小窗口；Esc 返回，Alt + 拖动移动窗口"));
    contentLayout->addSpacing(8);
    stage=new QWidget; stage->setObjectName("phoneStage"); stage->setMinimumSize(260,240); contentLayout->addWidget(stage,1);
    placeholder=new QLabel(stage); placeholder->setObjectName("stagePlaceholder");
    auto waiting=new QVBoxLayout(placeholder); waiting->setContentsMargins(16,12,16,12); waiting->setSpacing(10); waiting->addStretch();
    auto art=new QLabel;
    auto illustration=waitingIllustration(collaboration,devicePixelRatioF());
    art->setPixmap(illustration.scaled(QSize(138,96)*devicePixelRatioF(),Qt::KeepAspectRatio,Qt::SmoothTransformation));
    art->setFixedHeight(96); art->setAlignment(Qt::AlignCenter); waiting->addWidget(art);
    auto title=label(QStringLiteral("等待手机连接"),nullptr,"waitingTitle"); title->setAlignment(Qt::AlignCenter); waiting->addWidget(title);
    auto instruction=label(collaboration ? QStringLiteral("打开手机「多屏协同」，选择这台电脑") : QStringLiteral("打开手机「智慧投屏」，搜索并选择这台电脑"),nullptr,"waitingInstruction"); instruction->setAlignment(Qt::AlignCenter); waiting->addWidget(instruction);
    auto name=label(QStringLiteral("Nubia Link Linux")); name->setAlignment(Qt::AlignCenter); waiting->addWidget(name);
    auto network=label(collaboration ? QStringLiteral("连接同一 Wi-Fi · 两端开启蓝牙") : QStringLiteral("手机与电脑连接同一 Wi-Fi"),nullptr,"muted"); network->setAlignment(Qt::AlignCenter); waiting->addWidget(network); waiting->addStretch();
    steps_=new QWidget; auto steps=new QHBoxLayout(steps_); steps->setContentsMargins(0,5,0,5); steps->setSpacing(12);
    const QStringList captions={QStringLiteral("打开手机功能"),QStringLiteral("选择这台电脑"),QStringLiteral("自动连接")};
    const QStringList notes={collaboration ? QStringLiteral("进入原厂多屏协同") : QStringLiteral("进入原厂智慧投屏"),QStringLiteral("Nubia Link Linux"),QStringLiteral("手机画面在此显示")};
    for(int i=0;i<3;++i) {
        auto step=new QVBoxLayout; step->setSpacing(5);
        step->addWidget(label(QStringLiteral("<span style='color:#9199ab'>0%1</span> &nbsp; %2").arg(i+1).arg(captions[i])));
        step->addWidget(label(notes[i],nullptr,"muted")); steps->addLayout(step,1);
    }
    contentLayout->addWidget(steps_);
    auto toolbar=new QFrame; toolbar->setObjectName("toolbar"); controls=new QHBoxLayout(toolbar); controls->setContentsMargins(0,12,0,0); controls->setSpacing(8); contentLayout->addWidget(toolbar);
    status=label({},nullptr,"sessionStatus"); status->setMinimumHeight(18); contentLayout->addWidget(status);
    connect(advancedToggle_,&QPushButton::toggled,advanced,&QWidget::setVisible);
    connect(helpToggle,&QPushButton::toggled,help,&QWidget::setVisible);
    connect(focus_,&QPushButton::toggled,this,&LinkShell::setFocusMode);
    auto escape=new QShortcut(QKeySequence(Qt::Key_Escape),this);
    escape->setContext(Qt::WindowShortcut); escape->setEnabled(false);
    connect(focus_,&QPushButton::toggled,escape,&QShortcut::setEnabled);
    connect(escape,&QShortcut::activated,this,[this] { focus_->setChecked(false); });
}
void LinkShell::setFocusMode(bool enabled) {
    if(focused_==enabled) return;
    focused_=enabled;
    auto host=window();
    if(enabled) {
        previousWindowState_=host->windowState(); previousGeometry_=host->geometry();
        previousWindowFlags_=host->windowHandle()->flags(); previousMinimumSize_=host->minimumSize(); previousStageMinimum_=stage->minimumSize();
        if(auto main=qobject_cast<QMainWindow *>(host)) {
            previousCentralPolicy_=main->centralWidget()->sizePolicy();
            main->centralWidget()->setSizePolicy(QSizePolicy::Ignored,QSizePolicy::Ignored);
        }
        contentLayout->removeWidget(stage); focusLayout_->addWidget(stage);
        stage->setMinimumSize(0,0);
        previousStageTracking_=stage->hasMouseTracking(); stage->setMouseTracking(true);
        stage->setStyleSheet("background:black; border:none; border-radius:0;");
        placeholder->hide(); pages_->setCurrentWidget(focusPage_); stage->show();
        host->setMinimumSize(120,120);
        host->setWindowState(Qt::WindowNoState);
        resizeFocusWindow(); host->show();
        host->raise(); host->activateWindow();
        qApp->installEventFilter(this);
        QTimer::singleShot(0,this,&LinkShell::focusScreen);
    } else {
        qApp->removeEventFilter(this);
        stage->setMouseTracking(previousStageTracking_); stage->unsetCursor();
        if(screenInput_) { screenInput_->setMouseTracking(previousInputTracking_); screenInput_->unsetCursor(); screenInput_=nullptr; }
        focusLayout_->removeWidget(stage); contentLayout->insertWidget(2,stage,1);
        stage->setMinimumSize(previousStageMinimum_);
        stage->setStyleSheet({}); pages_->setCurrentWidget(normalPage_); stage->show();
        placeholder->setVisible(waitingVisible_);
        host->windowHandle()->setFlags(previousWindowFlags_); host->setMinimumSize(previousMinimumSize_);
        if(auto main=qobject_cast<QMainWindow *>(host)) main->centralWidget()->setSizePolicy(previousCentralPolicy_);
        host->setGeometry(previousGeometry_); host->setWindowState(previousWindowState_); host->show();
        host->raise(); host->activateWindow();
        focus_->setFocus(Qt::OtherFocusReason);
    }
}
void LinkShell::focusScreen() {
    if(!focused_) return;
    for(auto child:stage->findChildren<QWidget *>(QString(),Qt::FindDirectChildrenOnly)) {
        if(child->isVisible() && child->focusPolicy()!=Qt::NoFocus) {
            if(screenInput_!=child) {
                screenInput_=child; previousInputTracking_=child->hasMouseTracking(); child->setMouseTracking(true);
            }
            child->setFocus(Qt::OtherFocusReason); return;
        }
    }
    stage->setFocus(Qt::OtherFocusReason);
}
Qt::Edges LinkShell::resizeEdges(QPoint position) const {
    constexpr int grip=8;
    Qt::Edges edges;
    if(position.x()<grip) edges|=Qt::LeftEdge;
    else if(position.x()>=window()->width()-grip) edges|=Qt::RightEdge;
    if(position.y()<grip) edges|=Qt::TopEdge;
    else if(position.y()>=window()->height()-grip) edges|=Qt::BottomEdge;
    return edges;
}
bool LinkShell::eventFilter(QObject *object,QEvent *event) {
    if(focused_ && object==window() && event->type()==QEvent::Resize && !constrainPending_) {
        constrainPending_=true;
        QTimer::singleShot(0,this,[this] { constrainPending_=false; if(focused_) constrainFocusSize(); });
    }
    if(focused_ && object==window() && event->type()==QEvent::WindowActivate)
        QTimer::singleShot(0,this,&LinkShell::focusScreen);
    auto widget=qobject_cast<QWidget *>(object);
    if(focused_ && widget && (widget==stage || stage->isAncestorOf(widget)) &&
       (event->type()==QEvent::MouseButtonPress || event->type()==QEvent::MouseMove)) {
        auto mouse=static_cast<QMouseEvent *>(event);
        const auto edges=resizeEdges(widget->mapTo(window(),mouse->position().toPoint()));
        if(event->type()==QEvent::MouseMove && !mouse->buttons()) {
            if((edges.testFlag(Qt::LeftEdge) && edges.testFlag(Qt::TopEdge)) || (edges.testFlag(Qt::RightEdge) && edges.testFlag(Qt::BottomEdge))) widget->setCursor(Qt::SizeFDiagCursor);
            else if((edges.testFlag(Qt::RightEdge) && edges.testFlag(Qt::TopEdge)) || (edges.testFlag(Qt::LeftEdge) && edges.testFlag(Qt::BottomEdge))) widget->setCursor(Qt::SizeBDiagCursor);
            else if(edges & (Qt::LeftEdge|Qt::RightEdge)) widget->setCursor(Qt::SizeHorCursor);
            else if(edges & (Qt::TopEdge|Qt::BottomEdge)) widget->setCursor(Qt::SizeVerCursor);
            else widget->unsetCursor();
        }
        if(mouse->button()==Qt::LeftButton) {
            if(auto native=window()->windowHandle()) {
                if(mouse->modifiers().testFlag(Qt::AltModifier)) { native->startSystemMove(); return true; }
                if(edges) { native->startSystemResize(edges); return true; }
            }
        }
    }
    return QWidget::eventFilter(object,event);
}
void LinkShell::setFrameSize(QSize size) {
    if(!size.isValid() || size==frameSize_) return;
    const bool changedDirection=(size.height()>size.width())!=(frameSize_.height()>frameSize_.width());
    frameSize_=size;
    if(focused_ && changedDirection) resizeFocusWindow();
}
void LinkShell::resizeFocusWindow() {
    auto host=window();
    const auto available=host->screen()->availableGeometry();
    const bool portrait=frameSize_.height()>frameSize_.width();
    const int height=portrait ? qMin(720,int(available.height()*0.78)) : int(available.height()*0.78);
    const QSize bounds(int(available.width()*0.72),height);
    const auto size=frameSize_.scaled(bounds,Qt::KeepAspectRatio);
    const auto border=host->frameGeometry().size()-host->size();
    host->resize(size);
    host->move(available.x()+(available.width()-size.width()-border.width())/2,
               available.y()+(available.height()-size.height()-border.height())/2);
}
void LinkShell::constrainFocusSize() {
    auto host=window();
    const double ratio=double(frameSize_.width())/frameSize_.height();
    const auto actual=host->size();
    if(qAbs(actual.width()-actual.height()*ratio)<=1) return;
    const QSize byHeight(qRound(actual.height()*ratio),actual.height());
    const QSize byWidth(actual.width(),qRound(actual.width()/ratio));
    QSize target=qAbs(byHeight.width()-actual.width())<qAbs(byWidth.height()-actual.height()) ? byHeight : byWidth;
    const auto bounds=host->screen()->availableGeometry().size();
    if(target.width()>bounds.width() || target.height()>bounds.height()) target=target.scaled(bounds,Qt::KeepAspectRatio);
    if(target.width()>=120 && target.height()>=120) host->resize(target);
}
void LinkShell::setWaitingVisible(bool visible) {
    waitingVisible_=visible; placeholder->setVisible(visible && !focused_);
}
void LinkShell::setReceiving(bool enabled) {
    receiving_=enabled; receiveButton->setText(enabled ? QStringLiteral("暂停接收") : QStringLiteral("开启接收"));
    receiveButton->setIcon(icon(enabled ? "pause" : "play"));
    receiverState->setText(enabled ? QStringLiteral("●  接收已开启") : QStringLiteral("接收已暂停"));
    connectionState->setText(enabled ? QStringLiteral("等待手机") : QStringLiteral("已暂停"));
}
void LinkShell::setMediaActive(bool active) { steps_->setVisible(!active); }
void LinkShell::setMessageControls(QWidget *widget) {
    message_=widget; arrangeMessageControls();
}
void LinkShell::arrangeMessageControls() {
    if(!message_) return;
    const bool inlineControls=width()>=1100;
    if((inlineControls && controls->indexOf(message_)>=0) || (!inlineControls && contentLayout->indexOf(message_)>=0)) return;
    controls->removeWidget(message_); contentLayout->removeWidget(message_);
    if(inlineControls) controls->addWidget(message_,1);
    else contentLayout->insertWidget(contentLayout->indexOf(status),message_);
    message_->show();
}
void LinkShell::resizeEvent(QResizeEvent *event) {
    QWidget::resizeEvent(event);
    contentLayout->setContentsMargins(width()<1000 ? 20 : 32,28,width()<1000 ? 20 : 32,24);
    arrangeMessageControls();
}
LinkShell::Interface LinkShell::receiverInterface() {
    Interface chosen; int score=-1;
    for(const auto &interface:QNetworkInterface::allInterfaces()) {
        if(!interface.flags().testFlag(QNetworkInterface::IsUp) || !interface.flags().testFlag(QNetworkInterface::CanMulticast) || interface.flags().testFlag(QNetworkInterface::IsLoopBack)) continue;
        const int priority=interface.type()==QNetworkInterface::Wifi ? 2 : interface.type()==QNetworkInterface::Ethernet ? 1 : -1;
        if(priority<0 || priority<=score) continue;
        for(const auto &entry:interface.addressEntries()) {
            if(entry.ip().protocol()!=QAbstractSocket::IPv4Protocol || entry.prefixLength()<1) continue;
            chosen={entry.ip(),entry.prefixLength(),interface.name()}; score=priority; break;
        }
    }
    return chosen;
}
