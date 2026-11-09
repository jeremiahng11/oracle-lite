#pragma once
// Date-range understanding for spoken agenda questions ("this week",
// "next week", "on Friday", "in December", "on 6th November", ...).
// Pure C++ (no Arduino types) so it can be unit-tested on a PC.
#include <cctype>
#include <cstdio>
#include <cstdlib>
#include <string>
#include <vector>

namespace agenda {

enum class RangeKind { Upcoming, Past, Range };

struct Range {
  RangeKind kind = RangeKind::Upcoming;
  std::string from, to;          // inclusive local days, YYYY-MM-DD (Range only)
  std::string phraseEn, phraseDe;
  bool includeOverdue = false;   // tasks: also list overdue ones (today questions)
  bool nextOnly = false;         // "what's my next appointment?"
  bool singleDay() const { return kind == RangeKind::Range && from == to; }
};

// Days since 1970-01-01 for a civil date (Howard Hinnant's algorithm).
inline long daysFromCivil(int y, int m, int d) {
  y -= m <= 2;
  const long era = (y >= 0 ? y : y - 399) / 400;
  const unsigned yoe = static_cast<unsigned>(y - era * 400);
  const unsigned doy = (153 * (m + (m > 2 ? -3 : 9)) + 2) / 5 + d - 1;
  const unsigned doe = yoe * 365 + yoe / 4 - yoe / 100 + doy;
  return era * 146097 + static_cast<long>(doe) - 719468;
}

inline void civilFromDays(long z, int& y, int& m, int& d) {
  z += 719468;
  const long era = (z >= 0 ? z : z - 146096) / 146097;
  const unsigned doe = static_cast<unsigned>(z - era * 146097);
  const unsigned yoe = (doe - doe / 1460 + doe / 36524 - doe / 146096) / 365;
  const unsigned doy = doe - (365 * yoe + yoe / 4 - yoe / 100);
  const unsigned mp = (5 * doy + 2) / 153;
  d = static_cast<int>(doy - (153 * mp + 2) / 5 + 1);
  m = static_cast<int>(mp < 10 ? mp + 3 : mp - 9);
  y = static_cast<int>(yoe) + static_cast<int>(era) * 400 + (m <= 2);
}

inline long dayNumber(const std::string& ymd) {
  if (ymd.size() < 10) return 0;
  return daysFromCivil(std::atoi(ymd.substr(0, 4).c_str()), std::atoi(ymd.substr(5, 2).c_str()),
                       std::atoi(ymd.substr(8, 2).c_str()));
}

inline std::string dayString(long n) {
  int y, m, d;
  civilFromDays(n, y, m, d);
  char buf[12];
  std::snprintf(buf, sizeof(buf), "%04d-%02d-%02d", y, m, d);
  return buf;
}

// 0 = Monday ... 6 = Sunday (1970-01-01 was a Thursday).
inline int weekdayMon0(long n) { return static_cast<int>(((n % 7) + 7 + 3) % 7); }

inline const char* monthEn(int m) {
  static const char* names[] = {"January","February","March","April","May","June","July",
                                "August","September","October","November","December"};
  return m >= 1 && m <= 12 ? names[m - 1] : "";
}
inline const char* monthDe(int m) {
  static const char* names[] = {"Januar","Februar","März","April","Mai","Juni","Juli",
                                "August","September","Oktober","November","Dezember"};
  return m >= 1 && m <= 12 ? names[m - 1] : "";
}
inline const char* weekdayEn(int w) {
  static const char* names[] = {"Monday","Tuesday","Wednesday","Thursday","Friday","Saturday","Sunday"};
  return w >= 0 && w < 7 ? names[w] : "";
}
inline const char* weekdayDe(int w) {
  static const char* names[] = {"Montag","Dienstag","Mittwoch","Donnerstag","Freitag","Samstag","Sonntag"};
  return w >= 0 && w < 7 ? names[w] : "";
}
inline std::string ordinal(int d) {
  const char* s = (d % 100 >= 11 && d % 100 <= 13) ? "th"
                : d % 10 == 1 ? "st" : d % 10 == 2 ? "nd" : d % 10 == 3 ? "rd" : "th";
  return std::to_string(d) + s;
}

// Lower-case word tokens (letters/digits; UTF-8 bytes kept inside words).
inline std::vector<std::string> words(const std::string& text) {
  std::vector<std::string> out;
  std::string cur;
  for (unsigned char c : text) {
    if (std::isalnum(c) || c >= 0x80) cur += static_cast<char>(std::tolower(c));
    else if (!cur.empty()) { out.push_back(cur); cur.clear(); }
  }
  if (!cur.empty()) out.push_back(cur);
  return out;
}

inline bool has(const std::string& s, const char* needle) { return s.find(needle) != std::string::npos; }

inline bool hasWord(const std::vector<std::string>& w, const char* word) {
  for (const std::string& x : w) if (x == word) return true;
  return false;
}

// Number 1..31 at token i ("6", "6th", "06").
inline int dayToken(const std::vector<std::string>& w, int i) {
  if (i < 0 || i >= static_cast<int>(w.size())) return 0;
  const std::string& t = w[i];
  if (t.empty() || !std::isdigit(static_cast<unsigned char>(t[0]))) return 0;
  const int n = std::atoi(t.c_str());
  size_t digits = 0;
  while (digits < t.size() && std::isdigit(static_cast<unsigned char>(t[digits]))) digits++;
  const std::string rest = t.substr(digits);
  if (!(rest.empty() || rest == "st" || rest == "nd" || rest == "rd" || rest == "th")) return 0;
  return n >= 1 && n <= 31 ? n : 0;
}

inline Range dayRange(long n, const std::string& en, const std::string& de) {
  Range r;
  r.kind = RangeKind::Range;
  r.from = r.to = dayString(n);
  r.phraseEn = en;
  r.phraseDe = de;
  return r;
}

inline Range spanRange(long a, long b, const std::string& en, const std::string& de) {
  Range r;
  r.kind = RangeKind::Range;
  r.from = dayString(a);
  r.to = dayString(b);
  r.phraseEn = en;
  r.phraseDe = de;
  return r;
}

inline std::string dateEn(long n) {
  int y, m, d;
  civilFromDays(n, y, m, d);
  return std::string(weekdayEn(weekdayMon0(n))) + " " + ordinal(d) + " " + monthEn(m);
}
inline std::string dateDe(long n) {
  int y, m, d;
  civilFromDays(n, y, m, d);
  return std::string(weekdayDe(weekdayMon0(n))) + ", " + std::to_string(d) + ". " + monthDe(m);
}

// lower: the question in lower case. today: local YYYY-MM-DD.
inline Range parseRange(const std::string& lower, const std::string& today) {
  const std::vector<std::string> w = words(lower);
  const long t = dayNumber(today);
  const int wd = weekdayMon0(t);
  int ty, tm, td;
  civilFromDays(t, ty, tm, td);
  const bool next = hasWord(w, "next") || hasWord(w, "nächste") || hasWord(w, "nächsten") ||
                    hasWord(w, "naechste") || hasWord(w, "naechsten") || hasWord(w, "kommende") ||
                    hasWord(w, "kommenden");

  // Past questions keep their own meaning.
  if (has(lower, "past event") || has(lower, "previous") || has(lower, "last week") ||
      has(lower, "vergangen") || has(lower, "vorbei") || has(lower, "früher") || has(lower, "frueher")) {
    Range r;
    r.kind = RangeKind::Past;
    r.phraseEn = "in the past";
    r.phraseDe = "zuletzt";
    if (has(lower, "last week")) {
      const long monday = t - wd - 7;
      r = spanRange(monday, monday + 6, "last week", "letzte Woche");
    }
    return r;
  }

  // Specific calendar date or month: "6th November", "November 6", "in December".
  static const char* monthKeysEn[] = {"january","february","march","april","may","june","july",
                                      "august","september","october","november","december"};
  static const char* monthKeysDe[] = {"januar","februar","märz","april","mai","juni","juli",
                                      "august","september","oktober","november","dezember"};
  static const char* monthShort[] = {"jan","feb","mar","apr","may","jun","jul","aug","sep","oct","nov","dec"};
  for (int i = 0; i < static_cast<int>(w.size()); i++) {
    int month = 0;
    for (int m = 0; m < 12 && !month; m++) {
      if (w[i] == monthKeysEn[m] || w[i] == monthKeysDe[m] || w[i] == monthShort[m] ||
          (m == 2 && w[i] == "maerz") || (m == 8 && w[i] == "sept")) month = m + 1;
    }
    if (!month) continue;
    int day = dayToken(w, i - 1);
    if (!day && i >= 2 && (w[i - 1] == "of")) day = dayToken(w, i - 2);
    if (!day) day = dayToken(w, i + 1);
    // "may" is also a verb: only treat it as the month with a day or after "in".
    if (month == 5 && w[i] == "may" && !day && !(i > 0 && (w[i - 1] == "in" || w[i - 1] == "of"))) continue;
    int year = ty;
    if (day) {
      if (daysFromCivil(year, month, day) < t) year++;
      const long n = daysFromCivil(year, month, day);
      return dayRange(n, "on " + dateEn(n), "am " + dateDe(n));
    }
    if (month < tm) year++;
    const long first = daysFromCivil(year, month, 1);
    const long last = daysFromCivil(month == 12 ? year + 1 : year, month == 12 ? 1 : month + 1, 1) - 1;
    Range r = spanRange(first < t ? t : first, last, std::string("in ") + monthEn(month),
                        std::string("im ") + monthDe(month));
    return r;
  }

  if (has(lower, "day after tomorrow") || has(lower, "übermorgen") || has(lower, "uebermorgen"))
    return dayRange(t + 2, "the day after tomorrow", "übermorgen");
  if (has(lower, "tomorrow") || hasWord(w, "morgen"))
    return dayRange(t + 1, "tomorrow", "morgen");
  if (has(lower, "today") || has(lower, "tonight") || has(lower, "heute")) {
    Range r = dayRange(t, "today", "heute");
    r.includeOverdue = true;
    return r;
  }

  const bool weekend = has(lower, "weekend") || has(lower, "wochenende");
  if (weekend) {
    long saturday = wd == 6 ? t - 1 : t + (5 - wd);
    if (next && wd < 5) saturday += 7;
    const long from = saturday < t ? t : saturday;
    return spanRange(from, saturday + 1, next ? "next weekend" : "this weekend",
                     next ? "nächstes Wochenende" : "am Wochenende");
  }
  if (has(lower, "week") || has(lower, "woche")) {
    if (next) {
      const long monday = t - wd + 7;
      return spanRange(monday, monday + 6, "next week", "nächste Woche");
    }
    return spanRange(t, t + (6 - wd), "this week", "diese Woche");
  }
  if (has(lower, "month") || has(lower, "monat")) {
    if (next) {
      const int ny = tm == 12 ? ty + 1 : ty, nm = tm == 12 ? 1 : tm + 1;
      const long first = daysFromCivil(ny, nm, 1);
      const long last = daysFromCivil(nm == 12 ? ny + 1 : ny, nm == 12 ? 1 : nm + 1, 1) - 1;
      return spanRange(first, last, "next month", "nächsten Monat");
    }
    const long last = daysFromCivil(tm == 12 ? ty + 1 : ty, tm == 12 ? 1 : tm + 1, 1) - 1;
    return spanRange(t, last, "this month", "diesen Monat");
  }

  static const char* dayKeysEn[] = {"monday","tuesday","wednesday","thursday","friday","saturday","sunday"};
  static const char* dayKeysDe[] = {"montag","dienstag","mittwoch","donnerstag","freitag","samstag","sonntag"};
  for (int d = 0; d < 7; d++) {
    if (!hasWord(w, dayKeysEn[d]) && !hasWord(w, dayKeysDe[d]) &&
        !(d < 6 && (hasWord(w, (std::string(dayKeysEn[d]) + "s").c_str())))) continue;
    int ahead = (d - wd + 7) % 7;
    if (next && ahead == 0) ahead = 7;
    const long n = t + ahead;
    return dayRange(n, "on " + dateEn(n), "am " + dateDe(n));
  }

  Range r;
  r.kind = RangeKind::Upcoming;
  r.phraseEn = "coming up";
  r.phraseDe = "demnächst";
  r.nextOnly = (has(lower, "next appointment") || has(lower, "next meeting") || has(lower, "next event") ||
                has(lower, "nächster termin") || has(lower, "nächsten termin") ||
                has(lower, "naechster termin") || has(lower, "naechsten termin"));
  return r;
}

// Questions about what was said in recorded meetings, not about the calendar.
// Only past-tense / minutes wording counts, so "When do I talk to San?" and
// "When do we discuss the budget?" stay calendar questions.
inline bool asksAboutMinutes(const std::string& lower) {
  static const char* cues[] = {
    "meeting minutes", "the minutes", "minutes of", "minutes from", "protokoll", "action item",
    "summary of", "summarize", "summarise", "zusammenfass",
    "discussed", "decided", "said", "talked about", "agreed", "mentioned",
    "besprochen", "entschieden", "beschlossen", "gesagt"};
  for (const char* cue : cues) if (has(lower, cue)) return true;
  return false;
}

// Words that name who/what the question is about ("San", "dentist").
inline std::vector<std::string> subjectWords(const std::string& lower) {
  static const char* stop[] = {
    "when","what","whats","which","who","where","how","do","does","did","have","has","had","any",
    "my","our","the","a","an","to","with","on","in","at","for","of","is","are","am","i","me","we",
    "this","next","last","week","weekend","today","tonight","tomorrow","day","after","time","times",
    "appointment","appointments","meeting","meetings","event","events","task","tasks","todo","todos",
    "schedule","scheduled","calendar","agenda","free","busy","coming","upcoming","up","and","or",
    "there","about","month","due","need","got","going","be","will","should","can","you","tell",
    "please","s","it","that","these","those","all","from","until","between","meet","see","talk",
    "call","plans","plan","anything","something","oracle","morning","afternoon","evening",
    "monday","tuesday","wednesday","thursday","friday","saturday","sunday",
    "mondays","tuesdays","wednesdays","thursdays","fridays","saturdays","sundays",
    "january","february","march","april","may","june","july","august","september","october",
    "november","december","st","nd","rd","th","jan","feb","mar","apr","jun","jul","aug","sep",
    "sept","oct","nov","dec","know","show","list","remind","whats","coming","got","have",
    "wann","was","welche","welcher","welchen","habe","ich","hab","wir","mein","meine","meinen",
    "termin","termine","aufgabe","aufgaben","heute","morgen","diese","dieser","nächste","nächsten",
    "naechste","naechsten","woche","wochenende","monat","am","im","um","mit","ein","eine","der",
    "die","das","den","dem","und","oder","gibt","es","noch","bis","für","fuer","zu","auf",
    "montag","dienstag","mittwoch","donnerstag","freitag","samstag","sonntag",
    "januar","februar","märz","maerz","mai","juni","juli","oktober","dezember","übermorgen",
    "uebermorgen"};
  std::vector<std::string> out;
  for (const std::string& word : words(lower)) {
    if (word.size() < 3 || std::isdigit(static_cast<unsigned char>(word[0]))) continue;
    bool skip = false;
    for (const char* s : stop) if (word == s) { skip = true; break; }
    if (!skip) out.push_back(word);
  }
  return out;
}

inline bool mentionsSubject(const std::string& itemText, const std::vector<std::string>& subject) {
  if (subject.empty()) return false;
  const std::vector<std::string> w = words(itemText);
  for (const std::string& s : subject)
    for (const std::string& x : w)
      if (x == s || (s.size() >= 4 && x.compare(0, s.size(), s) == 0)) return true;
  return false;
}


// Finds a clock time in text such as "9:30", "9.30pm", "3 pm", "21:15".
// Returns false when there is none (dates like 6/11 are not times).
inline bool clockTimeIn(const std::string& textIn, int& hour, int& minute) {
  std::string text;
  for (char c : textIn) text += static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
  for (size_t i = 0; i < text.size(); i++) {
    if (!std::isdigit(static_cast<unsigned char>(text[i])) || (i > 0 && std::isdigit(static_cast<unsigned char>(text[i - 1])))) continue;
    size_t j = i;
    int h = 0;
    while (j < text.size() && std::isdigit(static_cast<unsigned char>(text[j])) && j - i < 2) h = h * 10 + (text[j++] - '0');
    if (j < text.size() && std::isdigit(static_cast<unsigned char>(text[j]))) continue;   // 3+ digits
    int m = 0;
    bool hasMinutes = false;
    if (j + 2 < text.size() && (text[j] == ':' || text[j] == '.') &&
        std::isdigit(static_cast<unsigned char>(text[j + 1])) &&
        std::isdigit(static_cast<unsigned char>(text[j + 2])) &&
        (j + 3 >= text.size() || !std::isdigit(static_cast<unsigned char>(text[j + 3])))) {
      m = (text[j + 1] - '0') * 10 + (text[j + 2] - '0');
      hasMinutes = true;
      j += 3;
    }
    size_t k = j;
    while (k < text.size() && text[k] == ' ') k++;
    const bool pm = text.compare(k, 2, "pm") == 0 || text.compare(k, 4, "p.m.") == 0;
    const bool am = text.compare(k, 2, "am") == 0 || text.compare(k, 4, "a.m.") == 0;
    if (!hasMinutes && !am && !pm) continue;   // a bare number is not a time
    if (h > 23 || m > 59 || ((am || pm) && (h < 1 || h > 12))) continue;
    if (pm && h < 12) h += 12;
    if (am && h == 12) h = 0;
    hour = h;
    minute = m;
    return true;
  }
  return false;
}

}  // namespace agenda
