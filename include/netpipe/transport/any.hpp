#pragma once

#include <memory>

#include <netpipe/core/endpoint.hpp>
#include <netpipe/core/stream.hpp>

#include <netpipe/core/common.hpp>

#include <netpipe/transport/stream/ipc.hpp>
#include <netpipe/transport/stream/shm.hpp>
#include <netpipe/transport/stream/tcp.hpp>

namespace netpipe {

    /// A single endpoint type that can represent any stream transport.
    ///
    /// This exists to avoid the historical limitation of netpipe::Stream where
    /// connect()/listen() take a TcpEndpoint even for IPC/SHM.
    struct AnyEndpoint {
        enum class Type {
            TCP,
            IPC,
            SHM,
        };

        Type type;

        // TCP
        TcpEndpoint tcp;
        // IPC
        IpcEndpoint ipc;
        // SHM
        ShmEndpoint shm;

        static AnyEndpoint tcp_endpoint(dp::String host, dp::u16 port) {
            AnyEndpoint ep{};
            ep.type = Type::TCP;
            ep.tcp = TcpEndpoint{host, port};
            return ep;
        }

        static AnyEndpoint ipc_endpoint(dp::String path) {
            AnyEndpoint ep{};
            ep.type = Type::IPC;
            ep.ipc = IpcEndpoint{path};
            return ep;
        }

        static AnyEndpoint shm_endpoint(dp::String name, dp::usize size) {
            AnyEndpoint ep{};
            ep.type = Type::SHM;
            ep.shm = ShmEndpoint{name, size};
            return ep;
        }
    };

    /// Type-erased stream wrapper that can hold TCP/IPC/SHM and expose one API.
    ///
    /// Non-breaking: this is additive; existing netpipe::Stream/TcpStream/etc
    /// remain unchanged.
    class AnyStream {
      public:
        AnyStream() = default;
        explicit AnyStream(std::unique_ptr<Stream> stream) : stream_(std::move(stream)) {}

        AnyStream(const AnyStream &) = delete;
        AnyStream &operator=(const AnyStream &) = delete;
        AnyStream(AnyStream &&) noexcept = default;
        AnyStream &operator=(AnyStream &&) noexcept = default;

        static AnyStream tcp() { return AnyStream(std::unique_ptr<Stream>(static_cast<Stream *>(new TcpStream()))); }
        static AnyStream ipc() { return AnyStream(std::unique_ptr<Stream>(static_cast<Stream *>(new IpcStream()))); }
        static AnyStream shm() { return AnyStream(std::unique_ptr<Stream>(static_cast<Stream *>(new ShmStream()))); }

        inline dp::Res<void> connect(const AnyEndpoint &endpoint) {
            switch (endpoint.type) {
            case AnyEndpoint::Type::TCP:
                stream_ = std::unique_ptr<Stream>(static_cast<Stream *>(new TcpStream()));
                return stream_->connect(endpoint.tcp);
            case AnyEndpoint::Type::IPC: {
                auto ipc = std::unique_ptr<IpcStream>(new IpcStream());
                auto res = ipc->connect_ipc(endpoint.ipc);
                if (res.is_err()) {
                    return res;
                }
                stream_.reset(ipc.release());
                return dp::result::ok();
            }
            case AnyEndpoint::Type::SHM: {
                auto shm = std::unique_ptr<ShmStream>(new ShmStream());
                auto res = shm->connect_shm(endpoint.shm);
                if (res.is_err()) {
                    return res;
                }
                stream_.reset(shm.release());
                return dp::result::ok();
            }
            }
            return dp::result::err(dp::Error::invalid_argument("unknown endpoint type"));
        }

        inline dp::Res<void> listen(const AnyEndpoint &endpoint) {
            switch (endpoint.type) {
            case AnyEndpoint::Type::TCP:
                stream_ = std::unique_ptr<Stream>(static_cast<Stream *>(new TcpStream()));
                return stream_->listen(endpoint.tcp);
            case AnyEndpoint::Type::IPC: {
                auto ipc = std::unique_ptr<IpcStream>(new IpcStream());
                auto res = ipc->listen_ipc(endpoint.ipc);
                if (res.is_err()) {
                    return res;
                }
                stream_.reset(ipc.release());
                return dp::result::ok();
            }
            case AnyEndpoint::Type::SHM: {
                auto shm = std::unique_ptr<ShmStream>(new ShmStream());
                auto res = shm->listen_shm(endpoint.shm);
                if (res.is_err()) {
                    return res;
                }
                stream_.reset(shm.release());
                return dp::result::ok();
            }
            }
            return dp::result::err(dp::Error::invalid_argument("unknown endpoint type"));
        }

        inline dp::Res<AnyStream> accept() {
            if (!stream_) {
                return dp::result::err(dp::Error::not_found("stream not initialized"));
            }

            auto res = stream_->accept();
            if (res.is_err()) {
                return dp::result::err(res.error());
            }
            return dp::result::ok(AnyStream(std::move(res.value())));
        }

        dp::Res<void> send(const Message &msg) {
            if (!stream_) {
                return dp::result::err(dp::Error::not_found("stream not initialized"));
            }
            return stream_->send(msg);
        }

        dp::Res<Message> recv() {
            if (!stream_) {
                return dp::result::err(dp::Error::not_found("stream not initialized"));
            }
            return stream_->recv();
        }

        dp::Res<void> set_recv_timeout(dp::u32 timeout_ms) {
            if (!stream_) {
                return dp::result::err(dp::Error::not_found("stream not initialized"));
            }
            return stream_->set_recv_timeout(timeout_ms);
        }

        void close() {
            if (stream_) {
                stream_->close();
            }
        }

        bool is_connected() const { return stream_ ? stream_->is_connected() : false; }
        Stream *get() { return stream_.get(); }
        const Stream *get() const { return stream_.get(); }

      private:
        std::unique_ptr<Stream> stream_;
    };

} // namespace netpipe
