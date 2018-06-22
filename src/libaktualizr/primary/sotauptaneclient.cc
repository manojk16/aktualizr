#include "sotauptaneclient.h"

#include <unistd.h>
#include <chrono>
#include <memory>
#include <utility>
#include "json/json.h"

#include "crypto/crypto.h"
#include "crypto/keymanager.h"
#include "initializer.h"
#include "logging/logging.h"
#include "package_manager/packagemanagerfactory.h"
#include "uptane/exceptions.h"
#include "uptane/secondaryconfig.h"
#include "uptane/secondaryfactory.h"
#include "utilities/utils.h"

SotaUptaneClient::SotaUptaneClient(Config &config_in, std::shared_ptr<event::Channel> events_channel_in,
                                   Uptane::DirectorRepository &director_repo_in,
                                   Uptane::ImagesRepository &images_repo_in, Uptane::Manifest &uptane_manifest_in,
                                   std::shared_ptr<INvStorage> storage_in, HttpInterface &http_client,
                                   const Bootloader &bootloader_in)
    : config(config_in),
      events_channel(std::move(std::move(events_channel_in))),
      director_repo(director_repo_in),
      images_repo(images_repo_in),
      uptane_manifest(uptane_manifest_in),
      storage(std::move(storage_in)),
      http(http_client),
      uptane_fetcher(config, storage, http),
      bootloader(bootloader_in),
      last_targets_version(-1) {
  // consider boot successful as soon as we started, missing internet connection or connection to secondaries are not
  // proper reasons to roll back
  pacman = PackageManagerFactory::makePackageManager(config.pacman, storage);
  if (pacman->imageUpdated()) {
    bootloader.setBootOK();
  }

  if (config.discovery.ipuptane) {
    IpSecondaryDiscovery ip_uptane_discovery{config.network};
    auto ipuptane_secs = ip_uptane_discovery.discover();
    config.uptane.secondary_configs.insert(config.uptane.secondary_configs.end(), ipuptane_secs.begin(),
                                           ipuptane_secs.end());
  }
  initSecondaries();
}

void SotaUptaneClient::schedulePoll(const std::shared_ptr<command::Channel> &commands_channel) {
  uint64_t polling_sec = config.uptane.polling_sec;
  std::thread([polling_sec, commands_channel, this]() {
    std::this_thread::sleep_for(std::chrono::seconds(polling_sec));
    *commands_channel << std::make_shared<command::GetUpdateRequests>();
    if (shutdown) {
      return;
    }
  }).detach();
}

bool SotaUptaneClient::isInstalledOnPrimary(const Uptane::Target &target) {
  if (target.ecus().find(uptane_manifest.getPrimaryEcuSerial()) != target.ecus().end()) {
    return target == pacman->getCurrent();
  }
  return false;
}

std::vector<Uptane::Target> SotaUptaneClient::findForEcu(const std::vector<Uptane::Target> &targets,
                                                         const Uptane::EcuSerial &ecu_id) {
  std::vector<Uptane::Target> result;
  for (auto it = targets.begin(); it != targets.end(); ++it) {
    // DEBUG
    for (auto ecu_it = it->ecus().begin(); ecu_it != it->ecus().end(); ++ecu_it) {
    }
    if (it->ecus().find(ecu_id) != it->ecus().end()) {
      result.push_back(*it);
    }
  }
  return result;
}

data::InstallOutcome SotaUptaneClient::PackageInstall(const Uptane::Target &target) {
  LOG_INFO << "Installing package using " << pacman->name() << " package manager";
  try {
    return pacman->install(target);
  } catch (std::exception &ex) {
    return data::InstallOutcome(data::UpdateResultCode::kInstallFailed, ex.what());
  }
}

void SotaUptaneClient::PackageInstallSetResult(const Uptane::Target &target) {
  if (((!target.format().empty() && target.format() != "OSTREE") || target.length() != 0) &&
      config.pacman.type == PackageManager::kOstree) {
    data::InstallOutcome outcome(data::UpdateResultCode::kValidationFailed,
                                 "Cannot install a non-OSTree package on an OSTree system");
    operation_result["operation_result"] = data::OperationResult::fromOutcome(target.filename(), outcome).toJson();
  } else {
    data::OperationResult result = data::OperationResult::fromOutcome(target.filename(), PackageInstall(target));
    operation_result["operation_result"] = result.toJson();
    if (result.result_code == data::UpdateResultCode::kOk) {
      storage->saveInstalledVersion(target);
    }
  }
}

