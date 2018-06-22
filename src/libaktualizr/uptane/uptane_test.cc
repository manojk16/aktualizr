#include <gtest/gtest.h>

#include <algorithm>
#include <fstream>
#include <iostream>
#include <memory>
#include <string>
#include <vector>

#include <boost/filesystem.hpp>

#include "crypto/p11engine.h"
#include "httpfake.h"
#include "logging/logging.h"
#include "primary/initializer.h"
#include "primary/sotauptaneclient.h"
#include "storage/fsstorage.h"
#include "test_utils.h"
#include "uptane/tuf.h"
#include "uptane/uptanerepository.h"
#include "utilities/utils.h"

#ifdef BUILD_P11
#ifndef TEST_PKCS11_MODULE_PATH
#define TEST_PKCS11_MODULE_PATH "/usr/local/softhsm/libsofthsm2.so"
#endif
#endif

boost::filesystem::path sysroot;

TEST(Uptane, Verify) {
  TemporaryDirectory temp_dir;
  HttpFake http(temp_dir.Path());
  Config config;
  config.uptane.director_server = http.tls_server + "/director";
  config.uptane.repo_server = http.tls_server + "/repo";

  config.storage.path = temp_dir.Path();
  auto storage = INvStorage::newStorage(config.storage);
  HttpResponse response = http.get(http.tls_server + "/director/root.json");
  Uptane::Root root(Uptane::Root::Policy::kAcceptAll);
  Uptane::Root(Uptane::RepositoryType::Director, response.getJson(), root);
}

TEST(Uptane, VerifyDataBad) {
  TemporaryDirectory temp_dir;
  HttpFake http(temp_dir.Path());
  Config config;
  config.uptane.director_server = http.tls_server + "/director";
  config.uptane.repo_server = http.tls_server + "/repo";

  config.storage.path = temp_dir.Path();
  auto storage = INvStorage::newStorage(config.storage);
  Json::Value data_json = http.get(http.tls_server + "/director/root.json").getJson();
  data_json.removeMember("signatures");

  Uptane::Root root(Uptane::Root::Policy::kAcceptAll);
  EXPECT_THROW(Uptane::Root(Uptane::RepositoryType::Director, data_json, root), Uptane::UnmetThreshold);
}

TEST(Uptane, VerifyDataUnknownType) {
  TemporaryDirectory temp_dir;
  HttpFake http(temp_dir.Path());
  Config config;
  config.uptane.director_server = http.tls_server + "/director";
  config.uptane.repo_server = http.tls_server + "/repo";

  config.storage.path = temp_dir.Path();
  auto storage = INvStorage::newStorage(config.storage);
  Json::Value data_json = http.get(http.tls_server + "/director/root.json").getJson();
  data_json["signatures"][0]["method"] = "badsignature";
  data_json["signatures"][1]["method"] = "badsignature";

  Uptane::Root root(Uptane::Root::Policy::kAcceptAll);
  EXPECT_THROW(Uptane::Root(Uptane::RepositoryType::Director, data_json, root), Uptane::SecurityException);
}

TEST(Uptane, VerifyDataBadKeyId) {
  TemporaryDirectory temp_dir;
  HttpFake http(temp_dir.Path());
  Config config;
  config.uptane.director_server = http.tls_server + "/director";
  config.uptane.repo_server = http.tls_server + "/repo";

  config.storage.path = temp_dir.Path();
  auto storage = INvStorage::newStorage(config.storage);
  Json::Value data_json = http.get(http.tls_server + "/director/root.json").getJson();

  data_json["signatures"][0]["keyid"] = "badkeyid";

  Uptane::Root root(Uptane::Root::Policy::kAcceptAll);
  EXPECT_THROW(Uptane::Root(Uptane::RepositoryType::Director, data_json, root), Uptane::UnmetThreshold);
}

TEST(Uptane, VerifyDataBadThreshold) {
  TemporaryDirectory temp_dir;
  HttpFake http(temp_dir.Path());
  Config config;
  config.uptane.director_server = http.tls_server + "/director";
  config.uptane.repo_server = http.tls_server + "/repo";

  config.storage.path = temp_dir.Path();
  auto storage = INvStorage::newStorage(config.storage);
  Json::Value data_json = http.get(http.tls_server + "/director/root.json").getJson();
  data_json["signed"]["roles"]["root"]["threshold"] = -1;
  try {
    Uptane::Root root(Uptane::Root::Policy::kAcceptAll);
    Uptane::Root(Uptane::RepositoryType::Director, data_json, root);
    FAIL();
  } catch (Uptane::IllegalThreshold ex) {
  } catch (Uptane::UnmetThreshold ex) {
  }
}

/*
 * \verify{\tst{153}} Check that aktualizr creates provisioning files if they
 * don't exist already.
 */
TEST(Uptane, Initialize) {
  TemporaryDirectory temp_dir;
  HttpFake http(temp_dir.Path());
  Config conf("tests/config/basic.toml");
  conf.uptane.repo_server = http.tls_server + "/director";

  conf.uptane.repo_server = http.tls_server + "/repo";
  conf.tls.server = http.tls_server;

  conf.provision.primary_ecu_serial = "testecuserial";

  conf.storage.path = temp_dir.Path();
  conf.storage.uptane_metadata_path = "metadata";
  conf.storage.uptane_private_key_path = "private.key";
  conf.storage.uptane_private_key_path = "public.key";

  EXPECT_FALSE(boost::filesystem::exists(conf.storage.path / conf.storage.tls_clientcert_path));
  EXPECT_FALSE(boost::filesystem::exists(conf.storage.path / conf.storage.tls_cacert_path));
  EXPECT_FALSE(boost::filesystem::exists(conf.storage.path / conf.storage.tls_pkey_path));

  auto storage = INvStorage::newStorage(conf.storage);
  std::string pkey;
  std::string cert;
  std::string ca;
  bool result = storage->loadTlsCreds(&ca, &cert, &pkey);
  EXPECT_FALSE(result);
  EXPECT_FALSE(boost::filesystem::exists(conf.storage.path / conf.storage.tls_clientcert_path));
  EXPECT_FALSE(boost::filesystem::exists(conf.storage.path / conf.storage.tls_cacert_path));
  EXPECT_FALSE(boost::filesystem::exists(conf.storage.path / conf.storage.tls_pkey_path));

  KeyManager keys(storage, conf.keymanagerConfig());
  Initializer initializer(conf.provision, storage, http, keys, {});

  result = initializer.isSuccessful();
  EXPECT_TRUE(result);
  EXPECT_TRUE(boost::filesystem::exists(conf.storage.path / conf.storage.tls_clientcert_path));
  EXPECT_TRUE(boost::filesystem::exists(conf.storage.path / conf.storage.tls_cacert_path));
  EXPECT_TRUE(boost::filesystem::exists(conf.storage.path / conf.storage.tls_pkey_path));
  Json::Value ecu_data = Utils::parseJSONFile(temp_dir.Path() / "post.json");
  EXPECT_EQ(ecu_data["ecus"].size(), 1);
  EXPECT_EQ(ecu_data["primary_ecu_serial"].asString(), conf.provision.primary_ecu_serial);
}

/*
 * \verify{\tst{154}} Check that aktualizr does NOT change provisioning files if
 * they DO exist already.
 */
