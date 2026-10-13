#pragma once
#include "file_session.h"
#include <QDialog>
class QLabel;
class QPushButton;
class QTreeWidget;
class QTreeWidgetItem;
class QProgressBar;
class QCloseEvent;
class FileBrowser : public QDialog {
    Q_OBJECT
public:
    FileBrowser(QHostAddress local,QHostAddress phone,QWidget *parent=nullptr);
    ~FileBrowser() override;
protected:
    void closeEvent(QCloseEvent *event) override;
private:
    void refresh();
    void updateControls();
    void upload();
    void download();
    FileSession session_;
    QString path_;
    QList<Dfs::Entry> entries_;
    QLabel *pathLabel_,*state_;
    QTreeWidget *tree_;
    QPushButton *up_,*reload_,*reconnect_,*upload_,*download_;
    QProgressBar *progress_;
};
