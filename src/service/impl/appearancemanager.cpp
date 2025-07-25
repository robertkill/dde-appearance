// SPDX-FileCopyrightText: 2021 - 2023 UnionTech Software Technology Co., Ltd.
//
// SPDX-License-Identifier: GPL-3.0-or-later

#include "appearancemanager.h"

#include "appearancesyncconfig.h"
#include "dbus/appearancedbusproxy.h"
#include "dbus/appearanceproperty.h"
#include "modules/api/keyfile.h"
#include "modules/api/sunrisesunset.h"
#include "modules/api/themethumb.h"
#include "modules/api/utils.h"
#include "modules/common/commondefine.h"
#include "modules/dconfig/phasewallpaper.h"
#include "modules/subthemes/customtheme.h"
#include "modules/dconfig/dconfigsettings.h"

#include <xcb/xcb.h>
#include <KX11Extras>

#include <QColor>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QSettings>
#include <QTimer>
#include <QMetaObject>
#include <QCoreApplication>
#include <DDBusSender>
#include <QRandomGenerator>

#include <pwd.h>

#define NAN_ANGLE (-200.0)          // 异常经纬度
#define DEFAULT_WORKSPACE_COUNT (2) // 默认工作区数量

DCORE_USE_NAMESPACE

AppearanceManager::AppearanceManager(AppearanceProperty *prop, QObject *parent)
    : QObject(parent)
    , m_property(prop)
    , m_settingDconfig(APPEARANCESCHEMA)
    , m_dbusProxy(new AppearanceDBusProxy(this))
    , m_backgrounds(new Backgrounds())
    , m_fontsManager(new FontsManager())
    , m_longitude(NAN_ANGLE)
    , m_latitude(NAN_ANGLE)
    , m_timeUpdateTimeId(0)
    , m_ntpTimeId(0)
    , m_locationValid(false) // 非法经纬度，未初始化状态
    , m_fsnotify(new Fsnotify())
    , m_detectSysClockTimer(this)
    , m_themeAutoTimer(this)
    , m_customTheme(new CustomTheme())
    , m_globalThemeUpdating(false)
    , m_wallpaperConfig({})
    , m_setDefaulting(false)
{
    m_XSettingsDconfig = QSharedPointer<DConfig>(DconfigSettings::ConfigPtr(DDEDAEMONAPPID,XSETTINGSNAME));
    if (!m_XSettingsDconfig) {
        qWarning() << "XSettingsDconfig is NULL";
        exit(-1);
    }

    m_fontsManager->xSetting = m_XSettingsDconfig;
    init();
}

AppearanceManager::~AppearanceManager()
{
    delete m_customTheme;
    m_customTheme = nullptr;
}

bool AppearanceManager::init()
{
    qInfo() << "init";
    getScaleFactor();
    initGlobalOverrideConfig();
    // subthemes需要在获取ScaleFactor后再初始化
    m_subthemes.reset(new Subthemes(this));

    initCoordinate();
    initUserObj();
    initCurrentBgs();

    connect(m_dbusProxy.get(), &AppearanceDBusProxy::workspaceCountChanged, this, &AppearanceManager::handleWmWorkspaceCountChanged);
    connect(m_dbusProxy.get(), &AppearanceDBusProxy::SetScaleFactorStarted, this, &AppearanceManager::handleSetScaleFactorStarted);
    connect(m_dbusProxy.get(), &AppearanceDBusProxy::SetScaleFactorDone, this, &AppearanceManager::handleSetScaleFactorDone);

    connect(m_dbusProxy.get(), &AppearanceDBusProxy::PrimaryChanged, this, &AppearanceManager::updateMonitorMap);
    connect(m_dbusProxy.get(), &AppearanceDBusProxy::MonitorsChanged, this, &AppearanceManager::updateMonitorMap);

    updateMonitorMap();

    new ThemeFontSyncConfig("org.deepin.dde.Appearance1", "/org/deepin/dde/Appearance1/sync", QSharedPointer<AppearanceManager>(this));
    new BackgroundSyncConfig("org.deepin.dde.Appearance1", "/org/deepin/dde/Appearance1/Background", QSharedPointer<AppearanceManager>(this));
    m_zone = m_dbusProxy->timezone();

    connect(m_dbusProxy.get(), &AppearanceDBusProxy::TimezoneChanged, this, &AppearanceManager::handleTimezoneChanged);
    connect(m_dbusProxy.get(), &AppearanceDBusProxy::NTPChanged, this, &AppearanceManager::handleTimeUpdate);
    connect(m_dbusProxy.get(), &AppearanceDBusProxy::TimeUpdate, this, &AppearanceManager::handleTimeUpdate);

    QVector<QSharedPointer<Theme>> iconList = m_subthemes->listIconThemes();
    bool bFound = false;

    for (auto theme : iconList) {
        if (theme->getId() == m_property->iconTheme) {
            bFound = true;
            break;
        }
    }
    if (!bFound) {
        setIconTheme(DEFAULTICONTHEME);
        doSetIconTheme(DEFAULTICONTHEME);
    }

    QVector<QSharedPointer<Theme>> cursorList = m_subthemes->listCursorThemes();
    bFound = false;
    for (auto theme : cursorList) {
        if (theme->getId() == m_property->cursorTheme) {
            bFound = true;
            break;
        }
    }
    if (!bFound) {
        setCursorTheme(DEFAULTCURSORTHEME);
        doSetCursorTheme(DEFAULTCURSORTHEME);
    }

    initDtkSizeMode();
    initGlobalTheme();

    connect(m_fsnotify.data(), SIGNAL(themeFileChange(QString)), this, SLOT(handlethemeFileChange(QString)), Qt::QueuedConnection);

    // connect(m_xSetting.data(), SIGNAL(changed(const QString &)), this, SLOT(handleXsettingDConfigChange(QString)));

    connect(&m_settingDconfig, SIGNAL(valueChanged(const QString &)), this, SLOT(handleSettingDConfigChange(QString)));

    connect(m_XSettingsDconfig.data(),SIGNAL(valueChanged(const QString &)),this,SLOT(handleXsettingDConfigChange(QString)));
    connect(&m_detectSysClockTimer, SIGNAL(timeout()), this, SLOT(handleDetectSysClockTimeOut()));
    connect(&m_themeAutoTimer, SIGNAL(timeout()), this, SLOT(handleGlobalThemeChangeTimeOut()));
    m_themeAutoTimer.start(60000); // 每分钟检查一次是否要切换主题

    connect(m_customTheme, &CustomTheme::updateToCustom, this, &AppearanceManager::handleUpdateToCustom);

    return true;
}

void AppearanceManager::deleteThermByType(const QString &ty, const QString &name)
{
    if (ty.compare(TYPEGTK) == 0) {
        m_subthemes->deleteGtkTheme(name);
    } else if (ty.compare(TYPEICON) == 0) {
        m_subthemes->deleteIconTheme(name);
    } else if (ty.compare(TYPECURSOR) == 0) {
        m_subthemes->deleteCursorTheme(name);
    } else if (ty.compare(TYPEBACKGROUND) == 0) {
        m_backgrounds->deleteBackground(name);
    } else {
        // todo log
    }
}

void AppearanceManager::handleWmWorkspaceCountChanged(int count)
{
    QStringList bgs = m_settingDconfig.value(GSKEYBACKGROUNDURIS).toStringList();

    if (bgs.size() < count) {
        QVector<Background> allBgs = m_backgrounds->listBackground();

        int addCount = count - bgs.count();
        for (int i = 0; i < addCount; i++) {
            int index = rand() % allBgs.count();

            bgs.push_back(allBgs[index].getId());
        }

        m_settingDconfig.setValue(GSKEYBACKGROUNDURIS, bgs);
    } else if (bgs.size() > count) {
        bgs = bgs.mid(0, count);
        m_settingDconfig.setValue(GSKEYBACKGROUNDURIS, bgs);
    }

    PhaseWallPaper::resizeWorkspaceCount(getWorkspaceCount());
    doUpdateWallpaperURIs();
}

void AppearanceManager::handleWmWorkspaceSwithched(int from, int to)
{
    Q_UNUSED(from);
    m_dbusProxy->SetCurrentWorkspace(to);
}

void AppearanceManager::handleSetScaleFactorStarted()
{
    QString body = tr("Start setting display scaling, please wait patiently");
    QString summary = tr("Display scaling");
    m_dbusProxy->Notify("dde-control-center", "dialog-window-scale", summary, body, {}, {}, 0);
}