TEST(Uptane, InitializeTwice) {
  TemporaryDirectory temp_dir;
  Config conf("tests/config/basic.toml");
  conf.storage.path = temp_dir.Path();
  conf.provision.primary_ecu_serial = "testecuserial";
  conf.storage.uptane_private_key_path = "private.key";
  conf.storage.uptane_public_key_path = "public.key";

  EXPECT_FALSE(boost::filesystem::exists(conf.storage.path / conf.storage.tls_clientcert_path));
  EXPECT_FALSE(boost::filesystem::exists(conf.storage.path / conf.storage.tls_cacert_path));
  EXPECT_FALSE(boost::filesystem::exists(conf.storage.path / conf.storage.tls_pkey_path));

  auto storage = INvStorage::newStorage(conf.storage);
  std::string pkey1;
  std::string cert1;
  std::string ca1;
  bool result = storage->loadTlsCreds(&ca1, &cert1, &pkey1);
  EXPECT_FALSE(result);
  EXPECT_FALSE(boost::filesystem::exists(conf.storage.path / conf.storage.tls_clientcert_path));
  EXPECT_FALSE(boost::filesystem::exists(conf.storage.path / conf.storage.tls_cacert_path));
  EXPECT_FALSE(boost::filesystem::exists(conf.storage.path / conf.storage.tls_pkey_path));

  HttpFake http(temp_dir.Path());

  {
    KeyManager keys(storage, conf.keymanagerConfig());
    Initializer initializer(conf.provision, storage, http, keys, {});

    result = initializer.isSuccessful();
    EXPECT_TRUE(result);
    EXPECT_TRUE(boost::filesystem::exists(conf.storage.path / conf.storage.tls_clientcert_path));
    EXPECT_TRUE(boost::filesystem::exists(conf.storage.path / conf.storage.tls_cacert_path));
    EXPECT_TRUE(boost::filesystem::exists(conf.storage.path / conf.storage.tls_pkey_path));

    result = storage->loadTlsCreds(&ca1, &cert1, &pkey1);
    EXPECT_TRUE(result);
  }

  {
    KeyManager keys(storage, conf.keymanagerConfig());
    Initializer initializer(conf.provision, storage, http, keys, {});

    result = initializer.isSuccessful();
    EXPECT_TRUE(result);
    EXPECT_TRUE(boost::filesystem::exists(conf.storage.path / conf.storage.tls_clientcert_path));
    EXPECT_TRUE(boost::filesystem::exists(conf.storage.path / conf.storage.tls_cacert_path));
    EXPECT_TRUE(boost::filesystem::exists(conf.storage.path / conf.storage.tls_pkey_path));

    std::string pkey2;
    std::string cert2;
    std::string ca2;
    result = storage->loadTlsCreds(&ca2, &cert2, &pkey2);
    EXPECT_TRUE(result);

    EXPECT_EQ(cert1, cert2);
    EXPECT_EQ(ca1, ca2);
    EXPECT_EQ(pkey1, pkey2);
  }
}

/**
 * \verify{\tst{146}} Check that aktualizr does not generate a pet name when
 * device ID is specified. This is currently provisional and not a finalized
 * requirement at this time.
 */
TEST(Uptane, PetNameProvided) {
  TemporaryDirectory temp_dir;
  std::string test_name = "test-name-123";
  boost::filesystem::path device_path = temp_dir.Path() / "device_id";

  /* Make sure provided device ID is read as expected. */
  Config conf("tests/config/device_id.toml");
  conf.storage.path = temp_dir.Path();
  conf.storage.uptane_private_key_path = "private.key";
  conf.storage.uptane_public_key_path = "public.key";
  conf.provision.primary_ecu_serial = "testecuserial";

  auto storage = INvStorage::newStorage(conf.storage);
  HttpFake http(temp_dir.Path());
  KeyManager keys(storage, conf.keymanagerConfig());
  Initializer initializer(conf.provision, storage, http, keys, {});

  EXPECT_TRUE(initializer.isSuccessful());

  EXPECT_EQ(conf.provision.device_id, test_name);
  EXPECT_TRUE(boost::filesystem::exists(device_path));
  EXPECT_EQ(Utils::readFile(device_path), test_name);

  /* Make sure name is unchanged after re-initializing config. */
  conf.postUpdateValues();
  EXPECT_EQ(conf.provision.device_id, test_name);
  EXPECT_TRUE(boost::filesystem::exists(device_path));
  EXPECT_EQ(Utils::readFile(device_path), test_name);
}

/**
 * \verify{\tst{145}} Check that aktualizr generates a pet name if no device ID
 * is specified.
 */
TEST(Uptane, PetNameCreation) {
  TemporaryDirectory temp_dir;
  boost::filesystem::path device_path = temp_dir.Path() / "device_id";

  // Make sure name is created.
  Config conf("tests/config/basic.toml");
  conf.storage.path = temp_dir.Path();
  conf.storage.uptane_private_key_path = "private.key";
  conf.storage.uptane_public_key_path = "public.key";
  conf.provision.primary_ecu_serial = "testecuserial";
  boost::filesystem::copy_file("tests/test_data/cred.zip", temp_dir.Path() / "cred.zip");
  conf.provision.provision_path = temp_dir.Path() / "cred.zip";

  std::string test_name1, test_name2;
  {
    auto storage = INvStorage::newStorage(conf.storage);
    HttpFake http(temp_dir.Path());

    KeyManager keys(storage, conf.keymanagerConfig());
    Initializer initializer(conf.provision, storage, http, keys, {});

    EXPECT_TRUE(initializer.isSuccessful());

    EXPECT_TRUE(boost::filesystem::exists(device_path));
    test_name1 = Utils::readFile(device_path);
    EXPECT_NE(test_name1, "");
  }

  // Make sure a new name is generated if the config does not specify a name and
  // there is no device_id file.
  TemporaryDirectory temp_dir2;
  {
    conf.storage.path = temp_dir2.Path();
    boost::filesystem::copy_file("tests/test_data/cred.zip", temp_dir2.Path() / "cred.zip");
    conf.provision.device_id = "";

    auto storage = INvStorage::newStorage(conf.storage);
    HttpFake http(temp_dir2.Path());
    KeyManager keys(storage, conf.keymanagerConfig());
    Initializer initializer(conf.provision, storage, http, keys, {});

    EXPECT_TRUE(initializer.isSuccessful());

    EXPECT_TRUE(boost::filesystem::exists(temp_dir2.Path() / "device_id"));
    test_name2 = Utils::readFile(temp_dir2.Path() / "device_id");
    EXPECT_NE(test_name2, test_name1);
  }

  // If the device_id is cleared in the config, but the file is still present,
  // re-initializing the config should still read the device_id from file.
  {
    conf.provision.device_id = "";
    auto storage = INvStorage::newStorage(conf.storage);
    HttpFake http(temp_dir.Path());
    KeyManager keys(storage, conf.keymanagerConfig());
    Initializer initializer(conf.provision, storage, http, keys, {});

    EXPECT_TRUE(initializer.isSuccessful());

    EXPECT_TRUE(boost::filesystem::exists(temp_dir2.Path() / "device_id"));
    EXPECT_EQ(Utils::readFile(temp_dir2.Path() / "device_id"), test_name2);
  }

  // If the device_id file is removed, but the field is still present in the
  // config, re-initializing the config should still read the device_id from
  // config.
  {
    TemporaryDirectory temp_dir3;
    conf.storage.path = temp_dir3.Path();
    boost::filesystem::copy_file("tests/test_data/cred.zip", temp_dir3.Path() / "cred.zip");
    conf.provision.device_id = test_name2;

    auto storage = INvStorage::newStorage(conf.storage);
    HttpFake http(temp_dir3.Path());
    KeyManager keys(storage, conf.keymanagerConfig());
    Initializer initializer(conf.provision, storage, http, keys, {});

    EXPECT_TRUE(initializer.isSuccessful());

    EXPECT_TRUE(boost::filesystem::exists(temp_dir3.Path() / "device_id"));
    EXPECT_EQ(Utils::readFile(temp_dir3.Path() / "device_id"), test_name2);
  }
}

/**
 * \verify{\tst{49}} Check that aktualizr fails on expired metadata
 */
