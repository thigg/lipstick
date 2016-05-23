/***************************************************************************
**
** Copyright (C) 2010 Nokia Corporation and/or its subsidiary(-ies).
**
** This library is free software; you can redistribute it and/or
** modify it under the terms of the GNU Lesser General Public
** License version 2.1 as published by the Free Software Foundation
** and appearing in the file LICENSE.LGPL included in the packaging
** of this file.
**
****************************************************************************/

#include <QQmlComponent>
#include <QQmlContext>
#include <QQmlEngine>
#include <QScreen>
#include <QTimer>
#include <QDBusMessage>
#include <QDBusConnection>
#include <QIcon>
#include <QTranslator>
#include <QDebug>
#include <QEvent>
#include <systemd/sd-daemon.h>

#include "notifications/notificationmanager.h"
#include "notifications/notificationpreviewpresenter.h"
#include "notifications/batterynotifier.h"
#include "notifications/diskspacenotifier.h"
#include "notifications/thermalnotifier.h"
#include "screenlock/screenlock.h"
#include "screenlock/screenlockadaptor.h"
#include "touchscreen/touchscreen.h"
#include "lipsticksettings.h"
#include "homeapplication.h"
#include "homewindow.h"
#include "compositor/lipstickcompositor.h"
#include "compositor/lipstickcompositorwindow.h"
#include "lipstickdbus.h"

#include "volume/volumecontrol.h"
#include "usbmodeselector.h"
#include "shutdownscreen.h"
#include "shutdownscreenadaptor.h"
#include "connectionselector.h"
#include "screenshotservice.h"
#include "screenshotserviceadaptor.h"

void HomeApplication::quitSignalHandler(int)
{
    qApp->quit();
}

static void registerDBusObject(QDBusConnection &bus, const char *path, QObject *object)
{
    if (!bus.registerObject(path, object)) {
        qWarning("Unable to register object at path %s: %s", path, bus.lastError().message().toUtf8().constData());
    }
}

HomeApplication::HomeApplication(int &argc, char **argv, const QString &qmlPath)
    : QGuiApplication(argc, argv)
    , m_mainWindowInstance(0)
    , m_qmlPath(qmlPath)
    , m_originalSigIntHandler(signal(SIGINT, quitSignalHandler))
    , m_originalSigTermHandler(signal(SIGTERM, quitSignalHandler))
    , m_homeReadySent(false)
    , m_screenshotService(0)
{
    QTranslator *engineeringEnglish = new QTranslator(this);
    engineeringEnglish->load("lipstick_eng_en", "/usr/share/translations");
    installTranslator(engineeringEnglish);
    QTranslator *translator = new QTranslator(this);
    translator->load(QLocale(), "lipstick", "-", "/usr/share/translations");
    installTranslator(translator);

    // Set the application name, as used in notifications
    //% "System"
    setApplicationName(qtTrId("qtn_ap_lipstick"));
    setApplicationVersion(VERSION);

    // Initialize the QML engine
    m_qmlEngine = new QQmlEngine(this);

    // Export screen size / geometry as dconf keys
    LipstickSettings::instance()->exportScreenProperties();

    // Create touch screen and register for the screen lock.
    m_touchScreen = new TouchScreen(this);
    connect(m_touchScreen, &TouchScreen::displayStateChanged, this, &HomeApplication::displayStateChanged);

    // Create screen lock logic - not parented to "this" since destruction happens too late in that case
    m_screenLock = new ScreenLock(m_touchScreen);
    LipstickSettings::instance()->setScreenLock(m_screenLock);
    new ScreenLockAdaptor(m_screenLock);

    // Initialize the notification manager
    NotificationManager::instance();
    new NotificationPreviewPresenter(this);

    m_volumeControl = new VolumeControl;
    new BatteryNotifier(this);
    new DiskSpaceNotifier(this);
    new ThermalNotifier(this);
    m_usbModeSelector = new USBModeSelector(this);
    m_shutdownScreen = new ShutdownScreen(this);
    new ShutdownScreenAdaptor(m_shutdownScreen);
    m_connectionSelector = new ConnectionSelector(this);

    // MCE and usb-moded expect services to be registered on the system bus
    QDBusConnection systemBus = QDBusConnection::systemBus();
    if (!systemBus.registerService(LIPSTICK_DBUS_SERVICE_NAME)) {
        qWarning("Unable to register D-Bus service %s: %s", LIPSTICK_DBUS_SERVICE_NAME, systemBus.lastError().message().toUtf8().constData());
    }

    registerDBusObject(systemBus, LIPSTICK_DBUS_SCREENLOCK_PATH, m_screenLock);
    registerDBusObject(systemBus, LIPSTICK_DBUS_SHUTDOWN_PATH, m_shutdownScreen);

    m_screenshotService = new ScreenshotService(this);
    new ScreenshotServiceAdaptor(m_screenshotService);
    QDBusConnection sessionBus = QDBusConnection::sessionBus();

    registerDBusObject(sessionBus, LIPSTICK_DBUS_SCREENSHOT_PATH, m_screenshotService);

    // Setting up the context and engine things
    m_qmlEngine->rootContext()->setContextProperty("initialSize", QGuiApplication::primaryScreen()->size());
    m_qmlEngine->rootContext()->setContextProperty("lipstickSettings", LipstickSettings::instance());
    m_qmlEngine->rootContext()->setContextProperty("LipstickSettings", LipstickSettings::instance());
    m_qmlEngine->rootContext()->setContextProperty("volumeControl", m_volumeControl);

    connect(this, SIGNAL(homeReady()), this, SLOT(sendStartupNotifications()));
}

