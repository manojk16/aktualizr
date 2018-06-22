#ifndef FSSTORAGE_H_
#define FSSTORAGE_H_

#include <boost/filesystem.hpp>
#include "invstorage.h"

class FSStorage : public INvStorage {
 public:
  explicit FSStorage(const StorageConfig& config, bool migration_only = false);
  ~FSStorage() override = default;
  void storePrimaryKeys(const std::string& public_key, const std::string& private_key) override;
  bool loadPrimaryKeys(std::string* public_key, std::string* private_key) override;
  bool loadPrimaryPublic(std::string* public_key) override;
  bool loadPrimaryPrivate(std::string* private_key) override;
  void clearPrimaryKeys() override;

  void storeTlsCreds(const std::string& ca, const std::string& cert, const std::string& pkey) override;
  void storeTlsCa(const std::string& ca) override;
  void storeTlsCert(const std::string& cert) override;
  void storeTlsPkey(const std::string& pkey) override;
  bool loadTlsCreds(std::string* ca, std::string* cert, std::string* pkey) override;
  void clearTlsCreds() override;
  bool loadTlsCa(std::string* ca) override;
  bool loadTlsCert(std::string* cert) override;
  bool loadTlsPkey(std::string* pkey) override;

  void storeRole(const std::string& data, Uptane::RepositoryType repo, Uptane::Role role, Uptane::Version version);
  bool loadRole(std::string* data, Uptane::RepositoryType repo, Uptane::Role role,
                Uptane::Version version = Uptane::Version());
  void clearNonRootMeta(Uptane::RepositoryType repo);
  void clearMetadata();

  void storeDeviceId(const std::string& device_id) override;
  bool loadDeviceId(std::string* device_id) override;
  void clearDeviceId() override;
  void storeEcuSerials(const EcuSerials& serials) override;
  bool loadEcuSerials(EcuSerials* serials) override;
  void clearEcuSerials() override;
  void storeMisconfiguredEcus(const std::vector<MisconfiguredEcu>& ecus) override;
  bool loadMisconfiguredEcus(std::vector<MisconfiguredEcu>* ecus) override;
  void clearMisconfiguredEcus() override;
  void storeEcuRegistered() override;
  bool loadEcuRegistered() override;
  void clearEcuRegistered() override;
  void storeInstalledVersions(const std::vector<Uptane::Target>& installed_versions,
                              const std::string& current_hash) override;
  std::string loadInstalledVersions(std::vector<Uptane::Target>* installed_versions) override;
  void clearInstalledVersions() override;
  std::unique_ptr<StorageTargetWHandle> allocateTargetFile(bool from_director, const std::string& filename,
                                                           size_t size) override;
  std::unique_ptr<StorageTargetRHandle> openTargetFile(const std::string& filename) override;
  void removeTargetFile(const std::string& filename) override;
  void cleanUp() override;
  StorageType type() override { return StorageType::kFileSystem; };

  friend class FSTargetWHandle;
  friend class FSTargetRHandle;

 private:
  // descriptors of currently downloaded files
  std::map<std::string, FILE*> director_files;
  std::map<std::string, FILE*> image_files;

  std::map<std::pair<Uptane::RepositoryType, Uptane::Role>, Uptane::Version> latest_versions;

  boost::filesystem::path targetFilepath(const std::string& filename) const;
  bool loadTlsCommon(std::string* data, const boost::filesystem::path& path_in);

  bool splitNameRoleVersion(const std::string& full_name, std::string* role_name, int* version);
  Uptane::Version findMaxVersion(const boost::filesystem::path meta_directory, Uptane::Role role);
};

#endif  // FSSTORAGE_H_