/*TEST(Uptane, Expires) {
  TemporaryDirectory temp_dir;
  HttpFake http(temp_dir.Path());
  Config config;
  config.uptane.director_server = http.tls_server + "/director";
  config.uptane.repo_server = http.tls_server + "/repo";

  config.storage.path = temp_dir.Path();
  config.storage.uptane_metadata_path = "metadata";

  auto storage = INvStorage::newStorage(config.storage);

  Uptane::Root root(Uptane::RepositoryType::Director,
Utils::parseJSONFile("tests/test_data/repo/repo/director/root.json"));

  // Check that we don't fail on good metadata.
  EXPECT_NO_THROW(
      Uptane::Targets(Uptane::RepositoryType::Director,
Utils::parseJSONFile("tests/test_data/repo/repo/director/targets_noupdates.json"), root));

  EXPECT_THROW(
      Uptane::Root(Uptane::RepositoryType::Director,
Utils::parseJSONFile("tests/test_data/bad_metadata/root_expired.json"), root),
      Uptane::ExpiredMetadata);

  EXPECT_THROW(
      Uptane::Targets(Uptane::RepositoryType::Director,
Utils::parseJSONFile("tests/test_data/bad_metadata/targets_expired.json"), root),
      Uptane::ExpiredMetadata);

  EXPECT_THROW(Uptane::TimestampMeta(Uptane::RepositoryType::Director,
                                     Utils::parseJSONFile("tests/test_data/bad_metadata/timestamp_expired.json"), root),
               Uptane::ExpiredMetadata);

  EXPECT_THROW(Uptane::Snapshot(Uptane::RepositoryType::Director,
                                Utils::parseJSONFile("tests/test_data/bad_metadata/snapshot_expired.json"), root),
               Uptane::ExpiredMetadata);
}*/

/**
 * \verify{\tst{52}} Check that aktualizr fails on bad threshold
 */
/*TEST(Uptane, Threshold) {
  TemporaryDirectory temp_dir;
  HttpFake http(temp_dir.Path());
  Config config;
  config.uptane.director_server = http.tls_server + "/director";
  config.uptane.repo_server = http.tls_server + "/repo";

  config.storage.path = temp_dir.Path();
  config.storage.uptane_metadata_path = "metadata";

  auto storage = INvStorage::newStorage(config.storage);

  Uptane::Root root(Uptane::RepositoryType::Director,
Utils::parseJSONFile("tests/test_data/repo/repo/director/root.json"));

  // Check that we don't fail on good metadata.
  EXPECT_NO_THROW(
      Uptane::Targets(Uptane::RepositoryType::Director,
Utils::parseJSONFile("tests/test_data/repo/repo/director/targets_noupdates.json"), root));

  EXPECT_THROW(
      Uptane::Root(Uptane::RepositoryType::Director,
Utils::parseJSONFile("tests/test_data/bad_metadata/root_treshold_-1.json"), root),
      Uptane::IllegalThreshold);

  EXPECT_THROW(Uptane::Root(Uptane::RepositoryType::Director,
                            Utils::parseJSONFile("tests/test_data/bad_metadata/root_treshold_-32768.json"), root),
               Uptane::IllegalThreshold);

  EXPECT_THROW(Uptane::Root(Uptane::RepositoryType::Director,
                            Utils::parseJSONFile("tests/test_data/bad_metadata/root_treshold_-2147483648.json"), root),
               Uptane::IllegalThreshold);

  EXPECT_THROW(
      Uptane::Root(Uptane::RepositoryType::Director,
                   Utils::parseJSONFile("tests/test_data/bad_metadata/root_treshold_-9223372036854775808.json"), root),
      Uptane::IllegalThreshold);

  EXPECT_THROW(
      Uptane::Root(Uptane::RepositoryType::Director,
Utils::parseJSONFile("tests/test_data/bad_metadata/root_treshold_0.9.json"), root),
      Uptane::IllegalThreshold);

  EXPECT_THROW(
      Uptane::Root(Uptane::RepositoryType::Director,
Utils::parseJSONFile("tests/test_data/bad_metadata/root_treshold_0.json"), root),
      Uptane::IllegalThreshold);

  EXPECT_THROW(
      Uptane::Root(Uptane::RepositoryType::Director,
Utils::parseJSONFile("tests/test_data/bad_metadata/root_treshold_-1.json"), root),
      Uptane::IllegalThreshold);

  EXPECT_THROW(
      Uptane::Root(Uptane::RepositoryType::Director,
Utils::parseJSONFile("tests/test_data/bad_metadata/root_treshold_-1.json"), root),
      Uptane::IllegalThreshold);
}*/

TEST(Uptane, InitializeFail) {
  TemporaryDirectory temp_dir;
  HttpFake http(temp_dir.Path());
  Config conf("tests/config/basic.toml");
  conf.uptane.repo_server = http.tls_server + "/director";
  conf.storage.path = temp_dir.Path();
  conf.storage.uptane_private_key_path = "private.key";
  conf.storage.uptane_public_key_path = "public.key";

  conf.uptane.repo_server = http.tls_server + "/repo";
  conf.tls.server = http.tls_server;

  conf.provision.primary_ecu_serial = "testecuserial";

  auto storage = INvStorage::newStorage(conf.storage);

  http.provisioningResponse = ProvisioningResult::kFailure;
  KeyManager keys(storage, conf.keymanagerConfig());
  Initializer initializer(conf.provision, storage, http, keys, {});

  bool result = initializer.isSuccessful();
  http.provisioningResponse = ProvisioningResult::kOK;
  EXPECT_FALSE(result);
}

TEST(Uptane, AssembleManifestGood) {
  TemporaryDirectory temp_dir;
  HttpFake http(temp_dir.Path());
  Config config;
  config.storage.path = temp_dir.Path();
  config.storage.uptane_metadata_path = "metadata";
  config.storage.uptane_private_key_path = "private.key";
  config.storage.uptane_public_key_path = "public.key";
  boost::filesystem::copy_file("tests/test_data/cred.zip", (temp_dir / "cred.zip").string());
  boost::filesystem::copy_file("tests/test_data/firmware.txt", (temp_dir / "firmware.txt").string());
  boost::filesystem::copy_file("tests/test_data/firmware_name.txt", (temp_dir / "firmware_name.txt").string());
  config.provision.provision_path = temp_dir / "cred.zip";
  config.provision.mode = ProvisionMode::kAutomatic;
  config.uptane.director_server = http.tls_server + "/director";
  config.uptane.repo_server = http.tls_server + "/repo";
  config.provision.primary_ecu_serial = "testecuserial";
  config.pacman.type = PackageManager::kNone;

  Uptane::SecondaryConfig ecu_config;
  ecu_config.secondary_type = Uptane::SecondaryType::kVirtual;
  ecu_config.partial_verifying = false;
  ecu_config.full_client_dir = temp_dir.Path();
  ecu_config.ecu_serial = "secondary_ecu_serial";
  ecu_config.ecu_hardware_id = "secondary_hardware";
  ecu_config.ecu_private_key = "sec.priv";
  ecu_config.ecu_public_key = "sec.pub";
  ecu_config.firmware_path = temp_dir / "firmware.txt";
  ecu_config.target_name_path = temp_dir / "firmware_name.txt";
  ecu_config.metadata_path = temp_dir / "secondary_metadata";
  config.uptane.secondary_configs.push_back(ecu_config);

  auto storage = std::make_shared<FSStorage>(config.storage);
  Uptane::DirectorRepository director_repo;
  Uptane::ImagesRepository images_repo;
  Uptane::Manifest uptane_manifest{config, storage};
  Bootloader bootloader{config.bootloader};
  SotaUptaneClient sota_client(config, NULL, director_repo, images_repo, uptane_manifest, storage, http, bootloader);
  EXPECT_TRUE(sota_client.initialize());

  Json::Value manifest = sota_client.AssembleManifest();
  EXPECT_EQ(manifest.size(), 2);
}

