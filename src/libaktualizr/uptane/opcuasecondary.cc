#include "opcuasecondary.h"

#include "opcuabridge/opcuabridgeclient.h"
#include "secondaryconfig.h"

#include "logging/logging.h"
#include "package_manager/ostreereposync.h"
#include "utilities/utils.h"

#include <algorithm>
#include <iterator>

#include <boost/filesystem.hpp>
#include <boost/iostreams/device/mapped_file.hpp>

namespace fs = boost::filesystem;

namespace Uptane {

OpcuaSecondary::OpcuaSecondary(const SecondaryConfig& sconfig_in) : SecondaryInterface(sconfig_in) {}

OpcuaSecondary::~OpcuaSecondary() = default;

Uptane::EcuSerial OpcuaSecondary::getSerial() {
  opcuabridge::Client client{opcuabridge::SelectEndPoint(SecondaryInterface::sconfig)};
  return client.recvConfiguration().getSerial();
}
Uptane::HardwareIdentifier OpcuaSecondary::getHwId() {
  opcuabridge::Client client{opcuabridge::SelectEndPoint(SecondaryInterface::sconfig)};
  return Uptane::HardwareIdentifier(client.recvConfiguration().getHwId());
}
PublicKey OpcuaSecondary::getPublicKey() {
  opcuabridge::Client client{opcuabridge::SelectEndPoint(SecondaryInterface::sconfig)};
  return PublicKey(client.recvConfiguration().getPublicKey(), client.recvConfiguration().getPublicKeyType());
}

Json::Value OpcuaSecondary::getManifest() {
  opcuabridge::Client client{opcuabridge::SelectEndPoint(SecondaryInterface::sconfig)};
  auto original_manifest = client.recvOriginalManifest().getBlock();
  return Utils::parseJSON(std::string(original_manifest.begin(), original_manifest.end()));
}

bool OpcuaSecondary::putMetadata(const RawMetaPack& meta_pack) {
  std::vector<opcuabridge::MetadataFile> metadatafiles;
  {
    opcuabridge::MetadataFile mf;
    mf.setMetadata(meta_pack.director_root);
    metadatafiles.push_back(mf);
  }
  {
    opcuabridge::MetadataFile mf;
    mf.setMetadata(meta_pack.director_targets);
    metadatafiles.push_back(mf);
  }
  opcuabridge::Client client{opcuabridge::SelectEndPoint(SecondaryInterface::sconfig)};
  return client.sendMetadataFiles(metadatafiles);
}

std::future<bool> OpcuaSecondary::sendFirmwareAsync(const std::shared_ptr<std::string>& data) {
  return std::async(std::launch::async, &OpcuaSecondary::sendFirmware, this, data);
}

bool OpcuaSecondary::sendFirmware(const std::shared_ptr<std::string>& data) {
  sendEvent<event::InstallStarted>(getSerial());

  Json::Value data_json = Utils::parseJSON(*data);

  const fs::path source_repo_dir_path(ostree_repo_sync::GetOstreeRepoPath(data_json["sysroot_path"].asString()));

  opcuabridge::Client client{opcuabridge::SelectEndPoint(SecondaryInterface::sconfig)};
  if (!client) {
    sendEvent<event::InstallComplete>(getSerial(), false);
    return false;
  }

  bool retval = true;
  if (ostree_repo_sync::ArchiveModeRepo(source_repo_dir_path)) {
    retval = client.syncDirectoryFiles(source_repo_dir_path);
  } else {
    TemporaryDirectory temp_dir("opcuabridge-ostree-sync-working-repo");
    const fs::path working_repo_dir_path = temp_dir.Path();

    if (!ostree_repo_sync::LocalPullRepo(source_repo_dir_path, working_repo_dir_path,
                                         data_json["ref_hash"].asString())) {
      LOG_ERROR << "OSTree repo sync failed: unable to local pull from " << source_repo_dir_path.native();
      sendEvent<event::InstallComplete>(getSerial(), false);
      return false;
    }
    retval = client.syncDirectoryFiles(working_repo_dir_path);
  }
  sendEvent<event::InstallComplete>(getSerial(), retval);
  return retval;
}

int OpcuaSecondary::getRootVersion(bool /* director */) {
  LOG_ERROR << "OpcuaSecondary::getRootVersion is not implemented yet";
  return 0;
}

bool OpcuaSecondary::putRoot(const std::string& /* root */, bool /* director */) {
  LOG_ERROR << "OpcuaSecondary::putRoot is not implemented yet";
  return false;
}

}  // namespace Uptane