void AppearanceManager::handleSetScaleFactorDone()
{
    QString body = tr("Log out for display scaling settings to take effect");
    QString summary = tr("Set successfully");
    QStringList options{ "_logout", tr("Log Out Now"), "_later", tr("Later") };
    QMap<QString, QVariant> optionMap;
    optionMap["x-deepin-action-_logout"] = "dbus-send,--type=method_call,--dest=org.deepin.dde.SessionManager1,"
                                           "/org/deepin/dde/SessionManager1,org.deepin.dde.SessionManager1.RequestLogout";
    optionMap["x-deepin-action-_later"] = "";
    int expireTimeout = 15 * 1000;
    m_dbusProxy->Notify("dde-control-center", "dialog-window-scale", summary, body, options, optionMap, expireTimeout);
    // 更新ScaleFactor缓存
    getScaleFactor();
}

void AppearanceManager::handleTimezoneChanged(QString timezone)
{
    if (m_coordinateMap.count(timezone) == 1) {
        m_latitude = m_coordinateMap[timezone].latitude;
        m_longitude = m_coordinateMap[timezone].longitude;
    }
    m_zone = timezone;
    // todo l, err := time.LoadLocation(zone)

    if (m_property->gtkTheme == AUTOGTKTHEME) {
        autoSetTheme(m_latitude, m_longitude);
        resetThemeAutoTimer();
    }
}

void AppearanceManager::handleTimeUpdate()
{
    m_locationValid = true;
    m_timeUpdateTimeId = this->startTimer(2000);
}

void AppearanceManager::handleNTPChanged()
{
    m_locationValid = true;
    m_ntpTimeId = this->startTimer(2000);
}

void AppearanceManager::handlethemeFileChange(QString theme)
{
    if (theme == TYPEGLOBALTHEME) {
        m_subthemes->refreshGlobalThemes();
        initGlobalTheme();
        Q_EMIT Refreshed(TYPEGLOBALTHEME);
    } else if (theme == TYPEBACKGROUND) {
        m_backgrounds->notifyChanged();
    } else if (theme == TYPEGTK) {
        // todo <-time.After(time.Millisecond * 700)
        m_subthemes->refreshGtkThemes();
        Q_EMIT Refreshed(TYPEGTK);
    } else if (theme == TYPEICON) {
        // todo <-time.After(time.Millisecond * 700)
        m_subthemes->refreshIconThemes();
        m_subthemes->refreshCursorThemes();
        Q_EMIT Refreshed(TYPEICON);
        Q_EMIT Refreshed(TYPECURSOR);
    }
}

void AppearanceManager::handleXsettingDConfigChange(QString key)
{
    if (key == DCKEYQTDARKACTIVECOLOR || key == DCKEYQTACTIVECOLOR) {
        QString result = m_settingDconfig.value(GSKEYGLOBALTHEME).toString().endsWith("dark") ?
            m_XSettingsDconfig->value(DCKEYQTDARKACTIVECOLOR).toString() : m_XSettingsDconfig->value(DCKEYQTACTIVECOLOR).toString();
        QString value = qtActiveColorToHexColor(result);

        m_property->qtActiveColor = value;
        Q_EMIT Changed("QtActiveColor", value);
    } else if (key == DCKEYDTKWINDOWRADIUS) {
        m_property->windowRadius = m_XSettingsDconfig->value(DCKEYDTKWINDOWRADIUS).toInt();
        Q_EMIT Changed("WindowRadius", QString::number(m_property->windowRadius));
    }
}

// NOTE: it here is value change, then do change value, and emit signal
// else, just emit the signal
// All the signal will be sended on dconfig changed
void AppearanceManager::handleSettingDConfigChange(QString key)
{
    QString type;
    QString value;
    bool bSuccess = true;

    do {
        if (key == GSKEYGLOBALTHEME) {
            type = TYPEGLOBALTHEME;
            value = m_settingDconfig.value(key).toString();
            if (value == m_property->globalTheme) {
                break;
            }
            bSuccess = doSetGlobalTheme(value);
            if (bSuccess) {
                setGlobalTheme(value);
            }
        } else if (key == GSKEYGTKTHEME) {
            type = TYPEGTK;
            value = m_settingDconfig.value(key).toString();
            if (value == m_property->gtkTheme) {
                break;
            }
            bSuccess = doSetGtkTheme(value);
        } else if (key == GSKEYICONTHEM) {
            type = TYPEICON;
            value = m_settingDconfig.value(key).toString();
            if (value == m_property->iconTheme) {
                break;
            }
            bSuccess = doSetIconTheme(value);
        } else if (key == GSKEYCURSORTHEME) {
            type = TYPECURSOR;
            value = m_settingDconfig.value(key).toString();
            if (value == m_property->cursorTheme) {
                break;
            }
            bSuccess = doSetCursorTheme(value);
        } else if (key == GSKEYFONTSTANDARD) {
            type = TYPESTANDARDFONT;
            value = m_settingDconfig.value(key).toString();
            if (value == m_property->standardFont) {
                break;
            }
            bSuccess = doSetStandardFont(value);
        } else if (key == GSKEYFONTMONOSPACE) {
            type = TYPEMONOSPACEFONT;
            value = m_settingDconfig.value(key).toString();
            if (value == m_property->monospaceFont) {
                break;
            }
            bSuccess = doSetMonospaceFont(value);
        } else if (key == GSKEYFONTSIZE) {
            type = TYPEFONTSIZE;
            double size = m_settingDconfig.value(key).toDouble();
            if (size == m_property->fontSize) {
                break;
            }
            bSuccess = doSetFonts(size);
            value = QString::number(size);
        } else if (key == GSKEYBACKGROUNDURIS) {
            type = TYPEBACKGROUND;
            m_desktopBgs = m_settingDconfig.value(key).toStringList();
            if (m_settingDconfig.value(key).toString() == this->m_property->background) {
                break;
            }
            m_dbusProxy->SetDesktopBackgrounds(m_desktopBgs);
            value = m_desktopBgs.join(";");
        } else if (key == GSKEYWALLPAPERSLIDESHOW) {
            type = TYPEWALLPAPERSLIDESHOW;
            value = m_settingDconfig.value(key).toString();
            // noting to do, move to deepin-service-manager
        } else if (key == GSKEYOPACITY) {
            type = TYPEWINDOWOPACITY;
            bool ok = false;
            double opacity = m_settingDconfig.value(key).toDouble(&ok);
            if (opacity == m_property->opacity) {
                break;
            }
            if (ok) {
                setOpacity(opacity);
                value = QString::number(opacity);
            }
        } else if (key == DCKEYALLWALLPAPER) {
            type = TYPEALLWALLPAPER;
            QVariant wallpaper = m_settingDconfig.value(key);
            if (wallpaper.toJsonArray() == m_wallpaperConfig) {
                break;
            }
            utils::writeWallpaperConfig(wallpaper);
        } else if (key == DDTKSIZEMODE) {
            type = TYPEDTKSIZEMODE;
            bool ok = false;
            int mode = m_settingDconfig.value(key).toInt(&ok);
            if (ok) {
                doSetDTKSizeMode(mode);
            }
        } else if (key == DQTSCROLLBARPOLICY) {
            type = TYPEQTSCROLLBARPOLICY;
            bool ok = false;
            int policy = m_settingDconfig.value(key).toInt(&ok);
            if (ok) {
                setQtScrollBarPolicy(policy);
                value = QString::number(policy);
            }
        }
        else {
            return;
        }
    } while (false);

    if (!bSuccess) {
        qDebug() << "set " << key << "fail";
    }

    if (!type.isEmpty()) {
        Q_EMIT Changed(type, value);
    }
}

void AppearanceManager::handleDetectSysClockTimeOut()
{

    qint64 now = QDateTime::currentSecsSinceEpoch();
    qint64 diff = now - m_detectSysClockStartTime - 60;
    if (diff > -2 && diff < 2) {
        if (m_locationValid) {
            autoSetTheme(m_latitude, m_longitude);
            resetThemeAutoTimer();
        }
        m_detectSysClockStartTime = QDateTime::currentSecsSinceEpoch();
        m_detectSysClockTimer.start(60 * 1000);
    }
}

