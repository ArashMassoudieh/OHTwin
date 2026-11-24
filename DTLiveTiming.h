#pragma once
#include <QDateTime>

// Forecasts may extend into the future; persisted live Advance state may not.
// Replays and accelerated runs deliberately retain their requested end time.
inline QDateTime liveAdvanceEnd(const QDateTime &requested,
                                const QDateTime &now, bool live)
{
    return live && requested > now ? now : requested;
}