void SotaUptaneClient::reportHwInfo() {
  Json::Value hw_info = Utils::getHardwareInfo();
  if (!hw_info.empty()) {
    http.put(config.tls.server + "/core/system_info", hw_info);
  }
}

void SotaUptaneClient::reportInstalledPackages() {
  http.put(config.tls.server + "/core/installed", pacman->getInstalledPackages());
}

void SotaUptaneClient::reportNetworkInfo() {
  if (config.telemetry.report_network) {
    LOG_DEBUG << "Reporting network information";
    Json::Value network_info = Utils::getNetworkInfo();
    if (network_info != last_network_info_reported) {
      HttpResponse response = http.put(config.tls.server + "/system_info/network", network_info);
      if (response.isOk()) {
        last_network_info_reported = network_info;
      }
    }

  } else {
    LOG_DEBUG << "Not reporting network information because telemetry is disabled";
  }
}

Json::Value SotaUptaneClient::AssembleManifest() {
  Json::Value result;
  installed_images.clear();
  Json::Value unsigned_ecu_version = pacman->getManifest(uptane_manifest.getPrimaryEcuSerial());

  if (operation_result != Json::nullValue) {
    unsigned_ecu_version["custom"] = operation_result;
  }

  installed_images[uptane_manifest.getPrimaryEcuSerial()] = unsigned_ecu_version["filepath"].asString();

  result[uptane_manifest.getPrimaryEcuSerial().ToString()] = uptane_manifest.signVersionManifest(unsigned_ecu_version);
  std::map<Uptane::EcuSerial, std::shared_ptr<Uptane::SecondaryInterface> >::iterator it;
  for (it = secondaries.begin(); it != secondaries.end(); it++) {
    Json::Value secmanifest = it->second->getManifest();
    if (secmanifest.isMember("signatures") && secmanifest.isMember("signed")) {
      auto public_key = it->second->getPublicKey();
      std::string canonical = Json::FastWriter().write(secmanifest["signed"]);
      bool verified = public_key.VerifySignature(secmanifest["signatures"][0]["sig"].asString(), canonical);
      if (verified) {
        result[it->first.ToString()] = secmanifest;
        installed_images[it->first] = secmanifest["filepath"].asString();
      } else {
        LOG_ERROR << "Secondary manifest verification failed, manifest: " << secmanifest;
      }
    } else {
      LOG_ERROR << "Secondary manifest is corrupted or not signed, manifest: " << secmanifest;
    }
  }
  return result;
}

bool SotaUptaneClient::hasPendingUpdates(const Json::Value &manifests) {
  for (auto manifest : manifests) {
    if (manifest["signed"].isMember("custom")) {
      auto status =
          static_cast<data::UpdateResultCode>(manifest["signed"]["custom"]["operation_result"]["result_code"].asUInt());
      if (status == data::UpdateResultCode::kInProgress) {
        return true;
      }
    }
  }
  return false;
}

bool SotaUptaneClient::initialize() {
  KeyManager keys(storage, config.keymanagerConfig());
  Initializer initializer(config.provision, storage, http, keys, secondaries);

  if (!initializer.isSuccessful()) {
    return false;
  }

  EcuSerials serials;
  if (!storage->loadEcuSerials(&serials) || serials.size() == 0) {
    return false;
  }

  uptane_manifest.setPrimaryEcuSerialHwId(serials[0]);
  hw_ids.insert(serials[0]);

  return true;
}

