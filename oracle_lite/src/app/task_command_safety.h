#pragma once
#include <string>
// The fast local path is deliberately conservative. Questions, negations and
// conditional statements belong to the semantic, read-only interpretation.
inline bool unsafeLocalCompletion(const std::string& query) {
  const std::string padded = " " + query + " ";
  for (const char* word : {"nicht", "kein", "keine", "noch", "not", "never", "don't", "dont",
       "don t", "haven't", "haven t", "hasn't", "hasn t", "won't", "won t", "no",
       "nichtmehr", "wenn", "if", "falls", "soll", "sollte", "would", "could",
       "habe ich", "hast du", "ist die", "ist das", "is it", "is the", "are my", "did i", "have i", "whether"})
    if (padded.find(std::string(" ") + word + " ") != std::string::npos) return true;
  return query.find('?') != std::string::npos;
}