HomeApplication::~HomeApplication()
{
    emit aboutToDestroy();

    delete m_volumeControl;
    delete m_screenLock;
    delete m_mainWindowInstance;
    delete m_qmlEngine;
}

HomeApplication *HomeApplication::instance()
{
    return qobject_cast<HomeApplication *>(qApp);
}

void HomeApplication::restoreSignalHandlers()
{
    signal(SIGINT, m_originalSigIntHandler);
    signal(SIGTERM, m_originalSigTermHandler);
}

void HomeApplication::sendHomeReadySignalIfNotAlreadySent()
{
    if (!m_homeReadySent) {
        m_homeReadySent = true;
        disconnect(LipstickCompositor::instance(), SIGNAL(frameSwapped()), this, SLOT(sendHomeReadySignalIfNotAlreadySent()));

        emit homeReady();
    }
}

void HomeApplication::sendStartupNotifications()
{
    static QDBusConnection systemBus = QDBusConnection::systemBus();
    QDBusMessage homeReadySignal =
        QDBusMessage::createSignal("/com/nokia/duihome",
                                   "com.nokia.duihome.readyNotifier",
                                   "ready");
    systemBus.send(homeReadySignal);

    /* Let systemd know that we are initialized */
    if (arguments().indexOf("--systemd") >= 0) {
        sd_notify(0, "READY=1");
    }

    /* Let timed know that the UI is up */
    systemBus.call(QDBusMessage::createSignal("/com/nokia/startup/signal", "com.nokia.startup.signal", "desktop_visible"), QDBus::NoBlock);
}

bool HomeApplication::homeActive() const
{
    LipstickCompositor *c = LipstickCompositor::instance();
    return c?c->homeActive():(QGuiApplication::focusWindow() != 0);
}

TouchScreen *HomeApplication::touchScreen() const
{
    return m_touchScreen;
}

TouchScreen::DisplayState HomeApplication::displayState()
{
    Q_ASSERT(m_touchScreen);
    return m_touchScreen->currentDisplayState();
}

void HomeApplication::setDisplayOff()
{
    Q_ASSERT(m_touchScreen);
    m_touchScreen->setDisplayOff();
}

bool HomeApplication::event(QEvent *e)
{
    bool rv = QGuiApplication::event(e);
    if (LipstickCompositor::instance() == 0 &&
        (e->type() == QEvent::ApplicationActivate ||
         e->type() == QEvent::ApplicationDeactivate))
        emit homeActiveChanged();
    return rv;
}

const QString &HomeApplication::qmlPath() const
{
    return m_qmlPath;
}

void HomeApplication::setQmlPath(const QString &path)
{
    m_qmlPath = path;

    if (m_mainWindowInstance) {
        m_mainWindowInstance->setSource(path);
        if (m_mainWindowInstance->hasErrors()) {
            qWarning() << "HomeApplication: Errors while loading" << path;
            qWarning() << m_mainWindowInstance->errors();
        }
    }
}

const QString &HomeApplication::compositorPath() const
{
    return m_compositorPath;
}

void HomeApplication::setCompositorPath(const QString &path)
{
    if (path.isEmpty()) {
        qWarning() << "HomeApplication: Invalid empty compositor path";
        return;
    }

    if (!m_compositorPath.isEmpty()) {
        qWarning() << "HomeApplication: Compositor already set";
        return;
    }

    m_compositorPath = path;
    QQmlComponent component(m_qmlEngine, QUrl(path));
    if (component.isError()) {
        qWarning() << "HomeApplication: Errors while loading compositor from" << path;
        qWarning() << component.errors();
        return;
    } 

    QObject *compositor = component.beginCreate(m_qmlEngine->rootContext());
    if (compositor) {
        compositor->setParent(this);
        if (LipstickCompositor::instance()) {
            LipstickCompositor::instance()->setGeometry(QRect(QPoint(0, 0), QGuiApplication::primaryScreen()->size()));
            connect(m_usbModeSelector, SIGNAL(showUnlockScreen()),
                    LipstickCompositor::instance(), SIGNAL(showUnlockScreen()));
        }

        component.completeCreate();

        if (!m_qmlEngine->incubationController() && LipstickCompositor::instance()) {
            // install default incubation controller
            m_qmlEngine->setIncubationController(LipstickCompositor::instance()->incubationController());
        }
    } else {
        qWarning() << "HomeApplication: Error creating compositor from" << path;
        qWarning() << component.errors();
    }
}

HomeWindow *HomeApplication::mainWindowInstance()
{
    if (m_mainWindowInstance)
        return m_mainWindowInstance;

    m_mainWindowInstance = new HomeWindow();
    m_mainWindowInstance->setGeometry(QRect(QPoint(), QGuiApplication::primaryScreen()->size()));
    m_mainWindowInstance->setWindowTitle("Home");
    QObject::connect(m_mainWindowInstance->engine(), SIGNAL(quit()), qApp, SLOT(quit()));
    QObject::connect(m_mainWindowInstance, SIGNAL(visibleChanged(bool)), this, SLOT(connectFrameSwappedSignal(bool)));

    // Setting the source, if present
    if (!m_qmlPath.isEmpty())
        m_mainWindowInstance->setSource(m_qmlPath);

    return m_mainWindowInstance;
}

QQmlEngine *HomeApplication::engine() const
{
    return m_qmlEngine;
}

void HomeApplication::connectFrameSwappedSignal(bool mainWindowVisible)
{
    if (!m_homeReadySent && mainWindowVisible) {
        connect(LipstickCompositor::instance(), SIGNAL(frameSwapped()), this, SLOT(sendHomeReadySignalIfNotAlreadySent()));
    }
}

void HomeApplication::takeScreenshot(const QString &path)
{
    m_screenshotService->saveScreenshot(path);
}
