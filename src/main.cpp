#include "ftxui/component/screen_interactive.hpp"
#include "ftxui/component/event.hpp"
#include "ui/ChatUI.hpp"

int main() {
  auto screen = ftxui::ScreenInteractive::Fullscreen();

  cim::ChatUI chat_ui([&screen] {
    screen.Post(ftxui::Event::Custom);
  });
  screen.Loop(chat_ui.GetComponent());

  return 0;
}