bool SotaUptaneClient::updateDirectorMeta() {
  // Uptane step 2 (download time) is not implemented yet.
  // Uptane step 3 (download metadata)

  // reset director repo to initial state before starting UPTANE iteration
  director_repo.resetMeta();
  // Load Initial Director Root Metadata
  {
    std::string director_root;
    if (storage->loadRole(&director_root, Uptane::RepositoryType::Director, Uptane::Role::Root())) {
      if (!director_repo.initRoot(director_root)) {
        last_exception = director_repo.getLastException();
        return false;
      }
    } else {
      if (!uptane_fetcher.fetchRole(&director_root, Uptane::RepositoryType::Director, Uptane::Role::Root(),
                                    Uptane::Version(1)))
        return false;
      if (!director_repo.initRoot(director_root)) {
        last_exception = director_repo.getLastException();
        return false;
      }
      storage->storeRole(director_root, Uptane::RepositoryType::Director, Uptane::Role::Root(), Uptane::Version(1));
    }
  }

  // Update Director Root Metadata
  {
    std::string director_root;
    if (!uptane_fetcher.fetchRole(&director_root, Uptane::RepositoryType::Director, Uptane::Role::Root())) return false;
    int remote_version = Uptane::extractVersionUntrusted(director_root);
    int local_version = director_repo.rootVersion();

    for (int version = local_version + 1; version <= remote_version; ++version) {
      if (!uptane_fetcher.fetchRole(&director_root, Uptane::RepositoryType::Director, Uptane::Role::Root(),
                                    Uptane::Version(version))) {
        return false;
      }

      if (!director_repo.verifyRoot(director_root)) {
        last_exception = director_repo.getLastException();
        return false;
      }
      storage->storeRole(director_root, Uptane::RepositoryType::Director, Uptane::Role::Root(),
                         Uptane::Version(version));
      storage->clearNonRootMeta(Uptane::RepositoryType::Director);
    }

    if (director_repo.rootExpired()) {
      last_exception = Uptane::ExpiredMetadata("director", "root");
      return false;
    }
  }
  // Update Director Targets Metadata
  {
    std::string director_targets;

    if (!uptane_fetcher.fetchRole(&director_targets, Uptane::RepositoryType::Director, Uptane::Role::Targets()))
      return false;
    int remote_version = Uptane::extractVersionUntrusted(director_targets);

    int local_version;
    std::string director_targets_stored;
    if (storage->loadRole(&director_targets_stored, Uptane::RepositoryType::Director, Uptane::Role::Targets())) {
      local_version = Uptane::extractVersionUntrusted(director_targets_stored);
    } else {
      local_version = -1;
    }

    if (!director_repo.verifyTargets(director_targets)) {
      last_exception = director_repo.getLastException();
      return false;
    }

    if (local_version > remote_version) {
      return false;
    } else if (local_version == remote_version) {
      return true;
    }
    storage->storeRole(director_targets, Uptane::RepositoryType::Director, Uptane::Role::Targets(),
                       Uptane::Version(remote_version));
    if (director_repo.targetsExpired()) {
      last_exception = Uptane::ExpiredMetadata("director", "targets");
      return false;
    }
  }

  return true;
}

