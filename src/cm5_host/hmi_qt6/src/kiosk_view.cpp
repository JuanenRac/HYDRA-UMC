/*
 * =============================================================================
 * kiosk_view.cpp - real kiosk lockdown for the QWebEngineView shell
 * PROJECT: HYDRA-UMC
 * AUTHOR: JuanenRac (Electro Hobby 3D) - electrohobby3d@gmail.com
 * LICENSE: GPL-3.0 - see repo root LICENSE
 * See kiosk_view.h for the real Qt6 API/design reasoning.
 * =============================================================================
 */
#include "kiosk_view.h"

#include <QCloseEvent>
#include <QCursor>
#include <QKeyEvent>
#include <QMouseEvent>

KioskView::KioskView(QWidget *parent) : QWebEngineView(parent) {
  /* Real, documented way to disable the right-click context menu -
   * QWebEngineView's own default menu (Back/Forward/Reload/Inspect)
   * has no place on an operator-facing kiosk panel. */
  setContextMenuPolicy(Qt::NoContextMenu);

  m_cursorIdleTimer.setSingleShot(true);
  connect(&m_cursorIdleTimer, &QTimer::timeout, this, &KioskView::onCursorIdleTimeout);
  resetCursorIdleTimer();

  connect(this, &QWebEngineView::loadFinished, this, &KioskView::onLoadFinished);
}

void KioskView::loadWithRetry(const QUrl &url) {
  m_targetUrl = url;
  load(m_targetUrl);
}

void KioskView::onLoadFinished(bool ok) {
  if (ok) {
    return;
  }
  /* The real gap this closes: a cold-booting CM5 whose own Node
   * dashboard server (HYDRA-UMC-STUDIO, `npm start`) hasn't finished
   * starting yet used to show QWebEngineView's own built-in connection-
   * error page and never try again. Retrying on a fixed interval until
   * it succeeds means this kiosk shell just keeps waiting - real splash-
   * screen behavior - instead of stranding an operator on an error page
   * they have no keyboard/mouse to dismiss or refresh from. */
  QTimer::singleShot(kLoadRetryMs, this, [this]() { load(m_targetUrl); });
}

void KioskView::closeEvent(QCloseEvent *event) {
  if (m_unlocked) {
    event->accept();
    return;
  }
  /* Real Qt idiom: the window manager's own close button / Alt+F4
   * handling delivers a QCloseEvent the same way a programmatic close()
   * call would - ignoring it here blocks both without needing to trap
   * raw Alt+F4 keystrokes, which are window-manager-dependent and may
   * never reach this widget at all. */
  event->ignore();
}

void KioskView::keyPressEvent(QKeyEvent *event) {
  if (event->key() == kKioskUnlockKey && (event->modifiers() & kKioskUnlockModifiers) == kKioskUnlockModifiers) {
    m_unlocked = true;
    close();
    return;
  }
  /* Defense-in-depth against Escape/F11 - some window managers or the
   * page's own HTML5 Fullscreen API path bind these to "leave
   * fullscreen"; this app already owns fullscreen via showFullScreen()
   * on the top-level window (main.cpp), not the page's own JS API, so
   * swallowing them here costs a real operator nothing they'd actually
   * use, while closing a real (if WM-dependent) escape path. */
  if (event->key() == Qt::Key_Escape || event->key() == Qt::Key_F11) {
    event->accept();
    return;
  }
  QWebEngineView::keyPressEvent(event);
}

void KioskView::resetCursorIdleTimer() {
  unsetCursor();
  m_cursorIdleTimer.start(kCursorIdleMs);
}

void KioskView::onCursorIdleTimeout() { setCursor(Qt::BlankCursor); }

void KioskView::mouseMoveEvent(QMouseEvent *event) {
  resetCursorIdleTimer();
  QWebEngineView::mouseMoveEvent(event);
}

void KioskView::mousePressEvent(QMouseEvent *event) {
  resetCursorIdleTimer();
  QWebEngineView::mousePressEvent(event);
}
