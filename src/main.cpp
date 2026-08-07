#include "oatpp/network/Server.hpp"
#include "oatpp/network/tcp/server/ConnectionProvider.hpp"
#include "oatpp/web/server/HttpConnectionHandler.hpp"
#include "oatpp/web/server/HttpRouter.hpp"
#include "oatpp/Environment.hpp"

#include "KlvPipeline.hpp"
#include "VmsController.hpp"

#include <filesystem>
#include <iostream>

int main(int argc, char *argv[]) {
    std::filesystem::remove_all("hls");
    std::filesystem::create_directories("hls");

    oatpp::Environment::init();

    auto vms_camera = std::make_shared<KlvPipeline>();
    std::cout << "[System] Starting GStreamer Engine...\n";
    if (vms_camera->start(argc, argv) != 0) {
        std::cerr << "[Error] Failed to start camera feed!\n";
        oatpp::Environment::destroy();
        return -1;
    }

    // We only serve HTML + raw strings, never JSON DTOs.
    // ApiController only dereferences the mapper on createDtoResponse(),
    // which we never call, so nullptr is safe on every oatpp version.
    std::shared_ptr<oatpp::data::mapping::ObjectMapper> objectMapper = nullptr;

    auto router = oatpp::web::server::HttpRouter::createShared();
    router->addController(std::make_shared<VmsController>(objectMapper, vms_camera));

    auto connectionHandler =
        oatpp::web::server::HttpConnectionHandler::createShared(router);
    auto connectionProvider =
        oatpp::network::tcp::server::ConnectionProvider::createShared(
            {"0.0.0.0", 8000, oatpp::network::Address::IP_4});

    oatpp::network::Server server(connectionProvider, connectionHandler);

    std::cout << "\n=========================================\n";
    std::cout << "   VMS Web Server Active\n";
    std::cout << "   Open http://localhost:8000/player\n";
    std::cout << "=========================================\n";

    server.run();
    oatpp::Environment::destroy();
    return 0;
}
