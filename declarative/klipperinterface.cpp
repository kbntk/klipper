/*
    SPDX-FileCopyrightText: 2024 Fushan Wen <qydwhotmail@gmail.com>

    SPDX-License-Identifier: GPL-2.0-or-later
*/

#include "klipperinterface.h"
#include "klipper.h"
#include "klipper_debug.h"
#include "pasteinjector.h"

// libklipper does not export KLIPPER_LOG(), so the category gets defined again
// here - it uses the same name, hence the very same filtering rules apply
Q_LOGGING_CATEGORY(KLIPPER_LOG, "org.kde.klipper", QtInfoMsg)

KlipperInterface::KlipperInterface(QObject *parent)
    : QObject(parent)
    , m_klipper(Klipper::self())
{
}

KlipperInterface::~KlipperInterface() = default;

void KlipperInterface::configure()
{
    m_klipper->slotConfigure();
}

void KlipperInterface::requestPaste()
{
    qCDebug(KLIPPER_LOG) << "QML requested a paste on selection";
    PasteInjector::self()->requestPaste();
}

void KlipperInterface::popupClosed()
{
    qCDebug(KLIPPER_LOG) << "QML reported that the clipboard menu closed";
    PasteInjector::self()->popupClosed();
}

#include "moc_klipperinterface.cpp"
