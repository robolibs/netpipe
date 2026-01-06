#include <atomic>
#include <chrono>
#include <netpipe/netpipe.hpp>
#include <sys/mman.h>
#include <thread>
#include <wirebit/wirebit.hpp>

// Example: Tunnel wirebit Ethernet frames over netpipe TCP
//
// Architecture:
//   [Server ShmLink "eth_server"] <---> [Tunnel Server]
//                                             |
//                                        TCP tunnel
//                                             |
//   [Client ShmLink "eth_client"] <---> [Tunnel Client]
//
// Each side has its OWN ShmLink. The TCP tunnel bridges them.
// This simulates two separate machines connected via a network tunnel.

constexpr int TUNNEL_PORT = 9001;
constexpr int FRAME_COUNT = 5;

std::atomic<bool> g_running{true};

void tunnel_server() {
    echo::info("=== Tunnel Server Starting ===");

    // Create server-side ShmLink
    auto shm_res = wirebit::ShmLink::create(wirebit::String("eth_server"), 1024 * 1024);
    if (shm_res.is_err()) {
        echo::error("Failed to create ShmLink: ", shm_res.error().message.c_str());
        return;
    }
    auto shm_link = std::make_shared<wirebit::ShmLink>(std::move(shm_res.value()));

    // Create Ethernet endpoint for the tunnel (promiscuous mode to forward all frames)
    wirebit::MacAddr tunnel_mac = {0x02, 0x00, 0x00, 0x00, 0x00, 0x01};
    wirebit::EthConfig eth_cfg{.bandwidth_bps = 1000000000, .promiscuous = true};
    wirebit::EthEndpoint eth(shm_link, eth_cfg, 1, tunnel_mac);

    echo::info("Server tunnel endpoint: MAC=02:00:00:00:00:01 (promiscuous)");

    // Create TCP server
    netpipe::TcpStream tcp_server;
    netpipe::TcpEndpoint tcp_ep{"0.0.0.0", TUNNEL_PORT};

    if (auto res = tcp_server.listen(tcp_ep); res.is_err()) {
        echo::error("TCP listen failed: ", res.error().message.c_str());
        return;
    }

    echo::info("Waiting for tunnel client on port ", TUNNEL_PORT, "...");

    auto client_res = tcp_server.accept();
    if (client_res.is_err()) {
        echo::error("TCP accept failed: ", client_res.error().message.c_str());
        return;
    }
    auto tcp_client = std::move(client_res.value());
    echo::info("Tunnel established!");

    // Thread to receive from TCP and inject into local Ethernet
    std::thread tcp_to_eth([&]() {
        while (g_running) {
            auto recv_res = tcp_client->recv();
            if (recv_res.is_err()) {
                if (g_running) {
                    echo::error("TCP recv failed: ", recv_res.error().message.c_str());
                }
                break;
            }

            auto msg = recv_res.value();
            wirebit::Bytes frame(msg.begin(), msg.end());

            // Parse and log
            wirebit::MacAddr dst, src;
            uint16_t ethertype;
            wirebit::Bytes payload;
            wirebit::parse_eth_frame(frame, dst, src, ethertype, payload);

            echo::info("[Server] TCP->Eth: ", frame.size(), " bytes from ", wirebit::mac_to_string(src).c_str(),
                       " ethertype=0x", std::hex, ethertype, std::dec);

            // Inject into local Ethernet
            eth.send_eth(frame);
        }
    });

    // Thread to receive from local Ethernet and send over TCP
    std::thread eth_to_tcp([&]() {
        while (g_running) {
            eth.process();
            auto recv_res = eth.recv_eth();
            if (recv_res.is_ok()) {
                auto frame = recv_res.value();

                // Parse and log
                wirebit::MacAddr dst, src;
                uint16_t ethertype;
                wirebit::Bytes payload;
                wirebit::parse_eth_frame(frame, dst, src, ethertype, payload);

                echo::info("[Server] Eth->TCP: ", frame.size(), " bytes to ", wirebit::mac_to_string(dst).c_str(),
                           " ethertype=0x", std::hex, ethertype, std::dec);

                // Send over TCP tunnel
                netpipe::Message msg(frame.begin(), frame.end());
                if (auto res = tcp_client->send(msg); res.is_err()) {
                    echo::error("TCP send failed: ", res.error().message.c_str());
                    break;
                }
            }
            std::this_thread::sleep_for(std::chrono::milliseconds(10));
        }
    });

    // Wait for threads
    tcp_to_eth.join();
    eth_to_tcp.join();

    tcp_client->close();
    tcp_server.close();
    echo::info("Server done");
}