TEST(Uptane, AssembleManifestBad) {
  TemporaryDirectory temp_dir;
  HttpFake http(temp_dir.Path());
  Config config;
  config.storage.path = temp_dir.Path();
  config.storage.uptane_metadata_path = "metadata";
  config.storage.uptane_private_key_path = "private.key";
  config.storage.uptane_public_key_path = "public.key";
  boost::filesystem::copy_file("tests/test_data/cred.zip", (temp_dir / "cred.zip").string());
  boost::filesystem::copy_file("tests/test_data/firmware.txt", (temp_dir / "firmware.txt").string());
  boost::filesystem::copy_file("tests/test_data/firmware_name.txt", (temp_dir / "firmware_name.txt").string());
  config.provision.provision_path = temp_dir / "cred.zip";
  config.provision.mode = ProvisionMode::kAutomatic;
  config.uptane.director_server = http.tls_server + "/director";
  config.uptane.repo_server = http.tls_server + "/repo";
  config.provision.primary_ecu_serial = "testecuserial";
  config.pacman.type = PackageManager::kNone;

  Uptane::SecondaryConfig ecu_config;
  ecu_config.secondary_type = Uptane::SecondaryType::kVirtual;
  ecu_config.partial_verifying = false;
  ecu_config.full_client_dir = temp_dir.Path();
  ecu_config.ecu_serial = "secondary_ecu_serial";
  ecu_config.ecu_hardware_id = "secondary_hardware";
  ecu_config.ecu_private_key = "sec.priv";
  ecu_config.ecu_public_key = "sec.pub";
  ecu_config.firmware_path = temp_dir / "firmware.txt";
  ecu_config.target_name_path = temp_dir / "firmware_name.txt";
  ecu_config.metadata_path = temp_dir / "secondary_metadata";
  config.uptane.secondary_configs.push_back(ecu_config);

  std::string private_key, public_key;
  Crypto::generateKeyPair(ecu_config.key_type, &public_key, &private_key);
  Utils::writeFile(ecu_config.full_client_dir / ecu_config.ecu_private_key, private_key);
  public_key = Utils::readFile("tests/test_data/public.key");
  Utils::writeFile(ecu_config.full_client_dir / ecu_config.ecu_public_key, public_key);

  auto storage = std::make_shared<FSStorage>(config.storage);
  Uptane::DirectorRepository director_repo;
  Uptane::ImagesRepository images_repo;
  Uptane::Manifest uptane_manifest{config, storage};
  Bootloader bootloader{config.bootloader};
  SotaUptaneClient sota_client(config, NULL, director_repo, images_repo, uptane_manifest, storage, http, bootloader);
  EXPECT_TRUE(sota_client.initialize());

  Json::Value manifest = sota_client.AssembleManifest();

  EXPECT_EQ(manifest.size(), 1);
  EXPECT_EQ(manifest["testecuserial"]["signed"]["ecu_serial"], config.provision.primary_ecu_serial);
}

TEST(Uptane, PutManifest) {
  TemporaryDirectory temp_dir;
  HttpFake http(temp_dir.Path());
  Config config;
  config.storage.path = temp_dir.Path();
  config.storage.uptane_metadata_path = "metadata";
  config.storage.uptane_private_key_path = "private.key";
  config.storage.uptane_public_key_path = "public.key";
  boost::filesystem::copy_file("tests/test_data/cred.zip", (temp_dir / "cred.zip").string());
  boost::filesystem::copy_file("tests/test_data/firmware.txt", (temp_dir / "firmware.txt").string());
  boost::filesystem::copy_file("tests/test_data/firmware_name.txt", (temp_dir / "firmware_name.txt").string());
  config.provision.provision_path = temp_dir / "cred.zip";
  config.provision.mode = ProvisionMode::kAutomatic;
  config.uptane.director_server = http.tls_server + "/director";
  config.uptane.repo_server = http.tls_server + "/repo";
  config.provision.primary_ecu_serial = "testecuserial";
  config.pacman.type = PackageManager::kNone;

  Uptane::SecondaryConfig ecu_config;
  ecu_config.secondary_type = Uptane::SecondaryType::kVirtual;
  ecu_config.partial_verifying = false;
  ecu_config.full_client_dir = temp_dir.Path();
  ecu_config.ecu_serial = "secondary_ecu_serial";
  ecu_config.ecu_hardware_id = "secondary_hardware";
  ecu_config.ecu_private_key = "sec.priv";
  ecu_config.ecu_public_key = "sec.pub";
  ecu_config.firmware_path = temp_dir / "firmware.txt";
  ecu_config.target_name_path = temp_dir / "firmware_name.txt";
  ecu_config.metadata_path = temp_dir / "secondary_metadata";
  config.uptane.secondary_configs.push_back(ecu_config);

  auto storage = INvStorage::newStorage(config.storage);
  Uptane::DirectorRepository director_repo;
  Uptane::ImagesRepository images_repo;
  Uptane::Manifest uptane_manifest{config, storage};
  Bootloader bootloader{config.bootloader};
  SotaUptaneClient sota_client(config, NULL, director_repo, images_repo, uptane_manifest, storage, http, bootloader);
  EXPECT_TRUE(sota_client.initialize());

  auto signed_manifest = uptane_manifest.signManifest(sota_client.AssembleManifest());
  EXPECT_TRUE(http.put(config.uptane.director_server + "/manifest", signed_manifest).isOk());
  EXPECT_TRUE(boost::filesystem::exists(temp_dir / http.test_manifest));
  Json::Value json = Utils::parseJSONFile((temp_dir / http.test_manifest).string());

  EXPECT_EQ(json["signatures"].size(), 1u);
  EXPECT_EQ(json["signed"]["primary_ecu_serial"].asString(), "testecuserial");
  EXPECT_EQ(json["signed"]["ecu_version_manifests"].size(), 2u);
  EXPECT_EQ(json["signed"]["ecu_version_manifests"]["secondary_ecu_serial"]["signed"]["ecu_serial"],
            "secondary_ecu_serial");
  EXPECT_EQ(json["signed"]["ecu_version_manifests"]["secondary_ecu_serial"]["signed"]["installed_image"]["filepath"],
            "test-package");
}

TEST(Uptane, RunForeverNoUpdates) {
  TemporaryDirectory temp_dir;
  HttpFake http(temp_dir.Path());
  Config conf("tests/config/basic.toml");
  boost::filesystem::copy_file("tests/test_data/secondary_firmware.txt", temp_dir / "secondary_firmware.txt");
  conf.uptane.director_server = http.tls_server + "/director";
  conf.uptane.repo_server = http.tls_server + "/repo";
  conf.provision.primary_ecu_serial = "CA:FE:A6:D2:84:9D";
  conf.provision.primary_ecu_hardware_id = "primary_hw";
  conf.uptane.polling_sec = 1;
  conf.storage.path = temp_dir.Path();
  conf.storage.uptane_metadata_path = "metadata";
  conf.storage.uptane_private_key_path = "private.key";
  conf.storage.uptane_public_key_path = "public.key";
  conf.pacman.sysroot = sysroot;

  conf.tls.server = http.tls_server;
  std::shared_ptr<event::Channel> events_channel{new event::Channel};
  std::shared_ptr<command::Channel> commands_channel{new command::Channel};

  *commands_channel << std::make_shared<command::GetUpdateRequests>();
  *commands_channel << std::make_shared<command::GetUpdateRequests>();
  *commands_channel << std::make_shared<command::GetUpdateRequests>();
  *commands_channel << std::make_shared<command::Shutdown>();

  auto storage = INvStorage::newStorage(conf.storage);
  Bootloader bootloader{conf.bootloader};
  Uptane::DirectorRepository director_repo;
  Uptane::ImagesRepository images_repo;
  Uptane::Manifest uptane_manifest{conf, storage};
  SotaUptaneClient up(conf, events_channel, director_repo, images_repo, uptane_manifest, storage, http, bootloader);
  up.runForever(commands_channel);

  std::shared_ptr<event::BaseEvent> event;

  events_channel->setTimeout(std::chrono::milliseconds(1000));
  EXPECT_TRUE(events_channel->hasValues());
  EXPECT_TRUE(*events_channel >> event);
  EXPECT_EQ(event->variant, "UptaneTargetsUpdated");

  EXPECT_TRUE(events_channel->hasValues());
  EXPECT_TRUE(*events_channel >> event);
  EXPECT_EQ(event->variant, "UptaneTimestampUpdated");

  EXPECT_TRUE(events_channel->hasValues());
  EXPECT_TRUE(*events_channel >> event);
  EXPECT_EQ(event->variant, "UptaneTimestampUpdated");
}

