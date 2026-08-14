/*
 * =============================================================================
 * main.cpp - hmi_qt6 kiosk shell entry point
 * PROJECT: HYDRA-UMC
 * AUTHOR: JuanenRac (Electro Hobby 3D) - electrohobby3d@gmail.com
 * LICENSE: GPL-3.0 - see repo root LICENSE
 *
 * STARTING POINT ONLY - see ../README.md for the full status and what this
 * is (a kiosk wrapper around HYDRA-UMC-STUDIO's own web dashboard, not a
 * reimplementation of it). No splash screen, no kiosk lockdown, no OS
 * integration yet - just proves a full-screen QWebEngineView pointed at
 * the dashboard's own local URL is the right shape.
 * =============================================================================
 */

#include <QApplication>
#include <QWebEngineView>

int main(int argc, char *argv[])
{
  QApplication app(argc, argv);

  QWebEngineView view;
  view.setWindowTitle("HYDRA-UMC");
  /* TODO: wait for the dashboard's own Node server (HYDRA-UMC-STUDIO,
   * `npm start`, default port 3000) to actually be up before loading -
   * this navigates immediately and will show a connection error if the
   * server isn't ready yet. */
  view.load(QUrl("http://localhost:3000"));
  view.showFullScreen();

  return app.exec();
}