void tunnel_client() {
    echo::info("=== Tunnel Client Starting ===");
    std::this_thread::sleep_for(std::chrono::milliseconds(500));

    // Create client-side ShmLink
    auto shm_res = wirebit::ShmLink::create(wirebit::String("eth_client"), 1024 * 1024);
    if (shm_res.is_err()) {
        echo::error("Failed to create ShmLink: ", shm_res.error().message.c_str());
        return;
    }
    auto shm_link = std::make_shared<wirebit::ShmLink>(std::move(shm_res.value()));

    // Create Ethernet endpoint for the tunnel (promiscuous mode to forward all frames)
    wirebit::MacAddr tunnel_mac = {0x02, 0x00, 0x00, 0x00, 0x00, 0x02};
    wirebit::EthConfig eth_cfg{.bandwidth_bps = 1000000000, .promiscuous = true};
    wirebit::EthEndpoint eth(shm_link, eth_cfg, 2, tunnel_mac);

    echo::info("Client tunnel endpoint: MAC=02:00:00:00:00:02 (promiscuous)");

    // Connect to tunnel server
    netpipe::TcpStream tcp;
    netpipe::TcpEndpoint tcp_ep{"127.0.0.1", TUNNEL_PORT};

    if (auto res = tcp.connect(tcp_ep); res.is_err()) {
        echo::error("TCP connect failed: ", res.error().message.c_str());
        return;
    }
    echo::info("Connected to tunnel server!");

    // Thread to receive from TCP and inject into local Ethernet
    std::thread tcp_to_eth([&]() {
        while (g_running) {
            auto recv_res = tcp.recv();
            if (recv_res.is_err()) {
                if (g_running) {
                    echo::error("TCP recv failed: ", recv_res.error().message.c_str());
                }
                break;
            }

            auto msg = recv_res.value();
            wirebit::Bytes frame(msg.begin(), msg.end());

            // Parse and log
            wirebit::MacAddr dst, src;
            uint16_t ethertype;
            wirebit::Bytes payload;
            wirebit::parse_eth_frame(frame, dst, src, ethertype, payload);

            echo::info("[Client] TCP->Eth: ", frame.size(), " bytes from ", wirebit::mac_to_string(src).c_str(),
                       " ethertype=0x", std::hex, ethertype, std::dec);

            // Inject into local Ethernet
            eth.send_eth(frame);
        }
    });

    // Thread to receive from local Ethernet and send over TCP
    std::thread eth_to_tcp([&]() {
        while (g_running) {
            eth.process();
            auto recv_res = eth.recv_eth();
            if (recv_res.is_ok()) {
                auto frame = recv_res.value();

                // Parse and log
                wirebit::MacAddr dst, src;
                uint16_t ethertype;
                wirebit::Bytes payload;
                wirebit::parse_eth_frame(frame, dst, src, ethertype, payload);

                echo::info("[Client] Eth->TCP: ", frame.size(), " bytes to ", wirebit::mac_to_string(dst).c_str(),
                           " ethertype=0x", std::hex, ethertype, std::dec);

                // Send over TCP tunnel
                netpipe::Message msg(frame.begin(), frame.end());
                if (auto res = tcp.send(msg); res.is_err()) {
                    echo::error("TCP send failed: ", res.error().message.c_str());
                    break;
                }
            }
            std::this_thread::sleep_for(std::chrono::milliseconds(10));
        }
    });

    // Wait for threads
    tcp_to_eth.join();
    eth_to_tcp.join();

    tcp.close();
    echo::info("Client done");
}