TEST(Uptane, RunForeverHasUpdates) {
  TemporaryDirectory temp_dir;
  HttpFake http(temp_dir.Path());
  Config conf("tests/config/basic.toml");
  boost::filesystem::copy_file("tests/test_data/secondary_firmware.txt", temp_dir / "secondary_firmware.txt");
  conf.uptane.director_server = http.tls_server + "/director";
  conf.uptane.repo_server = http.tls_server + "/repo";
  conf.provision.primary_ecu_serial = "CA:FE:A6:D2:84:9D";
  conf.provision.primary_ecu_hardware_id = "primary_hw";
  conf.uptane.polling_sec = 1;
  conf.storage.path = temp_dir.Path();
  conf.storage.uptane_metadata_path = "metadata";
  conf.storage.uptane_private_key_path = "private.key";
  conf.storage.uptane_public_key_path = "public.key";
  conf.pacman.sysroot = sysroot;

  Uptane::SecondaryConfig ecu_config;
  ecu_config.secondary_type = Uptane::SecondaryType::kVirtual;
  ecu_config.partial_verifying = false;
  ecu_config.full_client_dir = temp_dir.Path();
  ecu_config.ecu_serial = "secondary_ecu_serial";
  ecu_config.ecu_hardware_id = "secondary_hw";
  ecu_config.ecu_private_key = "sec.priv";
  ecu_config.ecu_public_key = "sec.pub";
  ecu_config.firmware_path = temp_dir / "firmware.txt";
  ecu_config.target_name_path = temp_dir / "firmware_name.txt";
  ecu_config.metadata_path = temp_dir / "secondary_metadata";
  conf.uptane.secondary_configs.push_back(ecu_config);

  conf.tls.server = http.tls_server;
  std::shared_ptr<event::Channel> events_channel{new event::Channel};
  std::shared_ptr<command::Channel> commands_channel{new command::Channel};

  *commands_channel << std::make_shared<command::GetUpdateRequests>();
  *commands_channel << std::make_shared<command::Shutdown>();
  auto storage = INvStorage::newStorage(conf.storage);
  Bootloader bootloader{conf.bootloader};
  Uptane::DirectorRepository director_repo;
  Uptane::ImagesRepository images_repo;
  Uptane::Manifest uptane_manifest{conf, storage};
  SotaUptaneClient up(conf, events_channel, director_repo, images_repo, uptane_manifest, storage, http, bootloader);
  up.runForever(commands_channel);

  std::shared_ptr<event::BaseEvent> event;
  EXPECT_TRUE(events_channel->hasValues());
  events_channel->setTimeout(std::chrono::milliseconds(1000));
  EXPECT_TRUE(*events_channel >> event);
  EXPECT_EQ(event->variant, "UptaneTargetsUpdated");
  event::UptaneTargetsUpdated* targets_event = static_cast<event::UptaneTargetsUpdated*>(event.get());
  EXPECT_EQ(targets_event->packages.size(), 2u);
  EXPECT_EQ(targets_event->packages[0].filename(), "primary_firmware.txt");
  EXPECT_EQ(targets_event->packages[1].filename(), "secondary_firmware.txt");
}

std::vector<Uptane::Target> makePackage(const std::string& serial, const std::string& hw_id) {
  std::vector<Uptane::Target> packages_to_install;
  Json::Value ot_json;
  ot_json["custom"]["ecuIdentifiers"][serial]["hardwareId"] = hw_id;
  ot_json["custom"]["targetFormat"] = "OSTREE";
  ot_json["length"] = 0;
  ot_json["hashes"]["sha256"] = serial;
  packages_to_install.push_back(Uptane::Target(serial, ot_json));
  return packages_to_install;
}

TEST(Uptane, RunForeverInstall) {
  Config conf("tests/config/basic.toml");
  TemporaryDirectory temp_dir;
  HttpFake http(temp_dir.Path());
  conf.provision.primary_ecu_serial = "testecuserial";
  conf.provision.primary_ecu_hardware_id = "testecuhwid";
  conf.uptane.director_server = http.tls_server + "/director";
  conf.uptane.repo_server = http.tls_server + "/repo";
  conf.uptane.polling_sec = 1;
  conf.storage.path = temp_dir.Path();
  conf.storage.uptane_private_key_path = "private.key";
  conf.storage.uptane_public_key_path = "public.key";
  conf.pacman.sysroot = sysroot;

  conf.tls.server = http.tls_server;
  std::shared_ptr<event::Channel> events_channel{new event::Channel};
  std::shared_ptr<command::Channel> commands_channel{new command::Channel};

  std::vector<Uptane::Target> packages_to_install = makePackage("testecuserial", "testecuhwid");
  *commands_channel << std::make_shared<command::UptaneInstall>(packages_to_install);
  *commands_channel << std::make_shared<command::Shutdown>();
  auto storage = INvStorage::newStorage(conf.storage);
  Uptane::DirectorRepository director_repo;
  Uptane::ImagesRepository images_repo;
  Uptane::Manifest uptane_manifest{conf, storage};
  Bootloader bootloader{conf.bootloader};
  SotaUptaneClient up(conf, events_channel, director_repo, images_repo, uptane_manifest, storage, http, bootloader);
  up.runForever(commands_channel);

  EXPECT_TRUE(boost::filesystem::exists(temp_dir.Path() / http.test_manifest));

  Json::Value json;
  Json::Reader reader;
  std::ifstream ks((temp_dir.Path() / http.test_manifest).c_str());
  std::string mnfst_str((std::istreambuf_iterator<char>(ks)), std::istreambuf_iterator<char>());

  reader.parse(mnfst_str, json);
  EXPECT_EQ(json["signatures"].size(), 1u);
  EXPECT_EQ(json["signed"]["primary_ecu_serial"].asString(), "testecuserial");
  EXPECT_EQ(json["signed"]["ecu_version_manifests"].size(), 1u);
}

TEST(Uptane, UptaneSecondaryAdd) {
  TemporaryDirectory temp_dir;
  HttpFake http(temp_dir.Path());
  Config config;
  config.uptane.repo_server = http.tls_server + "/director";
  boost::filesystem::copy_file("tests/test_data/cred.zip", temp_dir / "cred.zip");
  config.provision.provision_path = temp_dir / "cred.zip";
  config.provision.mode = ProvisionMode::kAutomatic;
  config.uptane.repo_server = http.tls_server + "/repo";
  config.tls.server = http.tls_server;
  config.provision.primary_ecu_serial = "testecuserial";
  config.storage.path = temp_dir.Path();
  config.storage.uptane_private_key_path = "private.key";
  config.storage.uptane_public_key_path = "public.key";
  config.pacman.type = PackageManager::kNone;

  Uptane::SecondaryConfig ecu_config;
  ecu_config.secondary_type = Uptane::SecondaryType::kVirtual;
  ecu_config.partial_verifying = false;
  ecu_config.full_client_dir = temp_dir.Path();
  ecu_config.ecu_serial = "secondary_ecu_serial";
  ecu_config.ecu_hardware_id = "secondary_hardware";
  ecu_config.ecu_private_key = "sec.priv";
  ecu_config.ecu_public_key = "sec.pub";
  ecu_config.firmware_path = temp_dir / "firmware.txt";
  ecu_config.target_name_path = temp_dir / "firmware_name.txt";
  ecu_config.metadata_path = temp_dir / "secondary_metadata";
  config.uptane.secondary_configs.push_back(ecu_config);

  auto storage = INvStorage::newStorage(config.storage);
  Uptane::DirectorRepository director_repo;
  Uptane::ImagesRepository images_repo;
  Uptane::Manifest uptane_manifest{config, storage};
  std::shared_ptr<event::Channel> events_channel{new event::Channel};
  Bootloader bootloader{config.bootloader};
  SotaUptaneClient sota_client(config, events_channel, director_repo, images_repo, uptane_manifest, storage, http,
                               bootloader);
  EXPECT_TRUE(sota_client.initialize());
  Json::Value ecu_data = Utils::parseJSONFile(temp_dir / "post.json");
  EXPECT_EQ(ecu_data["ecus"].size(), 2);
  EXPECT_EQ(ecu_data["primary_ecu_serial"].asString(), config.provision.primary_ecu_serial);
  EXPECT_EQ(ecu_data["ecus"][1]["ecu_serial"].asString(), "secondary_ecu_serial");
  EXPECT_EQ(ecu_data["ecus"][1]["hardware_identifier"].asString(), "secondary_hardware");
  EXPECT_EQ(ecu_data["ecus"][1]["clientKey"]["keytype"].asString(), "RSA");
  EXPECT_TRUE(ecu_data["ecus"][1]["clientKey"]["keyval"]["public"].asString().size() > 0);
}

