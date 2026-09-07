/*
 * =============================================================================
 * main.cpp - hmi_qt6 kiosk shell entry point
 * PROJECT: HYDRA-UMC
 * AUTHOR: JuanenRac (Electro Hobby 3D) - electrohobby3d@gmail.com
 * LICENSE: GPL-3.0 - see repo root LICENSE
 *
 * See ../README.md for the full status (a kiosk wrapper around
 * HYDRA-UMC-STUDIO's own web dashboard, not a reimplementation of it).
 * Real now: a boot splash screen shown while KioskView's own
 * retry-until-it-loads flow (kiosk_view.cpp) waits for the dashboard's
 * Node server to come up, plus the real lockdown that view provides
 * (no context menu, no accidental close, Escape/F11 swallowed, cursor
 * hidden on idle) - see kiosk_view.h's own header for the real Qt6 API
 * each of those is built on. Still not yet built here (no Qt6 install
 * on this development machine - see ../CMakeLists.txt's own header).
 * =============================================================================
 */

#include <QApplication>
#include <QPainter>
#include <QPixmap>
#include <QSplashScreen>
#include <QUrl>

#include "kiosk_view.h"

namespace {

/* A splash drawn in code rather than shipped as an image asset - this
 * kiosk shell has no branding image checked into this repo yet, and a
 * plain filled QPixmap + QPainter text is real, working Qt (no
 * placeholder/no-op), not a stand-in waiting on a real asset. Swap for
 * QPixmap(":/splash.png") once a real branded asset exists. */
QPixmap buildSplashPixmap() {
  QPixmap pixmap(480, 320);
  pixmap.fill(QColor("#0b0f14"));

  QPainter painter(&pixmap);
  painter.setPen(QColor("#e6edf3"));
  QFont titleFont = painter.font();
  titleFont.setPointSize(28);
  titleFont.setBold(true);
  painter.setFont(titleFont);
  painter.drawText(pixmap.rect(), Qt::AlignCenter, "HYDRA-UMC");

  QFont subtitleFont = painter.font();
  subtitleFont.setPointSize(11);
  subtitleFont.setBold(false);
  painter.setFont(subtitleFont);
  painter.setPen(QColor("#8b98a5"));
  QRect subtitleRect = pixmap.rect().adjusted(0, 60, 0, 0);
  painter.drawText(subtitleRect, Qt::AlignHCenter | Qt::AlignTop, "Starting dashboard...");
  painter.end();

  return pixmap;
}

} // namespace

int main(int argc, char *argv[]) {
  QApplication app(argc, argv);

  QSplashScreen splash(buildSplashPixmap());
  splash.show();
  app.processEvents();

  KioskView view;
  view.setWindowTitle("HYDRA-UMC");

  /* Closes the splash only once the dashboard has genuinely finished
   * loading (loadFinished(true)) - KioskView's own retry-until-it-loads
   * flow (kiosk_view.cpp) keeps this splash on screen through however
   * many failed attempts a cold-booting CM5's Node server needs, rather
   * than dropping to a browser connection-error page after one try. */
  QObject::connect(&view, &QWebEngineView::loadFinished, &splash, [&splash, &view](bool ok) {
    if (ok) {
      splash.finish(&view);
    }
  });

  view.showFullScreen();
  view.loadWithRetry(QUrl("http://localhost:3000"));

  return app.exec();
}
