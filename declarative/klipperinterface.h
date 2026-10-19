/*
    SPDX-FileCopyrightText: 2024 Fushan Wen <qydwhotmail@gmail.com>

    SPDX-License-Identifier: GPL-2.0-or-later
*/

#pragma once

#include <QObject>
#include <qqmlregistration.h>

class Klipper;

/**
 * This class registers a client for Klipper
 **/
class KlipperInterface : public QObject
{
    Q_OBJECT
    QML_ELEMENT

public:
    explicit KlipperInterface(QObject *parent = nullptr);
    ~KlipperInterface() override;

    Q_INVOKABLE void configure();

    /**
     * An item from the clipboard history was chosen and the clipboard menu is
     * about to be hidden. Unless disabled in the settings, the item is pasted
     * into the window that was active before the menu was opened.
     **/
    Q_INVOKABLE void requestPaste();

    /**
     * The clipboard menu has been closed, which may trigger the paste
     * requested by a preceding call to requestPaste().
     **/
    Q_INVOKABLE void popupClosed();

private:
    std::shared_ptr<Klipper> m_klipper;
};