/**
 * \verify{\tst{149}} Check that basic device info sent by aktualizr on provisioning are on server
 * Also test that installation works as expected with the fake package manager.
 */
TEST(Uptane, ProvisionOnServer) {
  TemporaryDirectory temp_dir;
  Config config("tests/config/basic.toml");
  std::string server = "tst149";
  config.provision.server = server;
  config.tls.server = server;
  config.uptane.director_server = server + "/director";
  config.uptane.repo_server = server + "/repo";
  config.provision.device_id = "tst149_device_id";
  config.provision.primary_ecu_hardware_id = "tst149_hardware_identifier";
  config.provision.primary_ecu_serial = "tst149_ecu_serial";
  config.uptane.polling_sec = 1;
  config.storage.path = temp_dir.Path();

  std::shared_ptr<event::Channel> events_channel{new event::Channel};
  std::shared_ptr<command::Channel> commands_channel{new command::Channel};
  auto storage = INvStorage::newStorage(config.storage);
  HttpFake http(temp_dir.Path());
  std::vector<Uptane::Target> packages_to_install =
      makePackage(config.provision.primary_ecu_serial, config.provision.primary_ecu_hardware_id);
  *commands_channel << std::make_shared<command::GetUpdateRequests>();
  *commands_channel << std::make_shared<command::UptaneInstall>(packages_to_install);
  *commands_channel << std::make_shared<command::Shutdown>();
  Uptane::DirectorRepository director_repo;
  Uptane::ImagesRepository images_repo;
  Uptane::Manifest uptane_manifest{config, storage};
  Bootloader bootloader{config.bootloader};
  SotaUptaneClient up(config, events_channel, director_repo, images_repo, uptane_manifest, storage, http, bootloader);
  up.runForever(commands_channel);
}

TEST(Uptane, CheckOldProvision) {
  TemporaryDirectory temp_dir;
  HttpFake http(temp_dir.Path(), true);
  int result = system((std::string("cp -rf tests/test_data/oldprovdir/* ") + temp_dir.PathString()).c_str());
  (void)result;
  Config config;
  config.tls.server = http.tls_server;
  config.uptane.director_server = http.tls_server + "/director";
  config.uptane.repo_server = http.tls_server + "/repo";
  config.storage.path = temp_dir.Path();

  auto storage = INvStorage::newStorage(config.storage);
  EXPECT_FALSE(storage->loadEcuRegistered());

  KeyManager keys(storage, config.keymanagerConfig());
  Initializer initializer(config.provision, storage, http, keys, {});

  EXPECT_TRUE(initializer.isSuccessful());
  EXPECT_TRUE(storage->loadEcuRegistered());
}

TEST(Uptane, fs_to_sql_full) {
  TemporaryDirectory temp_dir;
  int result = system((std::string("cp -rf tests/test_data/prov/* ") + temp_dir.PathString()).c_str());
  (void)result;
  StorageConfig config;
  config.type = StorageType::kSqlite;
  config.uptane_metadata_path = "metadata";
  config.path = temp_dir.Path();
  config.sqldb_path = temp_dir.Path() / "database.db";

  config.uptane_private_key_path = "ecukey.der";
  config.tls_cacert_path = "root.crt";

  FSStorage fs_storage(config);

  std::string public_key;
  std::string private_key;
  fs_storage.loadPrimaryKeys(&public_key, &private_key);

  std::string ca;
  std::string cert;
  std::string pkey;
  fs_storage.loadTlsCreds(&ca, &cert, &pkey);

  std::string device_id;
  fs_storage.loadDeviceId(&device_id);

  EcuSerials serials;
  fs_storage.loadEcuSerials(&serials);

  bool ecu_registered = fs_storage.loadEcuRegistered() ? true : false;

  std::vector<Uptane::Target> installed_versions;
  fs_storage.loadInstalledVersions(&installed_versions);

  std::string director_root;
  std::string director_targets;
  std::string images_root;
  std::string images_targets;
  std::string images_timestamp;
  std::string images_snapshot;

  EXPECT_TRUE(fs_storage.loadRole(&director_root, Uptane::RepositoryType::Director, Uptane::Role::Root()));
  EXPECT_TRUE(fs_storage.loadRole(&director_targets, Uptane::RepositoryType::Director, Uptane::Role::Targets()));
  EXPECT_TRUE(fs_storage.loadRole(&images_root, Uptane::RepositoryType::Images, Uptane::Role::Root()));
  EXPECT_TRUE(fs_storage.loadRole(&images_targets, Uptane::RepositoryType::Images, Uptane::Role::Targets()));
  EXPECT_TRUE(fs_storage.loadRole(&images_timestamp, Uptane::RepositoryType::Images, Uptane::Role::Timestamp()));
  EXPECT_TRUE(fs_storage.loadRole(&images_snapshot, Uptane::RepositoryType::Images, Uptane::Role::Snapshot()));

  EXPECT_TRUE(boost::filesystem::exists(Utils::absolutePath(config.path, config.uptane_public_key_path)));
  EXPECT_TRUE(boost::filesystem::exists(Utils::absolutePath(config.path, config.uptane_private_key_path)));
  EXPECT_TRUE(boost::filesystem::exists(Utils::absolutePath(config.path, config.tls_cacert_path)));
  EXPECT_TRUE(boost::filesystem::exists(Utils::absolutePath(config.path, config.tls_clientcert_path)));
  EXPECT_TRUE(boost::filesystem::exists(Utils::absolutePath(config.path, config.tls_pkey_path)));

  boost::filesystem::path image_path = Utils::absolutePath(config.path, config.uptane_metadata_path) / "repo";
  boost::filesystem::path director_path = Utils::absolutePath(config.path, config.uptane_metadata_path) / "director";
  EXPECT_TRUE(boost::filesystem::exists(director_path / "1.root.json"));
  EXPECT_TRUE(boost::filesystem::exists(director_path / "0.targets.json"));
  EXPECT_TRUE(boost::filesystem::exists(image_path / "1.root.json"));
  EXPECT_TRUE(boost::filesystem::exists(image_path / "21.targets.json"));
  EXPECT_TRUE(boost::filesystem::exists(image_path / "21.timestamp.json"));
  EXPECT_TRUE(boost::filesystem::exists(image_path / "21.snapshot.json"));
  EXPECT_TRUE(boost::filesystem::exists(Utils::absolutePath(config.path, "device_id")));
  EXPECT_TRUE(boost::filesystem::exists(Utils::absolutePath(config.path, "is_registered")));
  EXPECT_TRUE(boost::filesystem::exists(Utils::absolutePath(config.path, "primary_ecu_serial")));
  EXPECT_TRUE(boost::filesystem::exists(Utils::absolutePath(config.path, "primary_ecu_hardware_id")));
  EXPECT_TRUE(boost::filesystem::exists(Utils::absolutePath(config.path, "secondaries_list")));
  auto sql_storage = INvStorage::newStorage(config, temp_dir.Path());

  EXPECT_FALSE(boost::filesystem::exists(Utils::absolutePath(config.path, config.uptane_public_key_path)));
  EXPECT_FALSE(boost::filesystem::exists(Utils::absolutePath(config.path, config.uptane_private_key_path)));
  EXPECT_FALSE(boost::filesystem::exists(Utils::absolutePath(config.path, config.tls_cacert_path)));
  EXPECT_FALSE(boost::filesystem::exists(Utils::absolutePath(config.path, config.tls_clientcert_path)));
  EXPECT_FALSE(boost::filesystem::exists(Utils::absolutePath(config.path, config.tls_pkey_path)));

  EXPECT_FALSE(boost::filesystem::exists(director_path / "root.json"));
  EXPECT_FALSE(boost::filesystem::exists(director_path / "targets.json"));
  EXPECT_FALSE(boost::filesystem::exists(director_path / "root.json"));
  EXPECT_FALSE(boost::filesystem::exists(director_path / "targets.json"));
  EXPECT_FALSE(boost::filesystem::exists(image_path / "timestamp.json"));
  EXPECT_FALSE(boost::filesystem::exists(image_path / "snapshot.json"));
  EXPECT_FALSE(boost::filesystem::exists(Utils::absolutePath(config.path, "device_id")));
  EXPECT_FALSE(boost::filesystem::exists(Utils::absolutePath(config.path, "is_registered")));
  EXPECT_FALSE(boost::filesystem::exists(Utils::absolutePath(config.path, "primary_ecu_serial")));
  EXPECT_FALSE(boost::filesystem::exists(Utils::absolutePath(config.path, "primary_ecu_hardware_id")));
  EXPECT_FALSE(boost::filesystem::exists(Utils::absolutePath(config.path, "secondaries_list")));

  std::string sql_public_key;
  std::string sql_private_key;
  sql_storage->loadPrimaryKeys(&sql_public_key, &sql_private_key);

  std::string sql_ca;
  std::string sql_cert;
  std::string sql_pkey;
  sql_storage->loadTlsCreds(&sql_ca, &sql_cert, &sql_pkey);

  std::string sql_device_id;
  sql_storage->loadDeviceId(&sql_device_id);

  EcuSerials sql_serials;
  sql_storage->loadEcuSerials(&sql_serials);

  bool sql_ecu_registered = sql_storage->loadEcuRegistered() ? true : false;

  std::vector<Uptane::Target> sql_installed_versions;
  sql_storage->loadInstalledVersions(&sql_installed_versions);

  std::string sql_director_root;
  std::string sql_director_targets;
  std::string sql_images_root;
  std::string sql_images_targets;
  std::string sql_images_timestamp;
  std::string sql_images_snapshot;

  sql_storage->loadRole(&sql_director_root, Uptane::RepositoryType::Director, Uptane::Role::Root());
  sql_storage->loadRole(&sql_director_targets, Uptane::RepositoryType::Director, Uptane::Role::Targets());
  sql_storage->loadRole(&sql_images_root, Uptane::RepositoryType::Images, Uptane::Role::Root());
  sql_storage->loadRole(&sql_images_targets, Uptane::RepositoryType::Images, Uptane::Role::Targets());
  sql_storage->loadRole(&sql_images_timestamp, Uptane::RepositoryType::Images, Uptane::Role::Timestamp());
  sql_storage->loadRole(&sql_images_snapshot, Uptane::RepositoryType::Images, Uptane::Role::Snapshot());

  EXPECT_EQ(sql_public_key, public_key);
  EXPECT_EQ(sql_private_key, private_key);
  EXPECT_EQ(sql_ca, ca);
  EXPECT_EQ(sql_cert, cert);
  EXPECT_EQ(sql_pkey, pkey);
  EXPECT_EQ(sql_device_id, device_id);
  EXPECT_EQ(sql_serials, serials);
  EXPECT_EQ(sql_ecu_registered, ecu_registered);
  EXPECT_EQ(sql_installed_versions, installed_versions);

  EXPECT_EQ(sql_director_root, director_root);
  EXPECT_EQ(sql_director_targets, director_targets);
  EXPECT_EQ(sql_images_root, images_root);
  EXPECT_EQ(sql_images_targets, images_targets);
  EXPECT_EQ(sql_images_timestamp, images_timestamp);
  EXPECT_EQ(sql_images_snapshot, images_snapshot);
}

