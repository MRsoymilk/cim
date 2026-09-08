#include "admin/AdminCLI.hpp"
#include "config/ServerOptions.hpp"
#include "core/CimServer.hpp"

#include <ixwebsocket/IXNetSystem.h>
#include <sodium.h>

#include <csignal>
#include <iostream>
#include <utility>

namespace {

volatile std::sig_atomic_t shutdown_requested = 0;

void HandleShutdownSignal(int) {
    shutdown_requested = 1;
}

} // namespace

int main(int argc, char** argv) {
    if (cim::IsAdminCommand(argc, argv)) {
        return cim::RunAdminCommand(argc, argv);
    }

    cim::ServerOptions options;
    if (!cim::ParseServerOptions(argc, argv, options) ||
        !cim::ValidateServerCredentials(options)) {
        return 2;
    }
    if (sodium_init() < 0) {
        std::cerr << "[Server] Failed to initialize libsodium" << std::endl;
        return 1;
    }
    if (!ix::initNetSystem()) {
        std::cerr << "[Server] Failed to initialize network system" << std::endl;
        return 1;
    }

    int result = 1;
    {
        cim::CimServer server(std::move(options));
        if (server.Initialize()) {
            if (std::signal(SIGINT, HandleShutdownSignal) == SIG_ERR ||
                std::signal(SIGTERM, HandleShutdownSignal) == SIG_ERR) {
                std::cerr << "[Server] Failed to install shutdown signal handlers" << std::endl;
            } else {
                result = server.Run([] { return shutdown_requested != 0; });
            }
        }
    }
    ix::uninitNetSystem();
    return result;
}