void AppearanceManager::handleUpdateToCustom(const QString &mode)
{
    m_currentGlobalTheme = "custom" + mode;
    setGlobalTheme(m_currentGlobalTheme);
}

void AppearanceManager::handleGlobalThemeChangeTimeOut()
{
    // 相同则为指定主题
    if (m_property->globalTheme == m_currentGlobalTheme || m_longitude <= NAN_ANGLE || m_latitude <= NAN_ANGLE)
        return;
    autoSetTheme(m_latitude, m_longitude);
}

void AppearanceManager::timerEvent(QTimerEvent *event)
{
    if (event->timerId() == m_timeUpdateTimeId || event->timerId() == m_ntpTimeId) {
        if (m_locationValid) {
            autoSetTheme(m_latitude, m_longitude);
            resetThemeAutoTimer();
        }

        killTimer(event->timerId());
    }
}

// 设置gsetting
void AppearanceManager::setFontSize(double value)
{
    if (!doUpdateFonts(value)) {
        return;
    }
    if (!m_fontsManager->isFontSizeValid(value)) {
        qWarning() << "set font size error:invalid size " << value;
        return;
    }

    if (m_settingDconfig.isValid() && !qFuzzyCompare(value, m_property->fontSize)) {
        m_settingDconfig.setValue(GSKEYFONTSIZE, value);
        m_property->fontSize = value;
        updateCustomTheme(TYPEFONTSIZE, QString::number(value));
    }
}

void AppearanceManager::setGlobalTheme(QString value)
{
    if (m_settingDconfig.isValid() && value != m_property->globalTheme) {
        m_settingDconfig.setValue(GSKEYGLOBALTHEME, value);
        m_property->globalTheme = value;
    }
}

void AppearanceManager::setGtkTheme(QString value)
{
    if (m_settingDconfig.isValid() && value != m_property->gtkTheme) {
        m_settingDconfig.setValue(GSKEYGTKTHEME, value);
        m_property->gtkTheme = value;
    }
}

void AppearanceManager::setIconTheme(QString value)
{
    if (m_settingDconfig.isValid() && value != m_property->iconTheme) {
        m_settingDconfig.setValue(GSKEYICONTHEM, value);
        m_property->iconTheme = value;
    }
}

void AppearanceManager::setCursorTheme(QString value)
{
    if (m_settingDconfig.isValid() && value != m_property->cursorTheme) {
        m_settingDconfig.setValue(GSKEYCURSORTHEME, value);
        m_property->cursorTheme = value;
    }
}

void AppearanceManager::setStandardFont(QString value)
{
    if (!m_fontsManager->isFontFamily(value)) {
        qWarning() << "set standard font error:invalid font " << value;
        return;
    }

    if (m_settingDconfig.isValid() && value != m_property->standardFont) {
        m_settingDconfig.setValue(GSKEYFONTSTANDARD, value);
        m_property->standardFont = value;
    }
}

void AppearanceManager::setMonospaceFont(QString value)
{
    if (!m_fontsManager->isFontFamily(value)) {
        qWarning() << "set monospace font error:invalid font " << value;
        return;
    }

    if (m_settingDconfig.isValid() && value != m_property->monospaceFont) {
        m_settingDconfig.setValue(GSKEYFONTMONOSPACE, value);
        m_property->monospaceFont = value;
    }
}

void AppearanceManager::setDTKSizeMode(int value)
{
    if (value != m_property->dtkSizeMode && m_settingDconfig.isValid()) {
        m_settingDconfig.setValue(DDTKSIZEMODE, value);
        m_property->dtkSizeMode = value;
    }
}

void AppearanceManager::setQtScrollBarPolicy(int value)
{
    if (value != m_property->qtScrollBarPolicy && m_settingDconfig.isValid()) {
        m_settingDconfig.setValue(DQTSCROLLBARPOLICY, value);
        m_property->qtScrollBarPolicy = value;
    }
}

void AppearanceManager::setActiveColors(const QString &value)
{
    m_settingDconfig.setValue(DACTIVECOLORS, value);
    QStringList colors = value.split(',');
    if (colors.isEmpty()) {
        return;
    }
    m_XSettingsDconfig->setValue(DCKEYQTACTIVECOLOR, hexColorToQtActiveColor(colors.value(0)));
    m_XSettingsDconfig->setValue(DCKEYQTDARKACTIVECOLOR, hexColorToQtActiveColor(colors.value(1)));
}

void AppearanceManager::setWindowRadius(int value)
{
    if (value != m_property->windowRadius && m_XSettingsDconfig) {
        m_XSettingsDconfig->setValue(DCKEYDTKWINDOWRADIUS, value);
        m_property->windowRadius = value;
        updateCustomTheme(TYPWINDOWRADIUS, QString::number(value));
    }
}

void AppearanceManager::setOpacity(double value)
{
    if (m_settingDconfig.isValid() && !qFuzzyCompare(value, m_property->opacity)) {
        m_settingDconfig.setValue(GSKEYOPACITY, value);
        m_property->opacity = value;
        updateCustomTheme(TYPEWINDOWOPACITY, QString::number(value));
    }
}

void AppearanceManager::setQtActiveColor(const QString &value)
{
    Q_UNUSED(value)
    QString activeColors = m_settingDconfig.value(DACTIVECOLORS).toString();
    QStringList colors = activeColors.split(',');
    QString result = m_currentGlobalTheme.endsWith("dark") ? colors.value(1) : colors.value(0);
    if (result != m_property->qtActiveColor && m_XSettingsDconfig) {
        m_property->qtActiveColor = result;
        updateCustomTheme(TYPEACTIVECOLOR, activeColors);
    }
}

bool AppearanceManager::setWallpaperSlideShow(const QString &value)
{
    if (value == m_property->wallpaperSlideShow) {
        return true;
    }
    if (!m_settingDconfig.isValid()) {
        return false;
    }
    qInfo() << "value: " << value;
    qInfo() << "value: GSKEYWALLPAPERSLIDESHOW" << m_settingDconfig.value(GSKEYWALLPAPERSLIDESHOW);
    m_settingDconfig.setValue(GSKEYWALLPAPERSLIDESHOW, value);
    m_property->wallpaperSlideShow = value;

    return true;
}

bool AppearanceManager::setWallpaperURls(const QString &value)
{
    if (value == m_property->wallpaperURls) {
        return true;
    }
    if (!m_settingDconfig.isValid()) {
        return false;
    }

    m_settingDconfig.setValue(GSKEYWALLPAPERURIS, value);
    m_property->wallpaperURls = value;

    return true;
}

QString AppearanceManager::qtActiveColorToHexColor(const QString &activeColor)
{
    QStringList fields = activeColor.split(",");
    if (fields.size() != 4)
        return "";

    QColor clr = QColor::fromRgba64(fields.at(0).toUShort(), fields.at(1).toUShort(), fields.at(2).toUShort(), fields.at(3).toUShort());
    return clr.name(clr.alpha() == 255 ? QColor::HexRgb : QColor::HexArgb).toUpper();
}

QString AppearanceManager::hexColorToQtActiveColor(const QString &hexColor)
{
    if (!QColor::isValidColorName(hexColor))
        return QString();
    QColor clr(hexColor);
    QStringList rgbaList;
    QRgba64 clr64 = clr.rgba64();
    rgbaList.append(QString::number(clr64.red()));
    rgbaList.append(QString::number(clr64.green()));
    rgbaList.append(QString::number(clr64.blue()));
    rgbaList.append(QString::number(clr64.alpha()));
    return rgbaList.join(",");
}

void AppearanceManager::initCoordinate()
{
    QString context;
    QString zonepath = ZONEPATH;
    if (qEnvironmentVariableIsSet("TZDIR"))
        zonepath = qEnvironmentVariable("TZDIR") + "/zone1970.tab";
    QFile file(zonepath);
    if (!file.open(QIODevice::ReadOnly | QIODevice::Text)) {
        return;
    }

    while (!file.atEnd()) {
        QString line = file.readLine();
        if (line.length() == 0) {
            continue;
        }
        line = line.trimmed();
        if (line.startsWith("#")) {
            continue;
        }

        QStringList strv = line.split("\t");
        if (strv.size() < 3) {
            continue;
        }

        iso6709Parsing(strv[2], strv[1]);
    }

    QString city = m_dbusProxy->timezone();
    if (m_coordinateMap.count(city) == 1) {
        m_latitude = m_coordinateMap[city].latitude;
        m_longitude = m_coordinateMap[city].longitude;
    }
}