TEST(Uptane, fs_to_sql_partial) {
  TemporaryDirectory temp_dir;
  boost::filesystem::copy_file("tests/test_data/prov/ecukey.der", temp_dir.Path() / "ecukey.der");
  boost::filesystem::copy_file("tests/test_data/prov/ecukey.pub", temp_dir.Path() / "ecukey.pub");

  StorageConfig config;
  config.type = StorageType::kSqlite;
  config.uptane_metadata_path = "metadata";
  config.path = temp_dir.Path();
  config.sqldb_path = temp_dir.Path() / "database.db";

  config.uptane_private_key_path = "ecukey.der";
  config.tls_cacert_path = "root.crt";

  FSStorage fs_storage(config);

  std::string public_key;
  std::string private_key;
  fs_storage.loadPrimaryKeys(&public_key, &private_key);

  std::string ca;
  std::string cert;
  std::string pkey;
  fs_storage.loadTlsCreds(&ca, &cert, &pkey);

  std::string device_id;
  fs_storage.loadDeviceId(&device_id);

  EcuSerials serials;
  fs_storage.loadEcuSerials(&serials);

  bool ecu_registered = fs_storage.loadEcuRegistered() ? true : false;

  std::vector<Uptane::Target> installed_versions;
  fs_storage.loadInstalledVersions(&installed_versions);

  std::string director_root;
  std::string director_targets;
  std::string images_root;
  std::string images_targets;
  std::string images_timestamp;
  std::string images_snapshot;

  fs_storage.loadRole(&director_root, Uptane::RepositoryType::Director, Uptane::Role::Root());
  fs_storage.loadRole(&director_targets, Uptane::RepositoryType::Director, Uptane::Role::Targets());
  fs_storage.loadRole(&images_root, Uptane::RepositoryType::Images, Uptane::Role::Root());
  fs_storage.loadRole(&images_targets, Uptane::RepositoryType::Images, Uptane::Role::Targets());
  fs_storage.loadRole(&images_timestamp, Uptane::RepositoryType::Images, Uptane::Role::Timestamp());
  fs_storage.loadRole(&images_snapshot, Uptane::RepositoryType::Images, Uptane::Role::Snapshot());

  EXPECT_TRUE(boost::filesystem::exists(Utils::absolutePath(config.path, config.uptane_public_key_path)));
  EXPECT_TRUE(boost::filesystem::exists(Utils::absolutePath(config.path, config.uptane_private_key_path)));

  auto sql_storage = INvStorage::newStorage(config, temp_dir.Path());

  EXPECT_FALSE(boost::filesystem::exists(Utils::absolutePath(config.path, config.uptane_public_key_path)));
  EXPECT_FALSE(boost::filesystem::exists(Utils::absolutePath(config.path, config.uptane_private_key_path)));

  std::string sql_public_key;
  std::string sql_private_key;
  sql_storage->loadPrimaryKeys(&sql_public_key, &sql_private_key);

  std::string sql_ca;
  std::string sql_cert;
  std::string sql_pkey;
  sql_storage->loadTlsCreds(&sql_ca, &sql_cert, &sql_pkey);

  std::string sql_device_id;
  sql_storage->loadDeviceId(&sql_device_id);

  EcuSerials sql_serials;
  sql_storage->loadEcuSerials(&sql_serials);

  bool sql_ecu_registered = sql_storage->loadEcuRegistered() ? true : false;

  std::vector<Uptane::Target> sql_installed_versions;
  sql_storage->loadInstalledVersions(&sql_installed_versions);

  std::string sql_director_root;
  std::string sql_director_targets;
  std::string sql_images_root;
  std::string sql_images_targets;
  std::string sql_images_timestamp;
  std::string sql_images_snapshot;

  sql_storage->loadRole(&sql_director_root, Uptane::RepositoryType::Director, Uptane::Role::Root());
  sql_storage->loadRole(&sql_director_targets, Uptane::RepositoryType::Director, Uptane::Role::Targets());
  sql_storage->loadRole(&sql_images_root, Uptane::RepositoryType::Images, Uptane::Role::Root());
  sql_storage->loadRole(&sql_images_targets, Uptane::RepositoryType::Images, Uptane::Role::Targets());
  sql_storage->loadRole(&sql_images_timestamp, Uptane::RepositoryType::Images, Uptane::Role::Timestamp());
  sql_storage->loadRole(&sql_images_snapshot, Uptane::RepositoryType::Images, Uptane::Role::Snapshot());

  EXPECT_EQ(sql_public_key, public_key);
  EXPECT_EQ(sql_private_key, private_key);
  EXPECT_EQ(sql_ca, ca);
  EXPECT_EQ(sql_cert, cert);
  EXPECT_EQ(sql_pkey, pkey);
  EXPECT_EQ(sql_device_id, device_id);
  EXPECT_EQ(sql_serials, serials);
  EXPECT_EQ(sql_ecu_registered, ecu_registered);
  EXPECT_EQ(sql_installed_versions, installed_versions);

  EXPECT_EQ(sql_director_root, director_root);
  EXPECT_EQ(sql_director_targets, director_targets);
  EXPECT_EQ(sql_images_root, images_root);
  EXPECT_EQ(sql_images_targets, images_targets);
  EXPECT_EQ(sql_images_timestamp, images_timestamp);
  EXPECT_EQ(sql_images_snapshot, images_snapshot);
}

