#include "DTLiveTiming.h"
#include <cassert>
int main() {
 const auto now=QDateTime::fromString("2026-11-24T22:00:00Z",Qt::ISODate);
 const auto future=now.addDays(1), past=now.addSecs(-3600);
 assert(liveAdvanceEnd(future,now,true)==now);
 assert(liveAdvanceEnd(past,now,true)==past);
 assert(liveAdvanceEnd(now,now,true)==now);
 assert(liveAdvanceEnd(future,now,false)==future);
 assert(liveAdvanceEnd(past,now,false)==past);
}