void AppearanceManager::initUserObj()
{
    qInfo() << "initUserObj";
    struct passwd *pw = getpwuid(getuid());
    if (pw == nullptr) {
        return;
    }

    QString userPath = AppearanceDBusProxy::FindUserById(QString::number(pw->pw_uid));

    m_dbusProxy->setUserInterface(userPath);

    QStringList userBackgrounds = m_dbusProxy->desktopBackgrounds();

    QStringList gsBackgrounds = m_settingDconfig.value(GSKEYBACKGROUNDURIS).toStringList();
    for (auto iter : gsBackgrounds) {
        if (userBackgrounds.indexOf(iter) == -1) {
            m_dbusProxy->SetDesktopBackgrounds(gsBackgrounds);
            break;
        }
    }
}

void AppearanceManager::initCurrentBgs()
{
    qInfo() << "initCurrentBgs";
    m_desktopBgs = m_settingDconfig.value(GSKEYBACKGROUNDURIS).toStringList();

    m_greeterBg = m_dbusProxy->greeterBackground();
}


void AppearanceManager::updateMonitorMap()
{
    QString primary = m_dbusProxy->primary();
    QStringList monitorList = m_dbusProxy->ListOutputNames();
    for (int i = 0; i < monitorList.size(); i++) {
        if (monitorList[i] == primary) {
            m_monitorMap[monitorList[i]] = "Primary";
        } else {
            m_monitorMap[monitorList[i]] = "Subsidiary" + QString::number(i);
        }
    }
}

void AppearanceManager::iso6709Parsing(QString city, QString coordinates)
{
    QRegularExpression pattern(R"((\+|-)\d+\.?\d*)");

    QVector<QString> resultVet;

    QRegularExpressionMatchIterator it = pattern.globalMatch(coordinates);
    while (it.hasNext() && resultVet.size() <= 2) {
        QRegularExpressionMatch match = it.next();
        resultVet.push_back(match.captured(0));
    }

    if (resultVet.size() < 2) {
        return;
    }

    resultVet[0] = resultVet[0].mid(0, 3) + "." + resultVet[0].mid(3, resultVet[0].size());
    resultVet[1] = resultVet[1].mid(0, 4) + "." + resultVet[1].mid(4, resultVet[1].size());

    coordinate cdn;

    cdn.latitude = resultVet[0].toDouble();
    cdn.longitude = resultVet[1].toDouble();

    m_coordinateMap[city] = cdn;
}

int AppearanceManager::getWorkspaceCount()
{
    const auto count = m_dbusProxy->WorkspaceCount();
    return count > 0 ? count : DEFAULT_WORKSPACE_COUNT;
}

void AppearanceManager::doUpdateWallpaperURIs()
{
    QMap<QString, QString> monitorWallpaperUris;

    QStringList monitorList = m_dbusProxy->ListOutputNames();

    for (int i = 0; i < monitorList.length(); i++) {
        for (int idx = 1; idx <= getWorkspaceCount(); idx++) {
            QString wallpaperUri = getWallpaperUri(QString::number(idx), monitorList.at(i));
            if (wallpaperUri.isEmpty())
                continue;

            QString key;
            if (m_monitorMap.count(monitorList[i]) != 0) {
                key = QString::asprintf("%s&&%d", m_monitorMap[monitorList[i]].toLatin1().data(), idx);
            } else {
                key = QString::asprintf("&&%d", idx);
            }

            monitorWallpaperUris[key] = wallpaperUri;
        }
    }

    setPropertyWallpaperURIs(monitorWallpaperUris);

    if (monitorWallpaperUris.isEmpty()) {
        return;
    }
}

void AppearanceManager::setPropertyWallpaperURIs(QMap<QString, QString> monitorWallpaperUris)
{
    QJsonDocument doc;
    QJsonObject monitorObj;
    for (auto iter : monitorWallpaperUris.toStdMap()) {
        monitorObj.insert(iter.first, iter.second);
    }

    doc.setObject(monitorObj);
    QString wallPaperUriVal = doc.toJson(QJsonDocument::Compact);
    if (m_settingDconfig.isValid() && wallPaperUriVal != m_property->wallpaperURls) {
        m_settingDconfig.setValue(GSKEYWALLPAPERURIS, wallPaperUriVal);
        m_property->wallpaperURls = wallPaperUriVal;
    }
}

void AppearanceManager::updateNewVersionData()
{
    QString primaryMonitor;
    for (auto item : m_monitorMap.toStdMap()) {
        if (item.second == "Primary") {
            primaryMonitor = item.first;
        }
    }
    QJsonDocument doc = QJsonDocument::fromJson(m_property->wallpaperSlideShow->toLatin1());
    QJsonObject wallPaperSlideObj;
    const auto workspaceCount = getWorkspaceCount();
    if (!doc.isEmpty()) {
        for (int i = 1; i <= workspaceCount; i++) {
            QString key = QString("%1&&%2").arg(primaryMonitor).arg(i);
            wallPaperSlideObj.insert(key, m_property->wallpaperSlideShow.data());
        }

        QJsonDocument tempDoc(wallPaperSlideObj);

        if (!setWallpaperSlideShow(tempDoc.toJson(QJsonDocument::Compact))) {
            return;
        }
    }

    QJsonObject wallpaperURIsObj;
    for (auto item : m_monitorMap.toStdMap()) {
        for (int i = 1; i <= workspaceCount; i++) {
            QString wallpaperURI = doGetWorkspaceBackgroundForMonitor(i, item.first);
            if (wallpaperURI.isEmpty())
                continue;
            QString key = QString("%1&&%2").arg(item.second).arg(i);
            wallpaperURIsObj.insert(key, wallpaperURI);
        }
    }

    QJsonDocument tempDoc(wallpaperURIsObj);
    setWallpaperURls(tempDoc.toJson(QJsonDocument::Compact));
}

void AppearanceManager::autoSetTheme(double latitude, double longitude)
{
    QDateTime curr = QDateTime::currentDateTime();
    double utcOffset = curr.offsetFromUtc() / 3600.0;

    QDateTime sunrise, sunset;
    bool bSuccess = SunriseSunset::getSunriseSunset(latitude, longitude, utcOffset, curr.date(), sunrise, sunset);
    if (!bSuccess) {
        return;
    }
    QString themeName;
    if (sunrise.secsTo(curr) >= 0 && curr.secsTo(sunset) >= 0) {
        themeName = m_property->globalTheme + ".light";
    } else {
        themeName = m_property->globalTheme + ".dark";
    }

    if (m_currentGlobalTheme != themeName) {
        doSetGlobalTheme(themeName);
    }
}

void AppearanceManager::resetThemeAutoTimer()
{
    if (!m_locationValid) {
        qDebug() << "location is invalid";
        return;
    }

    QDateTime curr = QDateTime::currentDateTime();
    QDateTime changeTime = getThemeAutoChangeTime(curr, m_latitude, m_longitude);

    qint64 interval = curr.msecsTo(changeTime);
    qDebug() << "change theme after:" << interval << curr << changeTime;
}

void AppearanceManager::updateThemeAuto(bool enable)
{
    enableDetectSysClock(enable);

    if (enable) {
        QString city = m_dbusProxy->timezone();
        if (m_coordinateMap.count(city) == 1) {
            m_latitude = m_coordinateMap[city].latitude;
            m_longitude = m_coordinateMap[city].longitude;
        }
        m_locationValid = true;
        autoSetTheme(m_latitude, m_longitude);
        resetThemeAutoTimer();
    }
}

void AppearanceManager::enableDetectSysClock(bool enable)
{
    if (enable) {
        m_detectSysClockTimer.start(60 * 1000);
    } else {
        m_detectSysClockTimer.stop();
    }
}


QDateTime AppearanceManager::getThemeAutoChangeTime(QDateTime date, double latitude, double longitude)
{
    Q_UNUSED(date);
    QDateTime curr = QDateTime::currentDateTime();

    double utcOffset = curr.offsetFromUtc() / 3600.0;

    QDateTime sunrise, sunset;
    bool bSuccess = SunriseSunset::getSunriseSunset(latitude, longitude, utcOffset, curr.date(), sunrise, sunset);
    if (!bSuccess) {
        return QDateTime();
    }

    if (curr.secsTo(sunrise) > 0) {
        return sunrise;
    }

    if (curr.secsTo(sunset) > 0) {
        return sunset;
    }

    curr = curr.addDays(1);

    bSuccess = SunriseSunset::getSunriseSunset(latitude, longitude, utcOffset, curr.date(), sunrise, sunset);
    if (!bSuccess) {
        return QDateTime();
    }

    return sunrise;
}