bool SotaUptaneClient::updateImagesMeta() {
  images_repo.resetMeta();
  // Load Initial Images Root Metadata
  {
    std::string images_root;
    if (storage->loadRole(&images_root, Uptane::RepositoryType::Images, Uptane::Role::Root())) {
      if (!images_repo.initRoot(images_root)) {
        last_exception = images_repo.getLastException();
        return false;
      }
    } else {
      if (!uptane_fetcher.fetchRole(&images_root, Uptane::RepositoryType::Images, Uptane::Role::Root(),
                                    Uptane::Version(1)))
        return false;
      if (!images_repo.initRoot(images_root)) {
        last_exception = images_repo.getLastException();
        return false;
      }
      storage->storeRole(images_root, Uptane::RepositoryType::Images, Uptane::Role::Root(), Uptane::Version(1));
    }
  }

  // Update Image Roots Metadata
  {
    std::string images_root;
    if (!uptane_fetcher.fetchRole(&images_root, Uptane::RepositoryType::Images, Uptane::Role::Root())) return false;
    int remote_version = Uptane::extractVersionUntrusted(images_root);
    int local_version = images_repo.rootVersion();

    for (int version = local_version + 1; version <= remote_version; ++version) {
      if (!uptane_fetcher.fetchRole(&images_root, Uptane::RepositoryType::Images, Uptane::Role::Root(),
                                    Uptane::Version(version)))
        return false;
      if (!images_repo.verifyRoot(images_root)) {
        last_exception = images_repo.getLastException();
        return false;
      }
      storage->storeRole(images_root, Uptane::RepositoryType::Images, Uptane::Role::Root(), Uptane::Version(version));
      storage->clearNonRootMeta(Uptane::RepositoryType::Images);
    }

    if (images_repo.rootExpired()) {
      last_exception = Uptane::ExpiredMetadata("repo", "root");
      return false;
    }
  }

  // Update Images Timestamp Metadata
  {
    std::string images_timestamp;

    if (!uptane_fetcher.fetchRole(&images_timestamp, Uptane::RepositoryType::Images, Uptane::Role::Timestamp()))
      return false;
    int remote_version = Uptane::extractVersionUntrusted(images_timestamp);

    int local_version;
    std::string images_timestamp_stored;
    if (storage->loadRole(&images_timestamp_stored, Uptane::RepositoryType::Images, Uptane::Role::Timestamp())) {
      local_version = Uptane::extractVersionUntrusted(images_timestamp_stored);
    } else {
      local_version = -1;
    }

    if (!images_repo.verifyTimestamp(images_timestamp)) {
      last_exception = images_repo.getLastException();
      return false;
    }

    if (local_version > remote_version) {
      return false;
    } else if (local_version == remote_version) {
      return true;
    }
    storage->storeRole(images_timestamp, Uptane::RepositoryType::Images, Uptane::Role::Timestamp(),
                       Uptane::Version(remote_version));
    if (images_repo.timestampExpired()) {
      last_exception = Uptane::ExpiredMetadata("repo", "timestamp");
      return false;
    }
  }

  // Update Images Snapshot Metadata
  {
    std::string images_snapshot;

    if (!uptane_fetcher.fetchRole(&images_snapshot, Uptane::RepositoryType::Images, Uptane::Role::Snapshot()))
      return false;
    int remote_version = Uptane::extractVersionUntrusted(images_snapshot);

    int local_version;
    std::string images_snapshot_stored;
    if (storage->loadRole(&images_snapshot_stored, Uptane::RepositoryType::Images, Uptane::Role::Snapshot())) {
      local_version = Uptane::extractVersionUntrusted(images_snapshot_stored);
    } else {
      local_version = -1;
    }

    if (local_version > remote_version) {
      return false;
    } else if (local_version == remote_version) {
      return true;
    }

    if (!images_repo.verifySnapshot(images_snapshot)) {
      last_exception = images_repo.getLastException();
      return false;
    }

    storage->storeRole(images_snapshot, Uptane::RepositoryType::Images, Uptane::Role::Snapshot(),
                       Uptane::Version(remote_version));

    if (images_repo.snapshotExpired()) {
      last_exception = Uptane::ExpiredMetadata("repo", "snapshot");
      return false;
    }
  }

  // Update Images Targets Metadata
  {
    std::string images_targets;

    if (!uptane_fetcher.fetchRole(&images_targets, Uptane::RepositoryType::Images, Uptane::Role::Targets()))
      return false;
    int remote_version = Uptane::extractVersionUntrusted(images_targets);

    int local_version;
    std::string images_targets_stored;
    if (storage->loadRole(&images_targets_stored, Uptane::RepositoryType::Images, Uptane::Role::Targets())) {
      local_version = Uptane::extractVersionUntrusted(images_targets_stored);
    } else {
      local_version = -1;
    }

    if (local_version > remote_version) {
      return false;
    } else if (local_version == remote_version) {
      return true;
    }
    if (!images_repo.verifyTargets(images_targets)) {
      last_exception = images_repo.getLastException();
      return false;
    }
    storage->storeRole(images_targets, Uptane::RepositoryType::Images, Uptane::Role::Targets(),
                       Uptane::Version(remote_version));

    if (images_repo.targetsExpired()) {
      last_exception = Uptane::ExpiredMetadata("repo", "targets");
      return false;
    }
  }
  return true;
}

