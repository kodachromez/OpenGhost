#pragma once
#include "videoinfo.h"

// Test-only host. No fallback to the production network or profile.
class FixtureVideoInfo final : public VideoInfoService
{
  public:
    QHash<QString, VideoInfo> values;
    QStringList asked;
    QList<Done> pending;
    bool hold = false;
    void lookup(const QString &id, Done done) override
    {
        asked << id;
        if (hold)
            pending << std::move(done);
        else
            done(values.value(id));
    }
};