bool AppearanceManager::doUpdateFonts(double size)
{
    if (!m_fontsManager->isFontSizeValid(size)) {
        qWarning() << "set font size error:invalid size " << size;
        return false;
    }

    qDebug() << "doSetFonts, standardFont:" << m_property->standardFont << ", property->monospaceFont:" << m_property->monospaceFont;
    bool bSuccess = m_fontsManager->setFamily(m_property->standardFont, m_property->monospaceFont, size);
    if (!bSuccess) {
        qWarning() << "set font size error:can not set family ";
        return false;
    }
    m_dbusProxy->SetString("Qt/FontPointSize", QString::number(size));
    if (!setDQtTheme({ QTKEYFONTSIZE }, { QString::number(size) })) {
        qWarning() << "set font size error:can not set qt theme ";
        return false;
    }
    return true;
}

bool AppearanceManager::doSetFonts(double size)
{
    if (!doUpdateFonts(size)) {
        return false;
    }
    setFontSize(size);
    return true;
}

bool AppearanceManager::doSetGlobalTheme(QString value)
{
    enum GolbalThemeMode {
        Light = 1,
        Dark = 2,
        Auto = 3,
    };

    QString themeId = value;
    GolbalThemeMode mode = Auto;
    if (value.endsWith(".light")) {
        themeId = value.chopped(6);
        mode = Light;
    } else if (value.endsWith(".dark")) {
        themeId = value.chopped(5);
        mode = Dark;
    }

    QVector<QSharedPointer<Theme>> globalThemes = m_subthemes->listGlobalThemes();
    QString themePath;
    for (auto iter : globalThemes) {
        if (iter->getId() == themeId) {
            themePath = iter->getPath();
            break;
        }
    }
    if (themePath.isEmpty())
        return false;

    KeyFile theme(',');
    theme.loadFile(themePath + "/index.theme");
    QString defaultTheme = theme.getStr("Deepin Theme", "DefaultTheme");
    QString lightActiveColor;
    if (defaultTheme.isEmpty()) {
        return false;
    } else {
        lightActiveColor = theme.getStr(defaultTheme, "ActiveColor");
    }

    QString darkTheme = theme.getStr("Deepin Theme", "DarkTheme");
    QString darkActiveColor;
    if (darkTheme.isEmpty()) {
        mode = Light;
    } else {
        darkActiveColor = theme.getStr(darkTheme, "ActiveColor", lightActiveColor);
    }

    setActiveColors(lightActiveColor + "," + darkActiveColor);

    m_currentGlobalTheme = value;
    switch (mode) {
    case Light:
        applyGlobalTheme(theme, defaultTheme, defaultTheme, themePath, themeId);
        break;
    case Dark: {
        if (darkTheme.isEmpty())
            return false;
        applyGlobalTheme(theme, darkTheme, defaultTheme, themePath, themeId);
    } break;
    case Auto: {
        setGlobalTheme(value);
        updateThemeAuto(true);
    } break;
    }

    return true;
}

bool AppearanceManager::doSetGtkTheme(QString value)
{
    if (value == AUTOGTKTHEME) {
        return true;
    }

    if (!m_subthemes->isGtkTheme(value)) {
        return false;
    }
    QString ddeKWinTheme;
    if (value == DEEPIN) {
        ddeKWinTheme = "light";
    } else if (value == DEEPINDARK) {
        ddeKWinTheme = "dark";
    }

    if (!ddeKWinTheme.isEmpty()) {
        m_dbusProxy->SetDecorationDeepinTheme(ddeKWinTheme);
    }
    setGtkTheme(value);
    return m_subthemes->setGtkTheme(value);
}

bool AppearanceManager::doSetIconTheme(QString value)
{
    if (!m_subthemes->isIconTheme(value)) {
        return false;
    }

    if (!m_subthemes->setIconTheme(value)) {
        return false;
    }

    setIconTheme(value);
    return setDQtTheme({ QTKEYICON }, { value });
}

bool AppearanceManager::doSetCursorTheme(QString value)
{
    if (!m_subthemes->isCursorTheme(value)) {
        return false;
    }

    setCursorTheme(value);
    return m_subthemes->setCursorTheme(value);
}

bool AppearanceManager::doSetStandardFont(QString value)
{
    if (!m_fontsManager->isFontFamily(value)) {
        qWarning() << "set standard font error:invalid font " << value;
        return false;
    }
    QString tmpMonoFont = m_property->monospaceFont;
    QStringList fontList = m_fontsManager->listMonospace();
    if (tmpMonoFont.isEmpty() && !fontList.isEmpty()) {
        tmpMonoFont = fontList[0];
    }

    qDebug() << "doSetStandardFont standardFont:" << m_property->standardFont << ", monospaceFont:" << tmpMonoFont;
    if (!m_fontsManager->setFamily(value, tmpMonoFont, m_property->fontSize)) {
        qWarning() << "set standard font error:can not set family ";
        return false;
    }
    m_dbusProxy->SetString("Qt/FontName", value);
    if (!setDQtTheme({ QTKEYFONT }, { value })) {
        qWarning() << "set standard font error:can not set qt theme ";
        return false;
    }
    return true;
}

bool AppearanceManager::doSetMonospaceFont(QString value)
{
    if (!m_fontsManager->isFontFamily(value)) {
        return false;
    }
    QString tmpStandardFont = m_property->standardFont;
    QStringList fontList = m_fontsManager->listStandard();
    if (tmpStandardFont.isEmpty() && !fontList.isEmpty()) {
        tmpStandardFont = fontList[0];
    }

    qDebug() << "doSetMonospaceFont, standardFont:" << tmpStandardFont << ", monospaceFont:" << m_property->monospaceFont;
    if (!m_fontsManager->setFamily(tmpStandardFont, value, m_property->fontSize)) {
        qWarning() << "set monospace font error:can not set family ";
        return false;
    }

    m_dbusProxy->SetString("Qt/MonoFontName", value);
    if (!setDQtTheme({ QTKEYMONOFONT }, { value })) {
        qWarning() << "set monospace font error:can not set qt theme ";
        return false;
    }

    return true;
}

bool AppearanceManager::doSetBackground(QString value)
{
    if (checkWallpaperLockedStatus()) {
        return false;
    }

    if (!m_backgrounds->isBackgroundFile(value)) {
        return false;
    }

    QString file = m_backgrounds->prepare(value);
    QString uri = utils::enCodeURI(file, SCHEME_FILE);

    m_dbusProxy->ChangeCurrentWorkspaceBackground(uri);

    m_dbusProxy->Get(file);

    m_dbusProxy->Get("", file);

    return true;
}

bool AppearanceManager::doSetGreeterBackground(QString value)
{
    if (checkWallpaperLockedStatus()) {
        return false;
    }
    value = utils::enCodeURI(value, SCHEME_FILE);
    m_greeterBg = value;
    m_dbusProxy->SetGreeterBackground(value);

    if (!m_globalThemeUpdating) {
        m_settingDconfig.setValue(DCKEYISCUSTOMLOCKBACKGROUND, true);
    }

    return true;
}

QString AppearanceManager::doGetWallpaperSlideShow(QString monitorName)
{
    int index = m_dbusProxy->GetCurrentWorkspace();

    QJsonDocument doc = QJsonDocument::fromJson(m_property->wallpaperSlideShow->toLatin1());
    QVariantMap tempMap = doc.object().toVariantMap();

    QString key = QString("%1&&%2").arg(monitorName).arg(index);

    if (tempMap.count(key) == 1) {
        return tempMap[key].toString();
    }

    return "";
}

double AppearanceManager::getScaleFactor()
{
    double scaleFactor = 0.0;
    if (m_XSettingsDconfig) {
        scaleFactor = m_XSettingsDconfig->value("scale-factor").toDouble();
    } else {
        scaleFactor = m_dbusProxy->GetScaleFactor();
    }
    qInfo() << __FUNCTION__ << "UpdateScaleFactor" << scaleFactor;
    if (scaleFactor <= 0) {
        scaleFactor = 1.0;
    }
    UpdateScaleFactor(scaleFactor);
    return scaleFactor;
}

