#ifndef IMAGES_REPOSITORY_H_
#define IMAGES_REPOSITORY_H_

#include "uptanerepository.h"

namespace Uptane {

class ImagesRepository : public RepositoryCommon {
 public:
  ImagesRepository() : RepositoryCommon(RepositoryType::Images) {}

  void resetMeta();

  bool verifyTargets(const std::string& targets);
  bool targetsExpired() { return targets.isExpired(TimeStamp::Now()); }
  std::unique_ptr<Uptane::Target> getTarget(const Uptane::Target& director_target);

  bool verifyTimestamp(const std::string& timestamp);
  bool timestampExpired() { return timestamp.isExpired(TimeStamp::Now()); }

  bool verifySnapshot(const std::string& snapshot);
  bool snapshotExpired() { return snapshot.isExpired(TimeStamp::Now()); }

  Exception getLastException() const { return last_exception; }

 private:
  Uptane::Targets targets;
  Uptane::TimestampMeta timestamp;
  Uptane::Snapshot snapshot;

  Exception last_exception{"", ""};
};

}  // namespace Uptane

#endif  // IMAGES_REPOSITORY_H
