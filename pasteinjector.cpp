/*
    SPDX-FileCopyrightText: 2026 KDE Contributors

    SPDX-License-Identifier: GPL-2.0-or-later
*/

#include "pasteinjector.h"

#include "klipper_debug.h"
#include "klippersettings.h"

#include <KLocalizedString>
#include <KWindowSystem>

#include <KWayland/Client/connection_thread.h>
#include <KWayland/Client/fakeinput.h>
#include <KWayland/Client/plasmawindowmanagement.h>
#include <KWayland/Client/registry.h>

#include <QClipboard>
#include <QCoreApplication>
#include <QGuiApplication>
#include <QRegularExpression>
#include <QTimer>
#include <QWindow>

#include <linux/input-event-codes.h>

#include <algorithm>

using namespace Qt::StringLiterals;

namespace
{
// How long the clipboard has to be quiet before it is considered settled
constexpr int ClipboardSettleDelay = 100;
// How far back in time a clipboard change may lie to still count as settled
// when a paste is requested - the selection handler of the clipboard menu runs
// before the menu is told to hide itself, so the notification of a synchronous
// clipboard update will have arrived already
constexpr int ClipboardFreshness = 100;
// Interval and maximum total duration of waiting for this application to lose
// the keyboard focus, which happens once the clipboard menu has been hidden
constexpr int FocusPollDelay = 30;
constexpr int MaxFocusPolls = 50; // 50 * 30 ms = 1.5 s
// Extra grace period for the target window to fully regain the keyboard focus
constexpr int FocusGraceDelay = 100;
// Don't wait for the clipboard menu forever if it never reports being closed
constexpr int SafetyTimeout = 3000;
// Time between the individual key events of the paste shortcut
constexpr int KeyStepDelay = 15;

bool isShellWindow(const QString &appId)
{
    return appId == QLatin1String("org.kde.plasmashell") || appId == QLatin1String("org.kde.plasmawindowmenu") || appId == QLatin1String("org.kde.klipper");
}

// Parts of application identifiers and resource names that identify a terminal,
// long enough to be safe to match as substrings
const QStringList s_terminalSubstrings = {
    QStringLiteral("alacritty"),
    QStringLiteral("blackbox"),
    QStringLiteral("console"), // GNOME Console
    QStringLiteral("contour"),
    QStringLiteral("ghostty"),
    QStringLiteral("guake"),
    QStringLiteral("hyper"),
    QStringLiteral("kitty"),
    QStringLiteral("konsole"),
    QStringLiteral("lxterminal"),
    QStringLiteral("mlterm"),
    QStringLiteral("ptyxis"),
    QStringLiteral("qterminal"),
    QStringLiteral("rxvt"),
    QStringLiteral("sakura"),
    QStringLiteral("tabby"),
    QStringLiteral("terminal"),
    QStringLiteral("terminator"),
    QStringLiteral("terminology"),
    QStringLiteral("tilix"),
    QStringLiteral("wezterm"),
    QStringLiteral("xterm"),
    QStringLiteral("yakuake"),
};

// Too generic to be matched as substrings, they have to equal a complete
// component of the application identifier or resource name
const QStringList s_terminalWords = {
    QStringLiteral("foot"),
    QStringLiteral("kgx"),
    QStringLiteral("rio"),
    QStringLiteral("st"),
    QStringLiteral("tilda"),
    QStringLiteral("warp"),
};

bool looksLikeTerminal(const QString &appId, const QString &resourceName)
{
    const QString haystack = (appId + QLatin1Char(' ') + resourceName).toLower();
    if (std::any_of(s_terminalSubstrings.cbegin(), s_terminalSubstrings.cend(), [&haystack](const QString &token) {
            return haystack.contains(token);
        })) {
        return true;
    }
    const QStringList words = haystack.split(QRegularExpression(QStringLiteral("[^a-z0-9]")), Qt::SkipEmptyParts);
    return std::any_of(s_terminalWords.cbegin(), s_terminalWords.cend(), [&words](const QString &token) {
        return words.contains(token);
    });
}

QString describeWindow(QWindow *window)
{
    if (!window) {
        return QStringLiteral("<none>");
    }
    return u"%1 (%2)"_s.arg(window->objectName(), window->title());
}
}