bool SotaUptaneClient::getNewTargets(std::vector<Uptane::Target> *new_targets) {
  std::vector<Uptane::Target> targets = director_repo.getTargets();
  for (auto targets_it = targets.cbegin(); targets_it != targets.cend(); ++targets_it) {
    bool is_new = true;
    for (auto ecus_it = targets_it->ecus().cbegin(); ecus_it != targets_it->ecus().cend(); ++ecus_it) {
      Uptane::EcuSerial ecu_serial = ecus_it->first;
      Uptane::HardwareIdentifier hw_id = ecus_it->second;
      auto hwid_it = hw_ids.find(ecu_serial);
      if (hwid_it == hw_ids.end()) {
        LOG_WARNING << "Unknown ECU ID in director targets metadata: " << ecu_serial.ToString();
        is_new = false;
        break;
      }

      if (hwid_it->second != hw_id) {
        LOG_ERROR << "Wrong hardware identifier for ECU " << ecu_serial.ToString();
        return false;
      }

      auto images_it = installed_images.find(ecu_serial);
      if (images_it == installed_images.end()) {
        LOG_WARNING << "Unknown ECU ID in director targets metadata: " << ecu_serial.ToString();
        is_new = false;
        break;
      }
      if (images_it->second == targets_it->filename()) {
        // no updates for this image
        continue;
      }
      is_new = true;
    }
    if (is_new) {
      new_targets->push_back(*targets_it);
    }
  }
  return true;
}

bool SotaUptaneClient::downloadTargets(const std::vector<Uptane::Target> &targets) {
  // Uptane step 4 - download all the images and verify them against the metadata (for OSTree - pull without
  // deploying)
  for (auto it = targets.cbegin(); it != targets.cend(); ++it) {
    // TODO: delegations
    auto images_target = images_repo.getTarget(*it);
    if (!images_target.get()) {
      LOG_ERROR << "No matching target in images targets metadata for " << *it;
      continue;
    }
    // TODO: support downloading encrypted targets from director
    // TODO: check if the file is already there before downloading
    uptane_fetcher.fetchVerifyTarget(*images_target);
  }
  return true;
}

bool SotaUptaneClient::uptaneIteration() {
  // Uptane step 1 (build the vehicle version manifest):
  if (!updateDirectorMeta()) {
    LOG_ERROR << "Failed to update director metadata";
    return false;
  }
  std::vector<Uptane::Target> targets;
  if (!getNewTargets(&targets)) {
    LOG_ERROR << "Inconsistency between director and images targets metadata";
    return false;
  }

  if (targets.empty()) {
    LOG_INFO << "no new updates, sending UptaneTimestampUpdated event";
    *events_channel << std::make_shared<event::UptaneTimestampUpdated>();
    return true;
  }

  LOG_INFO << "got new updates";

  if (!updateImagesMeta()) {
    LOG_ERROR << "Failed to update image metadata";
    return false;
  }

  if (!downloadTargets(targets)) {
    LOG_ERROR << "Failed to download targets";
    return false;
  }
  *events_channel << std::make_shared<event::UptaneTargetsUpdated>(targets);
  return true;
}

