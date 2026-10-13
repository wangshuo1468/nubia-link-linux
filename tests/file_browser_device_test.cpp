#include "file_browser.h"
#include <QtTest>
#include <QTreeWidget>
#include <QPushButton>
#include <QLabel>

// Opt-in: reads only the root/Download directory names, never file contents.
class FileBrowserDeviceTest:public QObject {
    Q_OBJECT
private slots:
    void browseSharedStorage() {
        const auto phone=qEnvironmentVariable("NUBIA_TEST_LAN_PHONE"),local=qEnvironmentVariable("NUBIA_TEST_LAN_LOCAL");
        if(phone.isEmpty() || local.isEmpty()) QSKIP("Set authorized LAN device addresses to run this probe");
        FileBrowser browser{QHostAddress(local),QHostAddress(phone)}; browser.show();
        auto tree=browser.findChild<QTreeWidget *>("phoneFileList"); QVERIFY(tree);
        auto reconnect=browser.findChild<QPushButton *>("reconnectPhoneFiles"); QVERIFY(reconnect);
        QTRY_VERIFY_WITH_TIMEOUT((tree->isEnabled() && tree->topLevelItemCount()>0) || reconnect->isEnabled(),30000);
        const auto screenshot=qEnvironmentVariable("NUBIA_TEST_BROWSER_SCREENSHOT");
        if(!screenshot.isEmpty()) QVERIFY(browser.grab().save(screenshot));
        QVERIFY2(tree->isEnabled() && tree->topLevelItemCount()>0,"Phone file discovery/session failed; check that the phone is awake and official sharing is enabled");
        QPushButton *upload=nullptr;
        for(auto button:browser.findChildren<QPushButton *>()) if(button->text()==QStringLiteral("上传到当前目录")) upload=button;
        QVERIFY(upload); QVERIFY(upload->isEnabled());
        const int idleMs=qEnvironmentVariableIntValue("NUBIA_TEST_FILE_IDLE_MS");
        if(idleMs>0) {
            QVERIFY(idleMs<=60000);
            QTest::qWait(idleMs); // Include at least one real phone urgent heartbeat.
            QVERIFY2(upload->isEnabled() && !reconnect->isEnabled(),"File session disconnected while idle");
        }
        QTreeWidgetItem *download=nullptr;
        for(int i=0;i<tree->topLevelItemCount();++i) if(tree->topLevelItem(i)->text(0)=="Download") download=tree->topLevelItem(i);
        QVERIFY(download); tree->scrollToItem(download);
        const auto position=tree->visualItemRect(download).center();
        // QWidget mouseDClick sends only the second press. A real first click
        // establishes QAbstractItemView's pressed index before the double click.
        QTest::mouseClick(tree->viewport(),Qt::LeftButton,Qt::NoModifier,position);
        QTest::mouseDClick(tree->viewport(),Qt::LeftButton,Qt::NoModifier,position);
        QTest::mouseRelease(tree->viewport(),Qt::LeftButton,Qt::NoModifier,position);
        QTRY_VERIFY_WITH_TIMEOUT(([&] {
            for(auto label:browser.findChildren<QLabel *>()) if(label->text()==QStringLiteral("手机共享存储 /Download")) return true;
            return false;
        })(),15000);
        QTRY_VERIFY(upload->isEnabled()); browser.close(); QTest::qWait(7100);
    }
};
QTEST_MAIN(FileBrowserDeviceTest)
#include "file_browser_device_test.moc"