PasteInjector::PasteInjector()
{
    m_clipboardSettleTimer.setSingleShot(true);
    m_clipboardSettleTimer.setInterval(ClipboardSettleDelay);
    connect(&m_clipboardSettleTimer, &QTimer::timeout, this, [this]() {
        m_clipboardReady = true;
        qCDebug(KLIPPER_LOG) << "clipboard settled";
        maybeStartPaste();
    });

    m_focusTimer.setSingleShot(true);
    m_focusTimer.setInterval(FocusPollDelay);
    connect(&m_focusTimer, &QTimer::timeout, this, &PasteInjector::checkFocusLoss);

    m_safetyTimer.setSingleShot(true);
    m_safetyTimer.setInterval(SafetyTimeout);
    connect(&m_safetyTimer, &QTimer::timeout, this, [this]() {
        // The clipboard menu never reported being closed. If we do not hold the
        // keyboard focus anymore, it was closed nevertheless, otherwise cancel
        // the paste instead of pasting into the menu itself
        if (!m_pendingPaste) {
            return;
        }
        if (QGuiApplication::focusWindow()) {
            qCWarning(KLIPPER_LOG) << "Giving up on pasting, the clipboard menu still has the keyboard focus" << describeWindow(QGuiApplication::focusWindow());
            m_pendingPaste = false;
            return;
        }
        qCDebug(KLIPPER_LOG) << "Safety timeout, but focus is elsewhere - pasting anyway";
        m_popupClosed = true;
        m_clipboardReady = true;
        maybeStartPaste();
    });

    connect(QGuiApplication::clipboard(), &QClipboard::changed, this, &PasteInjector::onClipboardChanged);

    initWayland();
}

/* static */ PasteInjector *PasteInjector::self()
{
    // Deliberately leaked: the Wayland objects owned by the injector must not
    // outlive the gui connection, which happens when it is destroyed after the
    // application has shut down
    static PasteInjector *s_instance = new PasteInjector;
    return s_instance;
}

void PasteInjector::requestPaste()
{
    if (!KlipperSettings::self()->pasteOnSelect()) {
        qCDebug(KLIPPER_LOG) << "requestPaste ignored: disabled in the settings";
        return;
    }
    if (!m_fakeInput) {
        qCWarning(KLIPPER_LOG) << "requestPaste ignored: no org_kde_kwin_fake_input interface";
        return;
    }

    m_pendingPaste = true;
    m_popupClosed = false;
    m_focusPolls = 0;
    m_focusTimer.stop();
    // The clipboard may have been updated before this method was called, in
    // which case there will be no notification anymore
    m_clipboardReady = m_clipboardChangeTime.isValid() && m_clipboardChangeTime.elapsed() <= ClipboardFreshness;
    if (!m_clipboardReady) {
        m_clipboardSettleTimer.start();
    }
    m_safetyTimer.start();

    qCDebug(KLIPPER_LOG) << "requestPaste: clipboard settled:" << m_clipboardReady << "focus window:" << describeWindow(QGuiApplication::focusWindow());

    maybeStartPaste();
}

void PasteInjector::popupClosed()
{
    if (!m_pendingPaste) {
        qCDebug(KLIPPER_LOG) << "popupClosed without a pending paste";
        return;
    }
    qCDebug(KLIPPER_LOG) << "popupClosed, focus window:" << describeWindow(QGuiApplication::focusWindow());
    m_popupClosed = true;
    maybeStartPaste();
}

void PasteInjector::onClipboardChanged(QClipboard::Mode mode)
{
    if (mode != QClipboard::Clipboard) {
        return;
    }
    m_clipboardChangeTime.start();
    if (m_pendingPaste && !m_clipboardReady) {
        // Debounce, the clipboard may get updated in multiple steps
        m_clipboardSettleTimer.start();
    }
}

void PasteInjector::maybeStartPaste()
{
    if (!m_pendingPaste || !m_popupClosed || !m_clipboardReady || m_focusPolls > 0) {
        return;
    }
    // From now on we are waiting for this application to lose the keyboard
    // focus, which means that the clipboard menu has released it and that the
    // previously active window has a chance to regain it
    m_focusWindow = QGuiApplication::focusWindow();
    m_focusPolls = 1;
    m_focusTimer.start();
}

