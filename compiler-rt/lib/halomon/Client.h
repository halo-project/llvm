#pragma once

#include "boost/asio.hpp"

#include "Messages.pb.h"
#include "MessageKind.h"
#include "Channel.h"

#include <cinttypes>
#include "Logging.h"

namespace asio = boost::asio;
namespace ip = boost::asio::ip;

namespace halo {

class Client {
private:
  asio::io_service IOService;
  ip::tcp::resolver Resolver;
  ip::tcp::socket Socket;
  ip::tcp::resolver::query Query;
  ip::tcp::endpoint Endpoint;
  bool Connected{false};

  void connect_handler(boost::system::error_code const &Err) {
    if (Err) {
      logs(LC_Info) << "Failed to connect to "
              << endpoint_name() << " (" << Err.message() << ")\n";
      // we have to close the socket manually
      Socket.close();
      Connected = false;
    } else {
      logs(LC_Info) << "Connected to: " << endpoint_name() << "\n";
      Connected = true;
    }
  }

public:
  Channel Chan;

  Client(std::string const& server_hostname, std::string const& port) :
    IOService(),
    Resolver(IOService),
    Socket(IOService),
    Query(server_hostname, port),
    Endpoint(*Resolver.resolve(Query)),
    Chan(Socket) {}

  // returns number of handlers that were run
  size_t poll() {
    return IOService.poll();
  }

  std::string endpoint_name() const {
    std::stringstream ss;
    ss << Endpoint;
    return ss.str();
  }

  // returns number of handlers that were run
  size_t run_one_for(unsigned milliseconds) {
    return IOService.run_one_for(asio::chrono::milliseconds(milliseconds));
  }

  // returns number of handlers that were run
  size_t run_for(unsigned milliseconds) {
    return IOService.run_for(asio::chrono::milliseconds(milliseconds));
  }

  // returns true if connection has been established.
  bool connected() const { return Connected; }

  void blocking_connect() {
    boost::system::error_code Err;
    Socket.connect(Endpoint, Err);
    connect_handler(Err);
  }

  // FIXME: I have no idea why using this causes the samples to not
  // be sent to the server correctly. The server sees no samples
  // but the client successfully registers. For some reason only
  // the blocking connect actually fully works.
  void async_connect() {
    assert(!Connected && "why try connecting again?");

    Socket.async_connect(Endpoint, [&](boost::system::error_code const &Err) {
      connect_handler(Err);
    });
  }

};

} // namespace halo
