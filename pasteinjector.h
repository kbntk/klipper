/*
    SPDX-FileCopyrightText: 2026 KDE Contributors

    SPDX-License-Identifier: GPL-2.0-or-later
*/

#pragma once

#include "klipper_export.h"

#include <QClipboard>
#include <QElapsedTimer>
#include <QObject>
#include <QPointer>
#include <QTimer>

namespace KWayland
{
namespace Client
{
class FakeInput;
class PlasmaWindowManagement;
}
}

class QWindow;

/**
 * @short Pastes a clipboard history item into the previously active window.
 *
 * When an item is chosen from the clipboard history, it is copied to the
 * clipboard as before, and on top of that a paste shortcut (Ctrl+V, or
 * Shift+Insert when the target is a terminal) is injected into the window
 * that was active before the clipboard menu was opened.
 *
 * Injection is done through the privileged org_kde_kwin_fake_input Wayland
 * protocol, so this only works on Wayland and only if KWin granted the
 * interface to this client (it must be listed in X-KDE-Wayland-Interfaces
 * of the plasmashell desktop file).
 *
 * The shortcut is not sent right away, but once
 * - the clipboard menu has been closed, so that the previously active window
 *   has regained the keyboard focus, and
 * - the clipboard content has settled, as setting the clipboard may be
 *   asynchronous.
 */
class KLIPPER_EXPORT PasteInjector : public QObject
{
    Q_OBJECT

public:
    /**
     * The injector is created as soon as the clipboard menu infrastructure is
     * set up, so that it already observes clipboard changes when the first
     * item gets selected.
     * @return the global PasteInjector instance
     */
    static PasteInjector *self();

    /**
     * An item from the clipboard history was chosen and the clipboard menu is
     * about to be hidden. The paste shortcut is sent once the menu has been
     * closed and the clipboard content has settled.
     */
    void requestPaste();
    /**
     * The clipboard menu has been closed. Sends the shortcut requested before,
     * if there is any.
     */
    void popupClosed();

private:
    PasteInjector();

    void initWayland();
    void onClipboardChanged(QClipboard::Mode mode);
    void maybeStartPaste();
    void checkFocusLoss();
    bool isTerminalTarget() const;
    void sendPasteKeys(bool terminal);

    KWayland::Client::FakeInput *m_fakeInput = nullptr;
    KWayland::Client::PlasmaWindowManagement *m_windowManagement = nullptr;

    QTimer m_clipboardSettleTimer;
    QTimer m_focusTimer;
    QTimer m_safetyTimer;

    QElapsedTimer m_clipboardChangeTime;
    QPointer<QWindow> m_focusWindow;
    bool m_pendingPaste = false;
    bool m_popupClosed = false;
    bool m_clipboardReady = false;
    int m_focusPolls = 0;
};