ScaleFactors AppearanceManager::getScreenScaleFactors()
{
    return m_dbusProxy->GetScreenScaleFactors();
}

bool AppearanceManager::setScaleFactor(double scale)
{
    m_dbusProxy->SetScaleFactor(scale);
    return true;
}

bool AppearanceManager::setScreenScaleFactors(ScaleFactors scaleFactors)
{
    m_dbusProxy->SetScreenScaleFactors(scaleFactors);
    return true;
}

QString AppearanceManager::doList(QString type)
{
    if (type == TYPEGTK) {
        QVector<QSharedPointer<Theme>> gtks = m_subthemes->listGtkThemes();

        QVector<QSharedPointer<Theme>>::iterator iter = gtks.begin();
        while (iter != gtks.end()) {
            if ((*iter)->getId().startsWith("deepin")) {
                ++iter;
            } else {
                iter = gtks.erase(iter);
            }
        }
        return marshal(gtks);
    } else if (type == TYPEICON) {
        return marshal(m_subthemes->listIconThemes());
    } else if (type == TYPECURSOR) {
        return marshal(m_subthemes->listCursorThemes());
    } else if (type == TYPEBACKGROUND) {
        return marshal(backgroundListVerify(m_backgrounds->listBackground()));
    } else if (type == TYPESTANDARDFONT) {
        m_fontsManager->refreshFamilyList();
        return marshal(m_fontsManager->listStandard());
    } else if (type == TYPEMONOSPACEFONT) {
        m_fontsManager->refreshFamilyList();
        return marshal(m_fontsManager->listMonospace());
    } else if (type == TYPEGLOBALTHEME) {
        return marshal(m_subthemes->listGlobalThemes());
    }

    return "";
}

QString AppearanceManager::doShow(const QString &type, const QStringList &names)
{
    if (type == TYPEGTK) {
        QVector<QSharedPointer<Theme>> gtks = m_subthemes->listGtkThemes();

        QVector<QSharedPointer<Theme>>::iterator iter = gtks.begin();
        while (iter != gtks.end()) {
            if (names.indexOf((*iter)->getId()) != -1 || (*iter)->getId() == AUTOGTKTHEME) {
                ++iter;
            } else {
                iter = gtks.erase(iter);
            }
        }
        return marshal(gtks);
    } else if (type == TYPEICON) {
        QVector<QSharedPointer<Theme>> icons = m_subthemes->listIconThemes();

        QVector<QSharedPointer<Theme>>::iterator iter = icons.begin();
        while (iter != icons.end()) {
            if (names.indexOf((*iter)->getId()) != -1) {
                ++iter;
            } else {
                iter = icons.erase(iter);
            }
        }
        return marshal(icons);
    } else if (type == TYPEGLOBALTHEME) {
        QVector<QSharedPointer<Theme>> globalThemes = m_subthemes->listGlobalThemes();

        QVector<QSharedPointer<Theme>>::iterator iter = globalThemes.begin();
        while (iter != globalThemes.end()) {
            if (names.indexOf((*iter)->getId()) != -1) {
                ++iter;
            } else {
                iter = globalThemes.erase(iter);
            }
        }
        return marshal(globalThemes);
    } else if (type == TYPECURSOR) {
        QVector<QSharedPointer<Theme>> cursor = m_subthemes->listCursorThemes();

        QVector<QSharedPointer<Theme>>::iterator iter = cursor.begin();
        while (iter != cursor.end()) {
            if (names.indexOf((*iter)->getId()) != -1) {
                ++iter;
            } else {
                iter = cursor.erase(iter);
            }
        }
        return marshal(cursor);
    } else if (type == TYPEBACKGROUND) {
        QVector<Background> background = m_backgrounds->listBackground();

        QVector<Background>::iterator iter = background.begin();
        while (iter != background.end()) {
            if (names.indexOf(iter->getId()) != -1) {
                ++iter;
            } else {
                iter = background.erase(iter);
            }
        }
        return marshal(background);
    } else if (type == TYPESTANDARDFONT) {
        return marshal(m_fontsManager->getFamilies(names));
    } else if (type == TYPEMONOSPACEFONT) {
        return marshal(m_fontsManager->getFamilies(names));
    }

    return "";
}

void AppearanceManager::doResetSettingBykeys(QStringList keys)
{
    QStringList keyList = m_settingDconfig.keyList();
    for (auto item : keys) {
        if (!keyList.contains(item)) {
            continue;
        }
        m_settingDconfig.reset(item);
    }
}

void AppearanceManager::doResetFonts()
{
    bool bSuccess = m_fontsManager->reset();
    if (!bSuccess) {
        return;
    }

    m_fontsManager->checkFontConfVersion();
}

void AppearanceManager::doSetByType(const QString &type, const QString &value)
{
    bool updateValut = false;
    if (type == TYPEGTK) {
        if (!m_setDefaulting && value == m_property->gtkTheme) {
            return;
        }

        if (doSetGtkTheme(value)) {
            setGtkTheme(value);
            updateValut = true;
        }
    } else if (type == TYPEICON) {
        if (!m_setDefaulting && value == m_property->iconTheme) {
            return;
        }

        if (doSetIconTheme(value)) {
            setIconTheme(value);
            updateValut = true;
        }
    } else if (type == TYPECURSOR) {
        if (!m_setDefaulting && value == m_property->cursorTheme) {
            return;
        }

        if (doSetCursorTheme(value)) {
            setCursorTheme(value);
            updateValut = true;
        }
    } else if (type == TYPEGLOBALTHEME) {
        if (!m_setDefaulting && value == m_property->globalTheme) {
            return;
        }
        if (doSetGlobalTheme(value)) {
            setGlobalTheme(value);
        }
    } else if (type == TYPEBACKGROUND) {
        bool bSuccess = doSetBackground(value);
        if (bSuccess) {
            updateValut = true;
        }
    } else if (type == TYPEGREETERBACKGROUND) {
        doSetGreeterBackground(value);
    } else if (type == TYPESTANDARDFONT) {
        if (!m_setDefaulting &&m_property->standardFont == value) {
            return;
        }
        if (doSetStandardFont(value)) {
            setStandardFont(value);
            updateValut = true;
        }
    } else if (type == TYPEMONOSPACEFONT) {
        if (!m_setDefaulting && m_property->monospaceFont == value) {
            return;
        }
        if (doSetMonospaceFont(value)) {
            setMonospaceFont(value);
            updateValut = true;
        }
    } else if (type == TYPEFONTSIZE) {
        double size = value.toDouble();
        if (!m_setDefaulting && m_property->fontSize > size - 0.01 && m_property->fontSize < size + 0.01) {
            return;
        }
        setFontSize(size);
    } else if (type == TYPEACTIVECOLOR) {
        setQtActiveColor(value);
    } else if (type == TYPWINDOWRADIUS) {
        bool ok = false;
        int radius = value.toInt(&ok);
        if (ok) {
            setWindowRadius(radius);
        }
    } else if (type == TYPEWINDOWOPACITY) {
        bool ok = false;
        double opacity = value.toDouble(&ok);
        if (ok) {
            setOpacity(opacity);
        }
    } else if (type == TYPEWALLPAPER) {
        doSetCurrentWorkspaceBackground(value);
    } else if (type == TYPEDTKSIZEMODE) {
        bool ok = false;
        int mode = value.toInt(&ok);
        if (ok) {
            QString fontSizeKey = GSKEYFONTSIZE;
            doSetDTKSizeMode(mode);
        }
    } else if (type == TYPEQTSCROLLBARPOLICY) {
        bool ok = false;
        int policy = value.toInt(&ok);
        if (ok) {
            doSetQtScrollBarPolicy(policy);
        }
    }

    if (updateValut) {
        updateCustomTheme(type, value);
    }
}

QString AppearanceManager::doSetMonitorBackground(const QString &monitorName, const QString &imageGile)
{
    if (!m_backgrounds->isBackgroundFile(imageGile)) {
        return QString();
    }

    QString file = m_backgrounds->prepare(imageGile);

    QString uri = utils::enCodeURI(file, SCHEME_FILE);
    doSetCurrentWorkspaceBackgroundForMonitor(uri, monitorName);
    return uri;
}

