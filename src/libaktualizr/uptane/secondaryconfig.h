#ifndef UPTANE_SECONDARYCONFIG_H_
#define UPTANE_SECONDARYCONFIG_H_

#include <string>

#include <sys/socket.h>
#include <boost/filesystem.hpp>
#include "utilities/exceptions.h"
#include "utilities/types.h"
#include "utilities/utils.h"

namespace Uptane {

enum class SecondaryType {

  kVirtual,  // Virtual secondary (in-process fake implementation).

  kLegacy,  // legacy non-UPTANE secondary. All the UPTANE metadata is managed locally. All commands are sent to an
            // external firmware loader via shell.

  kOpcuaUptane,  // Uptane protocol over OPC-UA

  kIpUptane,  // Custom Uptane protocol over TCP/IP network

  kVirtualUptane,  // Partial UPTANE secondary implemented inside primary
  kIsoTpUptane,    // Custom Uptane protocol over ISO/TP network

};

class SecondaryConfig {
 public:
  SecondaryConfig() = default;
  SecondaryConfig(const boost::filesystem::path &config_file) {
    if (!boost::filesystem::exists(config_file)) {
      throw FatalException(config_file.string() + " does not exist!");
    }
    Json::Value config_json = Utils::parseJSONFile(config_file);

    std::string stype = config_json["secondary_type"].asString();
    if (stype == "virtual") {
      secondary_type = Uptane::SecondaryType::kVirtual;
    } else if (stype == "legacy") {
      throw FatalException("Legacy secondaries should be initialized with --legacy-interface.");
    } else if (stype == "ip_uptane") {
      secondary_type = Uptane::SecondaryType::kIpUptane;
    } else if (stype == "opcua_uptane") {
      secondary_type = Uptane::SecondaryType::kOpcuaUptane;
    } else if (stype == "isotp_uptane") {
      secondary_type = Uptane::SecondaryType::kIsoTpUptane;
    } else {
      throw FatalException(std::string("Unrecognized secondary type: ") + stype);
    }
    ecu_serial = config_json["ecu_serial"].asString();
    ecu_hardware_id = config_json["ecu_hardware_id"].asString();
    partial_verifying = config_json["partial_verifying"].asBool();
    ecu_private_key = config_json["ecu_private_key"].asString();
    ecu_public_key = config_json["ecu_public_key"].asString();

    full_client_dir = boost::filesystem::path(config_json["full_client_dir"].asString());
    firmware_path = boost::filesystem::path(config_json["firmware_path"].asString());
    metadata_path = boost::filesystem::path(config_json["metadata_path"].asString());
    target_name_path = boost::filesystem::path(config_json["target_name_path"].asString());
    flasher = "";

    std::string key_type_str = config_json["key_type"].asString();
    if (key_type_str.size() != 0u) {
      if (key_type_str == "RSA2048") {
        key_type = KeyType::kRSA2048;
      } else if (key_type_str == "RSA3072") {
        key_type = KeyType::kRSA3072;
      } else if (key_type_str == "RSA4096") {
        key_type = KeyType::kRSA4096;
      } else if (key_type_str == "ED25519") {
        key_type = KeyType::kED25519;
      }
    }
    try {
      can_id = static_cast<uint16_t>(stoi(config_json["can_id"].asString(), nullptr, 16));
    } catch (...) {
      can_id = 0;
    }
    can_iface = config_json["can_interface"].asString();
  }
  SecondaryType secondary_type{};
  std::string ecu_serial;
  std::string ecu_hardware_id;
  bool partial_verifying{};
  std::string ecu_private_key;
  std::string ecu_public_key;
  KeyType key_type{KeyType::kRSA2048};

  std::string opcua_lds_url;

  boost::filesystem::path full_client_dir;   // SecondaryType::kVirtual, SecondaryType::kLegacy
  boost::filesystem::path firmware_path;     // SecondaryType::kVirtual, SecondaryType::kLegacy
  boost::filesystem::path metadata_path;     // SecondaryType::kVirtual, SecondaryType::kLegacy
  boost::filesystem::path target_name_path;  // SecondaryType::kVirtual, SecondaryType::kLegacy

  boost::filesystem::path flasher;  // SecondaryType::kLegacy

  sockaddr_storage ip_addr{};  // SecondaryType::kIpUptane

  uint16_t can_id;        // SecondaryType::kIsoTpUptane;
  std::string can_iface;  // SecondaryType::kIsoTpUptane;
};
}  // namespace Uptane

#endif