void PasteInjector::checkFocusLoss()
{
    if (!m_pendingPaste) {
        m_focusTimer.stop();
        return;
    }

    QWindow *focusWindow = QGuiApplication::focusWindow();
    if (focusWindow && focusWindow == m_focusWindow) {
        if (m_focusPolls < MaxFocusPolls) {
            ++m_focusPolls;
            m_focusTimer.start();
            return;
        }
        qCWarning(KLIPPER_LOG) << "Giving up on pasting, focus was never released from" << describeWindow(focusWindow);
        m_pendingPaste = false;
        m_safetyTimer.stop();
        return;
    }

    // The clipboard menu released the keyboard focus
    m_focusTimer.stop();
    m_pendingPaste = false;
    m_safetyTimer.stop();
    qCDebug(KLIPPER_LOG) << "focus released after" << (m_focusPolls * FocusPollDelay) << "ms, focus window now:" << describeWindow(focusWindow);

    QTimer::singleShot(FocusGraceDelay, this, [this]() {
        sendPasteKeys(isTerminalTarget());
    });
}

bool PasteInjector::isTerminalTarget() const
{
    const KWayland::Client::PlasmaWindow *window = m_windowManagement ? m_windowManagement->activeWindow() : nullptr;
    const QString appId = window ? window->appId() : QString();
    const QString resourceName = window ? window->resourceName() : QString();
    if (window && isShellWindow(appId)) {
        qCWarning(KLIPPER_LOG) << "The active window is a shell window:" << appId;
    }
    const bool terminal = looksLikeTerminal(appId, resourceName);
    qCDebug(KLIPPER_LOG) << "Target window:" << appId << resourceName << (terminal ? "using Shift+Insert" : "using Ctrl+V");
    return terminal;
}

void PasteInjector::sendPasteKeys(bool terminal)
{
    if (!m_fakeInput) {
        return;
    }

    // Terminals usually paste with Shift+Insert, everything else with Ctrl+V
    const quint32 modifier = terminal ? KEY_LEFTSHIFT : KEY_LEFTCTRL;
    const quint32 key = terminal ? KEY_INSERT : KEY_V;

    qCDebug(KLIPPER_LOG) << "sending" << (terminal ? "Shift+Insert" : "Ctrl+V");
    m_fakeInput->requestKeyboardKeyPress(modifier);
    QTimer::singleShot(KeyStepDelay, this, [this, key]() {
        m_fakeInput->requestKeyboardKeyPress(key);
    });
    QTimer::singleShot(2 * KeyStepDelay, this, [this, key]() {
        m_fakeInput->requestKeyboardKeyRelease(key);
    });
    QTimer::singleShot(3 * KeyStepDelay, this, [this, modifier]() {
        m_fakeInput->requestKeyboardKeyRelease(modifier);
    });
}

void PasteInjector::initWayland()
{
    if (!KWindowSystem::isPlatformWayland()) {
        qCWarning(KLIPPER_LOG) << "Automatic pasting of clipboard history items is only supported on Wayland";
        return;
    }

    auto connection = KWayland::Client::ConnectionThread::fromApplication(qGuiApp);
    if (!connection) {
        qCWarning(KLIPPER_LOG) << "No Wayland connection available, clipboard history items cannot be pasted automatically";
        return;
    }

    auto registry = new KWayland::Client::Registry(this);
    connect(registry, &KWayland::Client::Registry::fakeInputAnnounced, this, [this, registry](quint32 name, quint32 version) {
        m_fakeInput = registry->createFakeInput(name, version, this);
        // KWin does not show a prompt for this, the interface is granted based
        // on the desktop file of the client
        m_fakeInput->authenticate(i18n("Klipper"), i18n("Pasting clipboard history items into other applications"));
        qCDebug(KLIPPER_LOG) << "org_kde_kwin_fake_input available, version" << version;
    });
    connect(registry, &KWayland::Client::Registry::plasmaWindowManagementAnnounced, this, [this, registry](quint32 name, quint32 version) {
        m_windowManagement = registry->createPlasmaWindowManagement(name, version, this);
    });
    connect(QCoreApplication::instance(), &QCoreApplication::aboutToQuit, registry, [registry]() {
        delete registry; // Avoid freeing resources when the gui is deleted
    });

    registry->create(connection);
    registry->setup();
    connection->roundtrip();

    if (!m_fakeInput) {
        qCWarning(KLIPPER_LOG) << "org_kde_kwin_fake_input is not available, choosing a clipboard item will not paste it automatically."
                               << "Make sure the interface is listed in X-KDE-Wayland-Interfaces of the plasmashell desktop file and restart plasmashell.";
    }
    if (!m_windowManagement) {
        qCWarning(KLIPPER_LOG) << "org_kde_plasma_window_management is not available, terminals will not be detected";
    }
}

#include "moc_pasteinjector.cpp"