QString AppearanceManager::doThumbnail(const QString &type, const QString &name)
{
    if (type == TYPEGTK) {
        QMap<QString, QString> gtkThumbnailMap = m_subthemes->getGtkThumbnailMap();
        if (gtkThumbnailMap.count(name) == 1) {
            return "/usr/share/dde-daemon/appearance/" + gtkThumbnailMap[name] + ".svg";
        }
        return m_subthemes->getGtkThumbnail(name);
    } else if (type == TYPEICON) {
        return m_subthemes->getIconThumbnail(name);
    } else if (type == TYPECURSOR) {
        return m_subthemes->getCursorThumbnail(name);
    } else if (type == TYPEGLOBALTHEME) {
        return m_subthemes->getGlobalThumbnail(name);
    } else {
        return QString("invalid type: %1").arg(type);
    }
}

bool AppearanceManager::doSetWallpaperSlideShow(const QString &monitorName, const QString &wallpaperSlideShow)
{
    int idx = m_dbusProxy->GetCurrentWorkspace();

    QJsonDocument doc = QJsonDocument::fromJson(wallpaperSlideShow.toLatin1());
    QJsonObject cfgObj = doc.object();

    QString key = QString("%1&&%2").arg(monitorName).arg(idx);

    cfgObj[key] = wallpaperSlideShow;

    QJsonDocument docTmp;
    docTmp.setObject(cfgObj);
    QString value = docTmp.toJson(QJsonDocument::Compact);

    setWallpaperSlideShow(value);

    m_curMonitorSpace = key;

    return true;
}

int AppearanceManager::getCurrentDesktopIndex()
{
    if (utils::isTreeland()) {
        // TODO: Need to implement
        return 0;
    } else {
        return KX11Extras::currentDesktop();
    }
}

void AppearanceManager::applyGlobalTheme(KeyFile &theme, const QString &themeName, const QString &defaultTheme, const QString &themePath, const QString &themeId)
{
    m_globalThemeUpdating = true;
    QString defTheme = (defaultTheme.isEmpty() || defaultTheme == themeName) ? QString() : defaultTheme;
    auto getValue = [&theme, &themeId, this](const QString &section, const QString &key) -> QString{
        QString ret = theme.getStr(section, key);
        for (auto &overrideItem : this->m_globalThemeOverrideMap.value(themeId)) {
            if (overrideItem.section == section && overrideItem.key == key) {
                ret = overrideItem.value;
                break;
            }
        }
        return ret;
    };
    // 设置globlaTheme的一项，先从themeName中找对应项，若没有则从defTheme中找对应项，最后调用doSetByType实现功能
    auto setGlobalItem = [&themeName, &defTheme, getValue, this](const QString &key, const QString &type) {
        QString themeValue = getValue(themeName, key);
        if (themeValue.isEmpty() && !defTheme.isEmpty())
            themeValue = getValue(defTheme, key);
        if (!themeValue.isEmpty())
            doSetByType(type, themeValue);
    };
    auto setGlobalFile = [&themeName, &defTheme, getValue, &themePath, this](const QString &key, const QString &type) {
        QString themeValue = getValue(themeName, key);
        if (themeValue.isEmpty() && !defTheme.isEmpty())
            themeValue = getValue(defTheme, key);
        if (!themeValue.isEmpty()) {
            themeValue = utils::deCodeURI(themeValue);
            QFileInfo fileInfo(themeValue);
            if (!fileInfo.isAbsolute()) {
                themeValue = themePath + "/" + themeValue;
            }
            doSetByType(type, themeValue);
        }
    };

    bool configEmpty = m_property->wallpaperURls->isEmpty();
    bool onlySetThemeType = m_property->globalTheme->startsWith(themeId);
    bool isCustom = PhaseWallPaper::isCustomWallpaper(QString::number(getCurrentDesktopIndex()), m_dbusProxy->primary());
    bool isCustomLockBackground = m_settingDconfig.value(DCKEYISCUSTOMLOCKBACKGROUND).toBool();

    // 1. 新装镜像，配置为空时设置一次初始化配置
    // 2. 若自定义过壁纸(考虑不同屏幕不同工作区)，且当前仅在设置深浅色，则不设置壁纸
    if (configEmpty || !(onlySetThemeType && isCustom))
        setGlobalFile("Wallpaper", TYPEWALLPAPER);

    // 同上，但锁屏壁纸不考虑屏幕和工作区
    if (m_greeterBg.isEmpty() || !(isCustomLockBackground && onlySetThemeType)) {
        setGlobalFile("LockBackground", TYPEGREETERBACKGROUND);
        m_settingDconfig.setValue(DCKEYISCUSTOMLOCKBACKGROUND, false);
    }

    setGlobalItem("IconTheme", TYPEICON);
    setGlobalItem("CursorTheme", TYPECURSOR);
    setGlobalItem("AppTheme", TYPEGTK);
    setGlobalItem("StandardFont", TYPESTANDARDFONT);
    setGlobalItem("MonospaceFont", TYPEMONOSPACEFONT);
    setGlobalItem("FontSize", TYPEFONTSIZE);
    setGlobalItem("ActiveColor", TYPEACTIVECOLOR);
    setGlobalItem("WindowRadius", TYPWINDOWRADIUS);
    setGlobalItem("WindowOpacity", TYPEWINDOWOPACITY);
    m_globalThemeUpdating = false;
}

bool AppearanceManager::checkWallpaperLockedStatus()
{
    if (QFileInfo::exists("/var/lib/deepin/permission-manager/wallpaper_locked")) {
        QDBusInterface notify("org.freedesktop.Notifications", "/org/freedesktop/Notifications", "org.freedesktop.Notifications");
        notify.asyncCall(QString("Notify"),
                         QString("org.deepin.dde.control-center"),   // title
                         static_cast<uint>(0),
                         QString("preferences-system"),   // icon
                         QObject::tr("This system wallpaper is locked. Please contact your admin."),
                         QString(), QStringList(), QVariantMap(), 5000);
        qInfo() << "wallpaper is locked..";
        return true;
    }
    return false;
}

void AppearanceManager::updateCustomTheme(const QString &type, const QString &value)
{
    if (!m_globalThemeUpdating) {
        m_customTheme->updateValue(type, value, m_property->globalTheme, m_subthemes->listGlobalThemes());
    }
}

bool AppearanceManager::isBgInUse(const QString &file)
{
    if (file == m_greeterBg) {
        return true;
    }
    for (auto bg : m_desktopBgs) {
        if (bg == file) {
            return true;
        }
    }
    return false;
}

QVector<Background> AppearanceManager::backgroundListVerify(const QVector<Background> &backgrounds)
{
    QVector<Background> bgs = backgrounds;
    for (Background &bg : bgs) {
        if (bg.getDeleteable()) {
            if (isBgInUse(bg.getId())) {
                bg.setDeletable(false);
            }
        }
    }
    return bgs;
}

QString AppearanceManager::getWallpaperUri(const QString &index, const QString &monitorName)
{
    bool ok;
    index.toInt(&ok);
    if (!ok)
        return QString();

    QString wallpaper = PhaseWallPaper::getWallpaperUri(index, monitorName);
    if (wallpaper.isEmpty()) {
        // 如果为空则随机给一个
        QVector<Background> backgroudlist = m_backgrounds->listBackground();
        QVariant wallpaperVar = m_settingDconfig.value(DCKEYALLWALLPAPER);
        QString value = QJsonDocument::fromVariant(wallpaperVar).toJson();
        QStringList bglist;
        for (auto &&bg : backgroudlist) {
            const QString &id = bg.getId();
            if (!value.contains(id)) {
                bglist.append(id);
            }
        }

        if (!bglist.isEmpty()) {
            wallpaper = bglist.at(QRandomGenerator::global()->generate() % bglist.size());
        } else if (!backgroudlist.isEmpty()) {
            const Background &bg = backgroudlist.at(QRandomGenerator::global()->generate() % backgroudlist.size());
            wallpaper = bg.getId();
        } else {
            wallpaper = "file:///usr/share/wallpapers/deepin/desktop.jpg";
        }

        if (auto value = PhaseWallPaper::setWallpaperUri(index, monitorName, wallpaper); value.has_value()) {
            m_wallpaperConfig = value.value();
        }
    }
    return wallpaper;
}

void AppearanceManager::initDtkSizeMode(){
    m_dbusProxy->SetInteger("DTK/SizeMode",m_property->dtkSizeMode);
}

