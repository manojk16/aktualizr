#ifndef UPTANE_REPOSITORY_H_
#define UPTANE_REPOSITORY_H_

#include <vector>

#include "json/json.h"

#include "config/config.h"
#include "crypto/crypto.h"
#include "crypto/keymanager.h"
#include "logging/logging.h"
#include "storage/invstorage.h"

namespace Uptane {

class Manifest {
 public:
  Manifest(const Config &config_in, std::shared_ptr<INvStorage> storage_in)
      : config_{config_in}, storage_{std::move(storage_in)}, keys_(storage_, config_in.keymanagerConfig()) {}

  Json::Value signManifest(const Json::Value &version_manifests);
  Json::Value signVersionManifest(const Json::Value &primary_version_manifests);

  void setPrimaryEcuSerialHwId(const std::pair<Uptane::EcuSerial, Uptane::HardwareIdentifier> &serials) {
    primary_ecu_serial = serials.first;
    primary_hardware_id = serials.second;
  }

  EcuSerial getPrimaryEcuSerial() { return primary_ecu_serial; }

 private:
  Uptane::EcuSerial primary_ecu_serial{Uptane::EcuSerial::Unknown()};
  Uptane::HardwareIdentifier primary_hardware_id{Uptane::HardwareIdentifier::Unknown()};
  const Config &config_;
  std::shared_ptr<INvStorage> storage_;
  KeyManager keys_;
};

class RepositoryCommon {
 public:
  RepositoryCommon(RepositoryType type_in) : type{type_in} {}
  bool initRoot(const std::string &root_raw);
  bool verifyRoot(const std::string &root_raw);
  int rootVersion() { return root.version(); }
  bool rootExpired() { return root.isExpired(TimeStamp::Now()); }

 protected:
  void resetRoot();
  Root root;
  RepositoryType type;
};
}  // namespace Uptane

#endif