void SotaUptaneClient::runForever(const std::shared_ptr<command::Channel> &commands_channel) {
  LOG_DEBUG << "Checking if device is provisioned...";

  if (!initialize()) {
    throw std::runtime_error("Fatal error of tls or ecu device registration");
  }

  verifySecondaries();
  LOG_DEBUG << "... provisioned OK";
  reportHwInfo();
  reportInstalledPackages();
  reportNetworkInfo();

  schedulePoll(commands_channel);

  std::shared_ptr<command::BaseCommand> command;
  while (*commands_channel >> command) {
    LOG_INFO << "got " + command->variant + " command";

    try {
      if (command->variant == "GetUpdateRequests") {
        schedulePoll(commands_channel);
        if (!putManifest()) {
          LOG_ERROR << "Failed to send vehicle manifest";
          return;
        }
        reportNetworkInfo();
        uptaneIteration();
      } else if (command->variant == "UptaneInstall") {
        // install images
        // Uptane step 5 (send time to all ECUs) is not implemented yet.
        operation_result = Json::nullValue;
        std::vector<Uptane::Target> updates = command->toChild<command::UptaneInstall>()->packages;
        std::vector<Uptane::Target> primary_updates = findForEcu(updates, uptane_manifest.getPrimaryEcuSerial());
        //   6 - send metadata to all the ECUs
        sendMetadataToEcus(updates);

        //   7 - send images to ECUs (deploy for OSTree)
        if (primary_updates.size() != 0u) {
          // assuming one OSTree OS per primary => there can be only one OSTree update
          std::vector<Uptane::Target>::const_iterator it;
          for (it = primary_updates.begin(); it != primary_updates.end(); ++it) {
            if (!isInstalledOnPrimary(*it)) {
              // notify the bootloader before installation happens, because installation is not atomic and
              //   a false notification doesn't hurt when rollbacks are implemented
              bootloader.updateNotify();
              PackageInstallSetResult(*it);
            } else {
              data::InstallOutcome outcome(data::UpdateResultCode::kAlreadyProcessed, "Package already installed");
              operation_result["operation_result"] =
                  data::OperationResult::fromOutcome(it->filename(), outcome).toJson();
            }
            break;
          }
          // TODO: other updates for primary
        } else {
          LOG_INFO << "No update to install on primary";
        }

        sendImagesToEcus(updates);
        // Not required for Uptane, but used to send a status code to the
        // director.
        if (!putManifest()) {
          continue;
        }

        // FIXME how to deal with reboot if we have a pending secondary update?
        boost::filesystem::path reboot_flag = "/tmp/aktualizr_reboot_flag";
        if (boost::filesystem::exists(reboot_flag)) {
          boost::filesystem::remove(reboot_flag);
          if (getppid() == 1) {  // if parent process id is 1, aktualizr runs under systemd
            exit(0);             // aktualizr service runs with 'Restart=always' systemd option.
                                 // Systemd will start aktualizr automatically if it exit.
          } else {               // Aktualizr runs from terminal
            LOG_INFO << "Aktualizr has been updated and reqires restart to run new version.";
          }
        }
      } else if (command->variant == "Shutdown") {
        shutdown = true;
        return;
      }

    } catch (const Uptane::Exception &e) {
      LOG_ERROR << e.what();
      *events_channel << std::make_shared<event::UptaneTimestampUpdated>();
    } catch (const std::exception &ex) {
      LOG_ERROR << "Unknown exception was thrown: " << ex.what();
      *events_channel << std::make_shared<event::UptaneTimestampUpdated>();
    }
  }
}

bool SotaUptaneClient::putManifest() {
  auto manifest = AssembleManifest();
  if (!hasPendingUpdates(manifest)) {
    auto signed_manifest = uptane_manifest.signManifest(manifest);
    HttpResponse response = http.put(config.uptane.director_server + "/manifest", signed_manifest);
    return response.isOk();
  }
  return false;
}

void SotaUptaneClient::initSecondaries() {
  std::vector<Uptane::SecondaryConfig>::const_iterator it;
  for (it = config.uptane.secondary_configs.begin(); it != config.uptane.secondary_configs.end(); ++it) {
    std::shared_ptr<Uptane::SecondaryInterface> sec = Uptane::SecondaryFactory::makeSecondary(*it);

    Uptane::EcuSerial sec_serial = sec->getSerial();
    Uptane::HardwareIdentifier sec_hw_id = sec->getHwId();
    std::map<Uptane::EcuSerial, std::shared_ptr<Uptane::SecondaryInterface> >::const_iterator map_it =
        secondaries.find(sec_serial);
    if (map_it != secondaries.end()) {
      LOG_ERROR << "Multiple secondaries found with the same serial: " << sec_serial;
      continue;
    }
    secondaries.insert(std::make_pair(sec_serial, sec));
    hw_ids.insert(std::make_pair(sec_serial, sec_hw_id));
  }
}