void AppearanceManager::initGlobalTheme()
{
    QVector<QSharedPointer<Theme>> globalList = m_subthemes->listGlobalThemes();
    bool bFound = false;

    if (m_property->globalTheme->isEmpty())
        m_property->globalTheme = DEFAULTGLOBALTHEME;

    QString globalID = m_property->globalTheme->split(".").first();
    for (auto theme : globalList) {
        if (theme->getId() == globalID) {
            bFound = true;
            break;
        }
    }
    m_setDefaulting = true;
    if (!bFound) {
        for (auto theme : globalList) {
            if (theme->getId() == DEFAULTGLOBALTHEME) {
                bFound = true;
                break;
            }
        }
        if (bFound) {
            doSetGlobalTheme(DEFAULTGLOBALTHEME);
            setGlobalTheme(DEFAULTGLOBALTHEME);
        } else if (!globalList.isEmpty()) {
            doSetGlobalTheme(globalList.first()->getId());
            setGlobalTheme(globalList.first()->getId());
        } else {
            setGlobalTheme(DEFAULTGLOBALTHEME);
        }
    } else {
        // 初始化m_currentGlobalTheme
        if (m_currentGlobalTheme.isEmpty())
            doSetGlobalTheme(m_property->globalTheme);
    }
    m_setDefaulting = false;
}

void AppearanceManager::doSetCurrentWorkspaceBackground(const QString &uri)
{
    return doSetCurrentWorkspaceBackgroundForMonitor(uri, m_dbusProxy->primary());
}

QString AppearanceManager::doGetCurrentWorkspaceBackground()
{
    QString strIndex = QString::number(getCurrentDesktopIndex());
    if (strIndex == "") {
        qWarning() << "error getting current desktop index through wm.";
        return "";
    }

    return getWallpaperUri(strIndex, m_dbusProxy->primary());
}

void AppearanceManager::doSetCurrentWorkspaceBackgroundForMonitor(const QString &uri, const QString &strMonitorName)
{
    if (checkWallpaperLockedStatus()) {
        return;
    }
    QString strIndex = QString::number(getCurrentDesktopIndex());
    if (strIndex == "") {
        qWarning() << "error getting current desktop index through wm";
        return;
    }

    if (auto value = PhaseWallPaper::setWallpaperUri(strIndex, strMonitorName, uri, !m_globalThemeUpdating); value.has_value()) {
        m_wallpaperConfig = value.value();
    }

    // TODO delete, 临时适配Treeland
    if (utils::isTreeland()) {
        QString arg = QString("personalization/wallpaper?url=%1").arg(uri);
        DDBusSender()
                .service("org.deepin.dde.ControlCenter1")
                .interface("org.deepin.dde.ControlCenter1")
                .path("/org/deepin/dde/ControlCenter1")
                .method("ShowPage")
                .arg(arg)
                .call();
    }

    doUpdateWallpaperURIs();
    return;
}

QString AppearanceManager::doGetCurrentWorkspaceBackgroundForMonitor(const QString &strMonitorName)
{
    QString strIndex = QString::number(getCurrentDesktopIndex());
    if (strIndex == "") {
        qWarning() << "error getting current desktop index through wm.";
        return "";
    }

    return getWallpaperUri(strIndex, strMonitorName);
}

void AppearanceManager::doSetWorkspaceBackgroundForMonitor(const int &index, const QString &strMonitorName, const QString &uri)
{
    if (checkWallpaperLockedStatus()) {
        return;
    }
 
    if (auto value = PhaseWallPaper::setWallpaperUri(QString::number(index), strMonitorName, uri, !m_globalThemeUpdating); value.has_value()) {
        m_wallpaperConfig = value.value();
    }
        // TODO delete, 临时适配Treeland
    if (utils::isTreeland()) {
        QString arg = QString("personalization/wallpaper?url=%1").arg(uri);
        DDBusSender()
                .service("org.deepin.dde.ControlCenter1")
                .interface("org.deepin.dde.ControlCenter1")
                .path("/org/deepin/dde/ControlCenter1")
                .method("ShowPage")
                .arg(arg)
                .call();
    }
    doUpdateWallpaperURIs();
}

QString AppearanceManager::doGetWorkspaceBackgroundForMonitor(const int &index, const QString &strMonitorName)
{
    return getWallpaperUri(QString::number(index), strMonitorName);
}

void AppearanceManager::doSetDTKSizeMode(int value) {
    if (value != m_property->dtkSizeMode) {
        setDTKSizeMode(value);
        m_dbusProxy->SetInteger("DTK/SizeMode",value);
    }
}

void AppearanceManager::doSetQtScrollBarPolicy(int value)
{
    if (value != m_property->qtScrollBarPolicy) {
        setQtScrollBarPolicy(value);
        m_dbusProxy->SetInteger("Qt/ScrollBarPolicy",value);
    }
}

QString AppearanceManager::getActiveColors()
{
    return m_settingDconfig.value(DACTIVECOLORS).toString();
}

bool AppearanceManager::setDQtTheme(QStringList key, QStringList value)
{
    if (key.length() != value.length()) {
        return false;
    }

    QString filePath = utils::GetUserConfigDir() + "/deepin" + "/qt-theme.ini";

    QSettings settings(filePath, QSettings::IniFormat);
    settings.beginGroup("Theme");
    for (int i = 0; i < key.length(); i++) {
        QString temp = settings.value(key[i]).value<QString>();
        if (temp == value[i]) {
            continue;
        }
        settings.setValue(key[i], value[i]);
    }

    return true;
}

QString AppearanceManager::marshal(const QVector<QSharedPointer<Theme>> &themes)
{
    QJsonDocument doc;
    QJsonArray array;
    for (auto iter : themes) {
        QJsonObject obj;
        obj.insert("Id", iter->getId());
        obj.insert("Path", iter->getPath());
        obj.insert("Deletable", iter->getDeleteable());
        obj.insert("Name", iter->name());
        obj.insert("Comment", iter->comment());
        obj.insert("hasDark", iter->hasDark());
        obj.insert("Example", iter->example());
        array.append(obj);
    }

    doc.setArray(array);

    return doc.toJson(QJsonDocument::Compact);
}

QString AppearanceManager::marshal(const QVector<Background> &backgrounds)
{
    QJsonDocument doc;
    QJsonArray array;
    for (auto iter : backgrounds) {
        QJsonObject obj;
        obj.insert("Id", iter.getId());
        obj.insert("Deletable", iter.getDeleteable());
        array.append(obj);
    }

    doc.setArray(array);

    return doc.toJson(QJsonDocument::Compact);
}

QString AppearanceManager::marshal(const QStringList &strs)
{
    QJsonDocument doc;
    QJsonArray array;
    for (auto iter : strs) {
        array.append(iter);
    }

    doc.setArray(array);

    return doc.toJson(QJsonDocument::Compact);
}

QString AppearanceManager::marshal(const QVector<QSharedPointer<FontsManager::Family>> &strs)
{
    QJsonDocument doc;

    QJsonArray arr;
    for (auto iter : strs) {
        QJsonObject obj;
        obj["Id"] = iter->id;
        obj["Name"] = iter->name.isEmpty() ? iter->id : iter->name;
        obj["Styles"] = QJsonArray::fromStringList(iter->styles);
        obj["Show"] = iter->show;
        arr.push_back(obj);
    }

    doc.setArray(arr);
    return doc.toJson(QJsonDocument::Compact);
}

void AppearanceManager::initGlobalOverrideConfig()
{
    m_globalThemeOverrideMap.clear();
    const auto &overrideList = m_settingDconfig.value("globalThemeOverride").value<QVariantList>();
    for (const auto &overrideItem : overrideList) {
        const auto &overrideMap = overrideItem.toMap();
        auto id = overrideMap.value("id").toString();
        auto overrideVecMap = overrideMap.value("override").value<QVariantList>();
        for (const auto &overrideValue : overrideVecMap) {
            const auto &overrideMap = overrideValue.toMap();
            GlobalThemeOverride itemOverride;
            itemOverride.section = overrideMap.value("section").toString();
            itemOverride.key = overrideMap.value("key").toString();
            itemOverride.value = overrideMap.value("value").toString();
            m_globalThemeOverrideMap[id].append(itemOverride);

            qDebug() << "add global theme override:" << id << itemOverride.section << itemOverride.key << itemOverride.value;
        }
    }
}
