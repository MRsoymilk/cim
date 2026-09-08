#include "ftxui/component/screen_interactive.hpp"
#include "ftxui/component/event.hpp"
#include "ui/chat/ChatUI.hpp"
#include <sodium.h>

int main() {
  if (sodium_init() < 0) {
    return 1;
  }
  auto screen = ftxui::ScreenInteractive::Fullscreen();

  cim::ChatUI chat_ui([&screen] {
    screen.Post(ftxui::Event::Custom);
  });
  screen.Loop(chat_ui.GetComponent());

  return 0;
}
