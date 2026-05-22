#ifndef FUZZY_H
#define FUZZY_H

#include "utils.h"

//
// Constants
//

#define FUZZY_MAX_RANGES           64
#define FUZZY_MAX_QUERY_CHARS      64
#define FUZZY_MAX_CANDIDATE_CHARS  512

//
// Types
//

// Byte-offset interval into the candidate string.  [start, end)
typedef struct {
    i32 start;
    i32 end;
} FuzzyRange;

// Returned by fuzzy_match.  score >= 1e8f means no match;
// lower (including negative) scores are better matches.
typedef struct {
    f32        score;
    i32        range_count;
    FuzzyRange ranges[FUZZY_MAX_RANGES];
} FuzzyMatch;

//
// API
//

// FZF-style fuzzy matching.
//   query     – user input (UTF-8)
//   candidate – single candidate string to match against (UTF-8)
// Returns a FuzzyMatch where .score >= 1e8f when there is no match.
// Match ranges describe byte-offset intervals suitable for per-segment
// highlighting (consecutive matched characters are merged into one range).
FuzzyMatch fuzzy_match(String query, String candidate, Arena* scratch);

#endif // FUZZY_H
