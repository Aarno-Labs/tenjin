// Justification — why an answer came out the way it did.
//
// No consumer ever branches on this. Every shipped rule is sound, so there
// is no trust gradient to express and no `tier` field anywhere in the
// interface; a field-path resolution and a bare-parameter one are equally
// reliable and differ only in how much work it took to establish them,
// which is the reader's business and not the consumer's.
//
// What it is for is making a *wrong* answer diagnosable. The useful thing
// to print is not "declined" but "agreed at 3 of 4 sites; disagreed at
// hash.c:52".

#pragma once

#include "llvm/ADT/SmallVector.h"

#include <string>

namespace xj::analysis
{

  struct Justification
  {
    // One line, e.g. "same symbol at 4/4 sites".
    std::string Summary;
    // Per-site detail and the reasons behind any disagreement, e.g.
    // "hash.c:52: t->storage cleared by the call at hash.c:47".
    llvm::SmallVector<std::string, 4> Notes;

    unsigned SitesTotal = 0;
    unsigned SitesAgreed = 0;
    unsigned SitesUnreachable = 0;
  };

} // namespace xj::analysis