// Check stored secondaries list against secondaries known to aktualizr via
// commandline input and legacy interface.
void SotaUptaneClient::verifySecondaries() {
  EcuSerials serials;
  if (!storage->loadEcuSerials(&serials) || serials.empty()) {
    LOG_ERROR << "No ECU serials found in storage!";
    return;
  }

  std::vector<MisconfiguredEcu> misconfigured_ecus;
  std::vector<bool> found(serials.size(), false);
  SerialCompare primary_comp(uptane_manifest.getPrimaryEcuSerial());
  EcuSerials::const_iterator store_it;
  store_it = std::find_if(serials.begin(), serials.end(), primary_comp);
  if (store_it == serials.end()) {
    LOG_ERROR << "Primary ECU serial " << uptane_manifest.getPrimaryEcuSerial() << " not found in storage!";
    misconfigured_ecus.emplace_back(store_it->first, store_it->second, EcuState::kOld);
  } else {
    found[std::distance(serials.cbegin(), store_it)] = true;
  }

  std::map<Uptane::EcuSerial, std::shared_ptr<Uptane::SecondaryInterface> >::const_iterator it;
  for (it = secondaries.cbegin(); it != secondaries.cend(); ++it) {
    SerialCompare secondary_comp(it->second->getSerial());
    store_it = std::find_if(serials.begin(), serials.end(), secondary_comp);
    if (store_it == serials.end()) {
      LOG_ERROR << "Secondary ECU serial " << it->second->getSerial() << " (hardware ID " << it->second->getHwId()
                << ") not found in storage!";
      misconfigured_ecus.emplace_back(it->second->getSerial(), it->second->getHwId(), EcuState::kNotRegistered);
    } else if (found[std::distance(serials.cbegin(), store_it)]) {
      LOG_ERROR << "Secondary ECU serial " << it->second->getSerial() << " (hardware ID " << it->second->getHwId()
                << ") has a duplicate entry in storage!";
    } else {
      found[std::distance(serials.cbegin(), store_it)] = true;
    }
  }

  std::vector<bool>::iterator found_it;
  for (found_it = found.begin(); found_it != found.end(); ++found_it) {
    if (!*found_it) {
      auto not_registered = serials[std::distance(found.begin(), found_it)];
      LOG_WARNING << "ECU serial " << not_registered.first << " in storage was not reported to aktualizr!";
      misconfigured_ecus.emplace_back(not_registered.first, not_registered.second, EcuState::kOld);
    }
  }
  storage->storeMisconfiguredEcus(misconfigured_ecus);
}

void SotaUptaneClient::rotateSecondaryRoot(Uptane::RepositoryType repo, Uptane::SecondaryInterface &secondary) {
  std::string latest_root;

  if (!storage->loadRole(&latest_root, repo, Uptane::Role::Root())) {
    LOG_ERROR << "No root metadata to send";
    return;
  }

  int last_root_version = Uptane::extractVersionUntrusted(latest_root);

  int sec_root_version = secondary.getRootVersion((repo == Uptane::RepositoryType::Director));
  if (sec_root_version >= 0) {
    for (int v = sec_root_version + 1; v <= last_root_version; v++) {
      std::string root;
      if (!storage->loadRole(&root, repo, Uptane::Role::Root(), Uptane::Version(v))) {
        LOG_WARNING << "Couldn't find root meta in the storage, trying remote repo";
        if (!uptane_fetcher.fetchRole(&root, repo, Uptane::Role::Root(), Uptane::Version(v))) {
          LOG_ERROR << "Root metadata could not be fetched, skipping to the next secondary";  // TODO: looks
                                                                                              // problematic, robust
                                                                                              // procedure needs to be
                                                                                              // defined
          return;
        }
      }
      if (!secondary.putRoot(root, repo == Uptane::RepositoryType::Director)) {
        LOG_ERROR << "Sending metadata to " << secondary.getSerial() << " failed";
      }
    }
  }
}