TEST(Uptane, SaveVersion) {
  TemporaryDirectory temp_dir;
  Config config;
  config.storage.path = temp_dir.Path();
  config.storage.tls_cacert_path = "ca.pem";
  config.storage.tls_clientcert_path = "client.pem";
  config.storage.tls_pkey_path = "pkey.pem";
  config.provision.device_id = "device_id";
  config.postUpdateValues();
  auto storage = INvStorage::newStorage(config.storage);
  HttpFake http(temp_dir.Path());

  Json::Value target_json;
  target_json["hashes"]["sha256"] = "a0fb2e119cf812f1aa9e993d01f5f07cb41679096cb4492f1265bff5ac901d0d";
  target_json["length"] = 123;

  Uptane::Target t("target_name", target_json);
  storage->saveInstalledVersion(t);
  Json::Value result = Utils::parseJSONFile((config.storage.path / "installed_versions").string());

  EXPECT_EQ(result["target_name"]["hashes"]["sha256"].asString(),
            "a0fb2e119cf812f1aa9e993d01f5f07cb41679096cb4492f1265bff5ac901d0d");
  EXPECT_EQ(result["target_name"]["length"].asInt(), 123);
}

TEST(Uptane, LoadVersion) {
  TemporaryDirectory temp_dir;
  Config config;
  config.storage.path = temp_dir.Path();
  config.storage.tls_cacert_path = "ca.pem";
  config.storage.tls_clientcert_path = "client.pem";
  config.storage.tls_pkey_path = "pkey.pem";
  config.provision.device_id = "device_id";
  config.postUpdateValues();
  auto storage = INvStorage::newStorage(config.storage);
  HttpFake http(temp_dir.Path());

  Json::Value target_json;
  target_json["hashes"]["sha256"] = "a0fb2e119cf812f1aa9e993d01f5f07cb41679096cb4492f1265bff5ac901d0d";
  target_json["length"] = 0;

  Uptane::Target t("target_name", target_json);
  storage->saveInstalledVersion(t);

  std::vector<Uptane::Target> versions;
  storage->loadInstalledVersions(&versions);
  EXPECT_EQ(t, versions[0]);
}

/*TEST(Uptane, krejectallTest) {
  TemporaryDirectory temp_dir;
  boost::filesystem::copy_file("tests/test_data/kRejectAll.db", temp_dir / "db.sqlite");
  HttpFake http(temp_dir.Path());
  Config config;
  config.uptane.director_server = http.tls_server + "/director";
  config.uptane.repo_server = http.tls_server + "/repo";
  config.storage.type = StorageType::kSqlite;
  config.storage.sqldb_path = temp_dir / "db.sqlite";

  config.provision.device_id = "device_id";
  config.postUpdateValues();
  auto storage = INvStorage::newStorage(config.storage);
  Uptane::Repository uptane(config, storage);
  Uptane::Fetcher fetcher(config, storage, http);
  EXPECT_TRUE(fetcher.fetchMeta());
  EXPECT_TRUE(uptane.feedCheckMeta());
} OYTIS*/

/*TEST(Uptane, VerifyMetaTest) {
  TemporaryDirectory temp_dir;
  boost::filesystem::copy_file("tests/test_data/kRejectAll.db", temp_dir / "db.sqlite");
  HttpFake http(temp_dir.Path());
  Config config;
  config.uptane.director_server = http.tls_server + "/director";
  config.uptane.repo_server = http.tls_server + "/repo";
  config.storage.type = StorageType::kSqlite;
  config.storage.sqldb_path = temp_dir / "db.sqlite";

  config.provision.device_id = "device_id";
  config.postUpdateValues();
  auto storage = INvStorage::newStorage(config.storage);
  Uptane::Repository uptane(config, storage);

  Json::Value targets_file = Utils::parseJSONFile("tests/test_data/targets_hasupdates.json");
  Uptane::Targets director_targets_good(targets_file);
  Uptane::Targets image_targets_good(targets_file);

  EXPECT_TRUE(uptane.verifyMetaTargets(director_targets_good, image_targets_good));

  Json::Value big_length = targets_file;
  big_length["signed"]["targets"]["secondary_firmware.txt"]["length"] = 16;

  EXPECT_FALSE(uptane.verifyMetaTargets(Uptane::Targets(big_length), image_targets_good));
  EXPECT_TRUE(uptane.verifyMetaTargets(director_targets_good, Uptane::Targets(big_length)));

  Json::Value no_target = targets_file;
  no_target["signed"]["targets"].removeMember("secondary_firmware.txt");
  EXPECT_FALSE(uptane.verifyMetaTargets(director_targets_good, Uptane::Targets(no_target)));
  EXPECT_TRUE(uptane.verifyMetaTargets(Uptane::Targets(no_target), image_targets_good));

  Json::Value wrong_name = targets_file;
  wrong_name["signed"]["targets"].removeMember("secondary_firmware.txt");
  wrong_name["signed"]["targets"]["secondary_firmware_wrong"] =
      targets_file["signed"]["targets"]["secondary_firmware.txt"];
  EXPECT_FALSE(uptane.verifyMetaTargets(director_targets_good, Uptane::Targets(wrong_name)));
  EXPECT_FALSE(uptane.verifyMetaTargets(Uptane::Targets(wrong_name), image_targets_good));

  Json::Value wrong_hash = targets_file;
  wrong_hash["signed"]["targets"]["secondary_firmware.txt"]["hashes"]["sha256"] = "wrong_hash";
  EXPECT_FALSE(uptane.verifyMetaTargets(director_targets_good, Uptane::Targets(wrong_hash)));
  EXPECT_FALSE(uptane.verifyMetaTargets(Uptane::Targets(wrong_hash), image_targets_good));

  Json::Value more_hashes = targets_file;
  more_hashes["signed"]["targets"]["secondary_firmware_wrong"]["hashes"]["sha1024"] = "new_hash";
  EXPECT_TRUE(uptane.verifyMetaTargets(director_targets_good, Uptane::Targets(more_hashes)));
  EXPECT_FALSE(uptane.verifyMetaTargets(Uptane::Targets(more_hashes), image_targets_good));
} OYTIS*/

#ifdef BUILD_P11
TEST(Uptane, Pkcs11Provision) {
  Config config;
  TemporaryDirectory temp_dir;
  boost::filesystem::copy_file("tests/test_data/implicit/ca.pem", temp_dir / "ca.pem");
  config.tls.cert_source = CryptoSource::kPkcs11;
  config.tls.pkey_source = CryptoSource::kPkcs11;
  config.p11.module = TEST_PKCS11_MODULE_PATH;
  config.p11.pass = "1234";
  config.p11.tls_clientcert_id = "01";
  config.p11.tls_pkey_id = "02";

  config.storage.path = temp_dir.Path();
  config.storage.tls_cacert_path = "ca.pem";
  config.postUpdateValues();

  auto storage = INvStorage::newStorage(config.storage);
  HttpFake http(temp_dir.Path());
  KeyManager keys(storage, config.keymanagerConfig());
  Initializer initializer(config.provision, storage, http, keys, {});

  EXPECT_TRUE(initializer.isSuccessful());
}
#endif

#ifndef __NO_MAIN__
int main(int argc, char** argv) {
  ::testing::InitGoogleTest(&argc, argv);

  logger_init();
  logger_set_threshold(boost::log::trivial::trace);

  if (argc != 2) {
    std::cerr << "Error: " << argv[0] << " requires the path to an OSTree sysroot as an input argument.\n";
    return EXIT_FAILURE;
  }
  sysroot = argv[1];
  return RUN_ALL_TESTS();
}
#endif
