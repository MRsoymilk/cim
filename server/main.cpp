#include "admin/AdminCLI.hpp"
#include "config/ServerOptions.hpp"
#include "core/CimServer.hpp"

#include <ixwebsocket/IXNetSystem.h>
#include <sodium.h>

#include <iostream>
#include <utility>

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
            result = server.Run();
        }
    }
    ix::uninitNetSystem();
    return result;
}
