#include "envoy/extensions/transport_sockets/tcp_stats/v3/tcp_stats.pb.h"
#include "envoy/extensions/transport_sockets/tcp_stats/v3/tcp_stats.pb.validate.h"
#include "envoy/registry/registry.h"
#include "envoy/server/transport_socket_config.h"

#include "source/common/config/utility.h"
#include "source/extensions/transport_sockets/tcp_stats/tcp_stats.h"

namespace Envoy {
namespace Extensions {
namespace TransportSockets {
namespace TcpStats {

class TcpStatsSocketFactory : public Network::TransportSocketFactory {
public:
  TcpStatsSocketFactory(Server::Configuration::TransportSocketFactoryContext& context,
                        const envoy::extensions::transport_sockets::tcp_stats::v3::Config& config,
                        Network::TransportSocketFactoryPtr&& inner_factory)
      : inner_factory_(std::move(inner_factory)) {
#if defined(__linux__)
    config_ = std::make_shared<Config>(config, context.scope());
#else
    UNREFERENCED_PARAMETER(config);
    UNREFERENCED_PARAMETER(context);
    throw EnvoyException("envoy.transport_sockets.tcp_stats is not supported on this platform.");
#endif
  }

  Network::TransportSocketPtr
  createTransportSocket(Network::TransportSocketOptionsConstSharedPtr options) const override {
#if defined(__linux__)
    return std::make_unique<TcpStatsSocket>(config_,
                                            inner_factory_->createTransportSocket(options));
#else
    UNREFERENCED_PARAMETER(options);
    return nullptr;
#endif
  }

  bool implementsSecureTransport() const override {
    return inner_factory_->implementsSecureTransport();
  }

  bool usesProxyProtocolOptions() const override {
    return inner_factory_->usesProxyProtocolOptions();
  }

private:
  Network::TransportSocketFactoryPtr inner_factory_;
#if defined(__linux__)
  ConfigConstSharedPtr config_;
#endif
};

class TcpStatsConfigFactory : public virtual Server::Configuration::TransportSocketConfigFactory {
public:
  std::string name() const override { return "envoy.transport_sockets.tcp_stats"; }
  ProtobufTypes::MessagePtr createEmptyConfigProto() override {
    return std::make_unique<envoy::extensions::transport_sockets::tcp_stats::v3::Config>();
  }
};

class UpstreamTcpStatsConfigFactory
    : public Server::Configuration::UpstreamTransportSocketConfigFactory,
      public TcpStatsConfigFactory {
public:
  Network::TransportSocketFactoryPtr createTransportSocketFactory(
      const Protobuf::Message& config,
      Server::Configuration::TransportSocketFactoryContext& context) override {
    const auto& outer_config = MessageUtil::downcastAndValidate<
        const envoy::extensions::transport_sockets::tcp_stats::v3::Config&>(
        config, context.messageValidationVisitor());
    auto& inner_config_factory = Envoy::Config::Utility::getAndCheckFactory<
        Server::Configuration::UpstreamTransportSocketConfigFactory>(
        outer_config.transport_socket());
    ProtobufTypes::MessagePtr inner_factory_config =
        Envoy::Config::Utility::translateToFactoryConfig(outer_config.transport_socket(),
                                                         context.messageValidationVisitor(),
                                                         inner_config_factory);
    auto inner_transport_factory =
        inner_config_factory.createTransportSocketFactory(*inner_factory_config, context);
    return std::make_unique<TcpStatsSocketFactory>(context, outer_config,
                                                   std::move(inner_transport_factory));
  }
};

class DownstreamTcpStatsConfigFactory
    : public Server::Configuration::DownstreamTransportSocketConfigFactory,
      public TcpStatsConfigFactory {
public:
  Network::TransportSocketFactoryPtr
  createTransportSocketFactory(const Protobuf::Message& config,
                               Server::Configuration::TransportSocketFactoryContext& context,
                               const std::vector<std::string>& server_names) override {
    const auto& outer_config = MessageUtil::downcastAndValidate<
        const envoy::extensions::transport_sockets::tcp_stats::v3::Config&>(
        config, context.messageValidationVisitor());
    auto& inner_config_factory = Envoy::Config::Utility::getAndCheckFactory<
        Server::Configuration::DownstreamTransportSocketConfigFactory>(
        outer_config.transport_socket());
    ProtobufTypes::MessagePtr inner_factory_config =
        Envoy::Config::Utility::translateToFactoryConfig(outer_config.transport_socket(),
                                                         context.messageValidationVisitor(),
                                                         inner_config_factory);
    auto inner_transport_factory = inner_config_factory.createTransportSocketFactory(
        *inner_factory_config, context, server_names);
    return std::make_unique<TcpStatsSocketFactory>(context, outer_config,
                                                   std::move(inner_transport_factory));
  }
};

REGISTER_FACTORY(UpstreamTcpStatsConfigFactory,
                 Server::Configuration::UpstreamTransportSocketConfigFactory);

REGISTER_FACTORY(DownstreamTcpStatsConfigFactory,
                 Server::Configuration::DownstreamTransportSocketConfigFactory);

} // namespace TcpStats
} // namespace TransportSockets
} // namespace Extensions
} // namespace Envoy
