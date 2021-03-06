#include "campaign/campaign.h"

namespace campaign {

Campaign Campaign::fromJson(const Json::Value &json) {
  try {
    if (!json.isObject()) {
      throw CampaignParseError();
    }

    std::string id = json["id"].asString();
    if (id.empty()) {
      throw CampaignParseError();
    }

    std::string name = json["name"].asString();
    if (name.empty()) {
      throw CampaignParseError();
    }

    bool autoAccept = json.get("autoAccept", false).asBool();

    std::string description;
    int estInstallationDuration = 0;
    int estPreparationDuration = 0;
    for (const auto &o : json["metadata"]) {
      if (!o.isObject()) {
        continue;
      }

      if (o["type"] == "DESCRIPTION") {
        if (!description.empty()) {
          throw CampaignParseError();
        }
        description = o["value"].asString();
      } else if (o["type"] == "ESTIMATED_INSTALLATION_DURATION") {
        if (estInstallationDuration != 0) {
          throw CampaignParseError();
        }
        estInstallationDuration = std::stoi(o["value"].asString());
      } else if (o["type"] == "ESTIMATED_PREPARATION_DURATION") {
        if (estPreparationDuration != 0) {
          throw CampaignParseError();
        }
        estPreparationDuration = std::stoi(o["value"].asString());
      }
    }

    return {id, name, autoAccept, description, estInstallationDuration, estPreparationDuration};
  } catch (const std::runtime_error &exc) {
    LOG_ERROR << exc.what();
    throw CampaignParseError();
  } catch (...) {
    throw CampaignParseError();
  }
}

std::vector<Campaign> campaignsFromJson(const Json::Value &json) {
  std::vector<Campaign> campaigns;

  Json::Value campaigns_array;
  try {
    if (!json.isObject()) {
      throw CampaignParseError();
    }
    campaigns_array = json["campaigns"];
    if (!campaigns_array.isArray()) {
      throw CampaignParseError();
    }
  } catch (...) {
    LOG_ERROR << "Invalid campaigns object: " << json;
    return {};
  }

  for (const auto &c : campaigns_array) {
    try {
      campaigns.push_back(Campaign::fromJson(c));
    } catch (const CampaignParseError &exc) {
      LOG_ERROR << "Error parsing " << c << ": " << exc.what();
    }
  }
  return campaigns;
}

std::vector<Campaign> fetchAvailableCampaigns(HttpInterface &http_client, const std::string &tls_server) {
  HttpResponse response = http_client.get(tls_server + "/campaigner/campaigns", kMaxCampaignsMetaSize);
  if (!response.isOk()) {
    LOG_ERROR << "Failed to fetch list of available campaigns";
    return {};
  }

  auto json = response.getJson();

  LOG_TRACE << "Campaign: " << json;

  return campaignsFromJson(json);
}
}  // namespace campaign