void send_test() {
    echo::info("=== Test Sender Starting ===");
    std::this_thread::sleep_for(std::chrono::seconds(1));

    // Attach to client's ShmLink
    auto shm_res = wirebit::ShmLink::attach(wirebit::String("eth_client"));
    if (shm_res.is_err()) {
        echo::error("Failed to attach to eth_client: ", shm_res.error().message.c_str());
        return;
    }
    auto shm_link = std::make_shared<wirebit::ShmLink>(std::move(shm_res.value()));

    // Create app endpoint
    wirebit::MacAddr app_mac = {0x02, 0x00, 0x00, 0x00, 0x00, 0xAA};
    wirebit::EthConfig eth_cfg{.bandwidth_bps = 1000000000};
    wirebit::EthEndpoint eth(shm_link, eth_cfg, 0xAA, app_mac);

    echo::info("Test sender: MAC=02:00:00:00:00:AA");

    // Destination is on the server side
    wirebit::MacAddr dst_mac = {0x02, 0x00, 0x00, 0x00, 0x00, 0xBB};

    for (int i = 0; i < FRAME_COUNT; i++) {
        wirebit::String msg = wirebit::String("Hello #") + wirebit::String(std::to_string(i).c_str());
        wirebit::Bytes payload(msg.begin(), msg.end());

        auto frame = wirebit::make_eth_frame(dst_mac, app_mac, 0x0800, payload);
        echo::info("[Sender] Sending: \"", msg.c_str(), "\"");

        eth.send_eth(frame);
        std::this_thread::sleep_for(std::chrono::milliseconds(500));
    }

    g_running = false;
    echo::info("Test sender done");
}

void recv_test() {
    echo::info("=== Test Receiver Starting ===");
    std::this_thread::sleep_for(std::chrono::seconds(1));

    // Attach to server's ShmLink
    auto shm_res = wirebit::ShmLink::attach(wirebit::String("eth_server"));
    if (shm_res.is_err()) {
        echo::error("Failed to attach to eth_server: ", shm_res.error().message.c_str());
        return;
    }
    auto shm_link = std::make_shared<wirebit::ShmLink>(std::move(shm_res.value()));

    // Create app endpoint
    wirebit::MacAddr app_mac = {0x02, 0x00, 0x00, 0x00, 0x00, 0xBB};
    wirebit::EthConfig eth_cfg{.bandwidth_bps = 1000000000};
    wirebit::EthEndpoint eth(shm_link, eth_cfg, 0xBB, app_mac);

    echo::info("Test receiver: MAC=02:00:00:00:00:BB");

    int received = 0;
    while (g_running && received < FRAME_COUNT) {
        eth.process();
        auto recv_res = eth.recv_eth();
        if (recv_res.is_ok()) {
            auto frame = recv_res.value();

            wirebit::MacAddr dst, src;
            uint16_t ethertype;
            wirebit::Bytes payload;
            wirebit::parse_eth_frame(frame, dst, src, ethertype, payload);

            wirebit::String msg(reinterpret_cast<const char *>(payload.data()), payload.size());
            echo::info("[Receiver] Got: \"", msg.c_str(), "\" from ", wirebit::mac_to_string(src).c_str());
            received++;
        }
        std::this_thread::sleep_for(std::chrono::milliseconds(50));
    }

    echo::info("Test receiver done (", received, " frames)");
}

void cleanup() {
    echo::info("Cleaning up shared memory...");
    shm_unlink("/eth_server_tx");
    shm_unlink("/eth_server_rx");
    shm_unlink("/eth_client_tx");
    shm_unlink("/eth_client_rx");
    echo::info("Cleanup complete");
}

int main(int argc, char **argv) {
    if (argc < 2) {
        echo::info("Usage: ", argv[0], " <command>");
        echo::info("");
        echo::info("Ethernet Tunnel - Bridge L2 Ethernet over TCP");
        echo::info("");
        echo::info("Architecture:");
        echo::info("  [Sender App] -> [eth_client] -> [Tunnel Client]");
        echo::info("                                        |");
        echo::info("                                   TCP tunnel");
        echo::info("                                        |");
        echo::info("  [Receiver App] <- [eth_server] <- [Tunnel Server]");
        echo::info("");
        echo::info("Commands (run in separate terminals):");
        echo::info("  1. server   - Start tunnel server");
        echo::info("  2. client   - Start tunnel client");
        echo::info("  3. recv     - Start test receiver (on server side)");
        echo::info("  4. send     - Start test sender (on client side)");
        echo::info("  cleanup     - Remove shared memory segments");
        return 1;
    }

    wirebit::String mode(argv[1]);
    if (mode == "server") {
        tunnel_server();
    } else if (mode == "client") {
        tunnel_client();
    } else if (mode == "send") {
        send_test();
    } else if (mode == "recv") {
        recv_test();
    } else if (mode == "cleanup") {
        cleanup();
    } else {
        echo::error("Unknown command: ", mode.c_str());
        return 1;
    }

    return 0;
}
