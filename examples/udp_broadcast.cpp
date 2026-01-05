#include <chrono>
#include <netpipe/netpipe.hpp>
#include <thread>

void sender() {
    echo::info("UDP Broadcast Sender starting...");

    netpipe::UdpDatagram udp;

    for (int i = 0; i < 5; i++) {
        dp::String msg_str = dp::String("Broadcast message #") + dp::String(std::to_string(i).c_str());
        netpipe::Message msg(msg_str.begin(), msg_str.end());

        auto res = udp.broadcast(msg);
        if (res.is_err()) {
            echo::error("Broadcast failed: ", res.error().message.c_str());
        } else {
            echo::info("Broadcast sent: ", msg_str.c_str());
        }

        std::this_thread::sleep_for(std::chrono::seconds(1));
    }

    udp.close();
    echo::info("Sender done");
}

void receiver() {
    echo::info("UDP Broadcast Receiver starting...");

    netpipe::UdpDatagram udp;
    netpipe::UdpEndpoint endpoint{"0.0.0.0", 7447};

    auto res = udp.bind(endpoint);
    if (res.is_err()) {
        echo::error("Bind failed: ", res.error().message.c_str());
        return;
    }

    echo::info("Listening for broadcasts on port 7447");

    for (int i = 0; i < 5; i++) {
        auto recv_res = udp.recv_from();
        if (recv_res.is_err()) {
            echo::error("Recv failed: ", recv_res.error().message.c_str());
            break;
        }

        auto [msg, src] = recv_res.value();
        dp::String msg_str(reinterpret_cast<const char *>(msg.data()));
        echo::info("Received from ", src.to_string(), ": ", msg_str.c_str());
    }

    udp.close();
    echo::info("Receiver done");
}

int main(int argc, char **argv) {
    if (argc < 2) {
        echo::info("Usage: ", argv[0], " [sender|receiver]");
        return 1;
    }

    dp::String mode(argv[1]);
    if (mode == "sender") {
        sender();
    } else if (mode == "receiver") {
        receiver();
    } else {
        echo::error("Unknown mode: ", mode.c_str());
        return 1;
    }

    return 0;
}
