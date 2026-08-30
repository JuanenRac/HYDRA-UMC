/*
 * =============================================================================
 * kiosk_view.h - real kiosk lockdown for the QWebEngineView shell
 * PROJECT: HYDRA-UMC
 * AUTHOR: JuanenRac (Electro Hobby 3D) - electrohobby3d@gmail.com
 * LICENSE: GPL-3.0 - see repo root LICENSE
 *
 * The real hardening README.md's own "What's still needed" listed as
 * TODO: no right-click context menu, no accidental close/escape out of
 * the kiosk, cursor hidden on an idle touch panel, and a retry loop so a
 * cold-booting CM5 (this view's own Node dashboard server not up yet)
 * shows a real splash instead of a browser connection-error page.
 *
 * Real Qt6 API used here (QWebEngineView is a QWidget subclass, so every
 * QWidget/QObject mechanism below applies unchanged):
 *   - QWidget::setContextMenuPolicy(Qt::NoContextMenu) - real, documented
 *     way to disable the right-click menu, rather than trying to filter
 *     individual mouse events.
 *   - QWidget::closeEvent(QCloseEvent*) override + event->ignore() - the
 *     real Qt idiom for "this window cannot be closed" - the window
 *     manager's own Alt+F4/close-button handling delivers a QCloseEvent
 *     the same way either source would, so blocking it here covers both
 *     without needing to intercept raw Alt+F4 keystrokes (which are
 *     window-manager-dependent and may never reach this widget at all).
 *   - QWidget::keyPressEvent(QKeyEvent*) override - swallows Escape/F11
 *     as defense-in-depth (some window managers/HTML5 Fullscreen API
 *     paths do bind these) unless the technician unlock combo below is
 *     pressed.
 *   - A QTimer restarted from mouseMoveEvent()/mousePressEvent() -
 *     standard, real Qt idle-cursor-hide pattern (QCursor(Qt::BlankCursor)
 *     while idle, unsetCursor() on the next motion).
 *
 * NOT YET COMPILED against a real Qt6 install (see ../CMakeLists.txt's
 * own header) - written against Qt6's real, documented public API, not
 * guessed at, but honestly unverified by this session's own toolchain
 * (no Qt6 on this development machine). Verify on target or a real Qt6
 * dev environment before trusting this builds clean.
 * =============================================================================
 */
#pragma once

#include <QWebEngineView>
#include <QTimer>
#include <QUrl>

/* The one keyboard combo that IS allowed to break out of the kiosk - a
 * technician debugging a stuck device, not a normal operator. Chosen to
 * be unreachable by a stray touch/accidental keypress: three modifiers
 * plus a letter no other shortcut here uses. */
constexpr int kKioskUnlockKey = Qt::Key_Q;
constexpr Qt::KeyboardModifiers kKioskUnlockModifiers =
    Qt::ControlModifier | Qt::AltModifier | Qt::ShiftModifier;

/* How long the cursor stays visible after the last touch/mouse motion
 * before this view hides it - a touch panel has no persistent mouse
 * cursor need, but SOME motion (a connected USB mouse for setup/debug)
 * should still show it again immediately, not require a restart. */
constexpr int kCursorIdleMs = 5000;

/* How long to wait before retrying a failed load - the real gap this
 * closes: main.cpp used to call view.load(url) exactly once, showing
 * whatever QWebEngineView's own built-in connection-error page looks
 * like for however long the dashboard's Node server (HYDRA-UMC-STUDIO,
 * `npm start`) takes to come up after this app's own process starts. */
constexpr int kLoadRetryMs = 1000;

class KioskView : public QWebEngineView {
  Q_OBJECT

public:
  explicit KioskView(QWidget *parent = nullptr);

  /* Starts (or restarts, if already loading) the retry-until-it-loads
   * flow against `url` - the real replacement for a single view.load()
   * call in main.cpp. */
  void loadWithRetry(const QUrl &url);

protected:
  void closeEvent(QCloseEvent *event) override;
  void keyPressEvent(QKeyEvent *event) override;
  void mouseMoveEvent(QMouseEvent *event) override;
  void mousePressEvent(QMouseEvent *event) override;

private slots:
  void onLoadFinished(bool ok);
  void onCursorIdleTimeout();

private:
  void resetCursorIdleTimer();

  QUrl m_targetUrl;
  QTimer m_cursorIdleTimer;
  bool m_unlocked = false;
};
