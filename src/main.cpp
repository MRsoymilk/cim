#include "ftxui/component/screen_interactive.hpp"
#include "ui/ChatUI.hpp"

int main() {
  auto screen = ftxui::ScreenInteractive::Fullscreen();

  cim::ChatUI chat_ui;
  screen.Loop(chat_ui.GetComponent());

  return 0;
}