// TODO: the function can't currently return any errors. The problem of error reporting from secondaries should
// be solved on a system (backend+frontend) error.
// TODO: the function blocks until it updates all the secondaries. Consider non-blocking operation.
void SotaUptaneClient::sendMetadataToEcus(std::vector<Uptane::Target> targets) {
  Uptane::RawMetaPack meta;
  if (!storage->loadRole(&meta.director_root, Uptane::RepositoryType::Director, Uptane::Role::Root())) {
    LOG_ERROR << "No director root metadata to send";
    return;
  }
  if (!storage->loadRole(&meta.director_targets, Uptane::RepositoryType::Director, Uptane::Role::Targets())) {
    LOG_ERROR << "No director targets metadata to send";
    return;
  }
  if (!storage->loadRole(&meta.image_root, Uptane::RepositoryType::Images, Uptane::Role::Root())) {
    LOG_ERROR << "No images root metadata to send";
    return;
  }
  if (!storage->loadRole(&meta.image_timestamp, Uptane::RepositoryType::Images, Uptane::Role::Timestamp())) {
    LOG_ERROR << "No images timestamp metadata to send";
    return;
  }
  if (!storage->loadRole(&meta.image_snapshot, Uptane::RepositoryType::Images, Uptane::Role::Snapshot())) {
    LOG_ERROR << "No images snapshot metadata to send";
    return;
  }
  if (!storage->loadRole(&meta.image_targets, Uptane::RepositoryType::Images, Uptane::Role::Targets())) {
    LOG_ERROR << "No images targets metadata to send";
    return;
  }

  // target images should already have been downloaded to metadata_path/targets/
  for (auto targets_it = targets.cbegin(); targets_it != targets.cend(); ++targets_it) {
    for (auto ecus_it = targets_it->ecus().cbegin(); ecus_it != targets_it->ecus().cend(); ++ecus_it) {
      Uptane::EcuSerial ecu_serial = ecus_it->first;

      auto sec = secondaries.find(ecu_serial);
      if (sec != secondaries.end()) {
        /* Root rotation if necessary */
        rotateSecondaryRoot(Uptane::RepositoryType::Director, *(sec->second));
        rotateSecondaryRoot(Uptane::RepositoryType::Images, *(sec->second));
        if (!sec->second->putMetadata(meta)) {
          LOG_ERROR << "Sending metadata to " << sec->second->getSerial() << " failed";
        }
      }
    }
  }
}

void SotaUptaneClient::sendImagesToEcus(std::vector<Uptane::Target> targets) {
  // target images should already have been downloaded to metadata_path/targets/
  for (auto targets_it = targets.begin(); targets_it != targets.end(); ++targets_it) {
    for (auto ecus_it = targets_it->ecus().cbegin(); ecus_it != targets_it->ecus().cend(); ++ecus_it) {
      Uptane::EcuSerial ecu_serial = ecus_it->first;

      auto sec = secondaries.find(ecu_serial);
      if (sec == secondaries.end()) {
        continue;
      }

      if (sec->second->sconfig.secondary_type == Uptane::SecondaryType::kOpcuaUptane) {
        Json::Value data;
        data["sysroot_path"] = config.pacman.sysroot.string();
        data["ref_hash"] = targets_it->sha256Hash();
        sec->second->sendFirmware(Utils::jsonToStr(data));
        continue;
      }

      std::stringstream sstr;
      sstr << *storage->openTargetFile(targets_it->filename());
      std::string fw = sstr.str();

      if (fw.empty()) {
        // empty firmware means OSTree secondaries: pack credentials instead
        std::string creds_archive = secondaryTreehubCredentials();
        if (creds_archive.empty()) {
          continue;
        }
        sec->second->sendFirmware(creds_archive);
      } else {
        sec->second->sendFirmware(fw);
      }
    }
  }
}

std::string SotaUptaneClient::secondaryTreehubCredentials() const {
  if (config.tls.pkey_source != CryptoSource::kFile || config.tls.cert_source != CryptoSource::kFile ||
      config.tls.ca_source != CryptoSource::kFile) {
    LOG_ERROR << "Cannot send OSTree update to a secondary when not using file as credential sources";
    return "";
  }
  std::string ca, cert, pkey;
  if (!storage->loadTlsCreds(&ca, &cert, &pkey)) {
    LOG_ERROR << "Could not load tls credentials from storage";
    return "";
  }

  std::string treehub_url = config.pacman.ostree_server;
  std::map<std::string, std::string> archive_map = {
      {"ca.pem", ca}, {"client.pem", cert}, {"pkey.pem", pkey}, {"server.url", treehub_url}};

  try {
    std::stringstream as;
    Utils::writeArchive(archive_map, as);

    return as.str();
  } catch (std::runtime_error &exc) {
    LOG_ERROR << "Could not create credentials archive: " << exc.what();
    return "";
  }
}
