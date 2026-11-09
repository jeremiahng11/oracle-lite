#include "ui.h"
#include "../../globals.h"
#include "draw.h"
#include "notes.h"
#include "tasks.h"
#include "task_visibility.h"
#include "settings.h"
#include "minutes.h"

namespace {
constexpr int kInset = 14;
constexpr int kRight = W - kInset;
constexpr int kContentWidth = W - 2 * kInset;
constexpr int kHeaderY = 10;
constexpr int kHeaderRuleY = 31;
constexpr int kContentTop = 43;
constexpr int kFooterRuleY = 174;
constexpr int kFooterTextY = 181;
constexpr int kAnswerLinesPerPage = 6;

struct UiTranslation { const char* en; const char* fr; const char* es; const char* it; const char* pl; const char* cs; const char* ru; const char* uk; const char* nl; const char* pt; };
const UiTranslation kUiTranslations[] = {
  {"TODAY","AUJOURD'HUI","HOY","OGGI","DZISIAJ","DNES","SEGODNYA","SOHODNI","VANDAAG","HOJE"},
  {"TASKS","TACHES","TAREAS","ATTIVITA","ZADANIA","UKOLY","ZADACHI","ZAVDANNYA","TAKEN","TAREFAS"},
  {"EVENTS","RENDEZ-VOUS","EVENTOS","EVENTI","TERMINY","TERMINY","SOBYTIYA","PODIYI","AFSPRAKEN","EVENTOS"},
  {"NEXT","SUIVANT","SIGUIENTE","AVANTI","DALEJ","DALSI","DALEE","DALI","VOLGENDE","SEGUINTE"},
  {"BACK","RETOUR","ATRAS","INDIETRO","WSTECZ","ZPET","NAZAD","NAZAD","TERUG","VOLTAR"},
  {"NOTE","NOTE","NOTA","NOTA","NOTATKA","POZNAMKA","ZAPISKA","NOTATKA","NOTITIE","NOTA"},
  {"ASK","QUESTION","PREGUNTA","DOMANDA","PYTANIE","DOTAZ","VOPROS","PYTANNYA","VRAAG","PERGUNTA"},
  {"LISTENING","ECOUTE","ESCUCHANDO","ASCOLTO","SLUCHAM","POSLOUCHAM","SLUSHAYU","SLUKHAYU","LUISTEREN","OUVINDO"},
  {"TRANSCRIBING","TRANSCRIPTION","TRANSCRIBIENDO","TRASCRIZIONE","TRANSKRYPCJA","PREPISUJI","RASSHIFROVKA","TRANSKRYBUYU","TRANSCRIBEREN","TRANSCREVENDO"},
  {"RECORDING","ENREGISTRE","GRABANDO","REGISTRA","NAGRYWANIE","NAHRAVANI","ZAPIS","ZAPYS","OPNAME","GRAVANDO"},
  {"SAVED","GARDE","GUARDADO","SALVATO","ZAPISANO","ULOZENO","SOHRANENO","ZBEREZHENO","OPGESLAGEN","GUARDADO"},
  {"ANSWER","REPONSE","RESPUESTA","RISPOSTA","ODPOWIEDZ","ODPOVED","OTVET","VIDPOVID","ANTWOORD","RESPOSTA"},
  {"FROM MEMORY","DE LA MEMOIRE","DE MEMORIA","DA MEMORIA","Z PAMIECI","Z PAMETI","IZ PAMYATI","Z PAMYATI","UIT GEHEUGEN","DA MEMORIA"},
  {"END","FIN","FIN","FINE","KONIEC","KONEC","KONEC","KINETS","EINDE","FIM"},
  {"READY","PRET","LISTO","PRONTO","GOTOWE","PRIPRAVENO","GOTOVO","HOTOVO","KLAAR","PRONTO"},
  {"CONNECTING","CONNEXION","CONECTANDO","CONNESSIONE","LACZENIE","PRIPOJOVANI","PODKLYUCHENIE","PIDKLYUCHENNYA","VERBINDEN","CONECTANDO"},
  {"ORGANIZING","CLASSEMENT","ORGANIZANDO","ORGANIZZO","PORZADKUJE","TRIDIM","SORTIRUYU","SORTUYU","ORDENEN","ORGANIZANDO"},
  {"REMEMBERING","MEMORISE","RECORDANDO","RICORDO","ZAPAMIETUJE","PAMATUJI","ZAPOMINAYU","ZAPAMYATOVUYU","ONTHOUDEN","MEMORIZANDO"},
  {"THINKING","REFLEXION","PENSANDO","PENSO","MYSLE","PREMYSLIM","DUMAYU","DUMAYU","DENKEN","PENSANDO"},
  {"SEARCHING","RECHERCHE","BUSCANDO","CERCO","SZUKAM","HLEDAM","ISHCHU","SHUKAYU","ZOEKEN","PROCURANDO"},
  {"ALMOST THERE","PRESQUE FINI","CASI LISTO","QUASI FATTO","PRAWIE GOTOWE","TEMER HOTOVO","POCHTI GOTOVO","MAYZHE HOTOVO","BIJNA KLAAR","QUASE PRONTO"},
  {"TRY AGAIN","REESSAYER","REINTENTAR","RIPROVA","SPROBUJ","ZNOVU","POVTORIT","SPROBUVATY","OPNIEUW","TENTAR NOVAMENTE"}
  ,{"ONE MOMENT","UN MOMENT","UN MOMENTO","UN MOMENTO","CHWILA","MOMENT","MOMENT","KHVYLYNU","EVEN WACHTEN","UM MOMENTO"}
  ,{"SAVED FOR LATER","GARDE POUR PLUS TARD","GUARDADO PARA DESPUES","SALVATO PER DOPO","ZAPISANE NA POZNIEJ","ULOZENO NA POZDEJI","SOHRANENO NA POTOM","ZBEREZHENO NA POTIM","BEWAARD VOOR LATER","GUARDADO PARA DEPOIS"}
  ,{"RECALLING","RAPPEL","RECORDANDO","RICHIAMO","PRZYPOMINAM","VYBAVUJI","VSPOMINAYU","ZGHADUYU","OPHALEN","RELEMBRANDO"}
  ,{"NO ANSWER","PAS DE REPONSE","SIN RESPUESTA","NESSUNA RISPOSTA","BRAK ODPOWIEDZI","BEZ ODPOVEDI","NET OTVETA","NEMA VIDPOVIDI","GEEN ANTWOORD","SEM RESPOSTA"}
  ,{"NO","AUCUN","NINGUNO","NESSUNO","BRAK","ZADNE","NET","NEMA","GEEN","SEM"}
  ,{"ALL","TOUT","TODO","TUTTO","WSZYSTKO","VSE","VSE","USE","ALLES","TUDO"}
  ,{"DONE","FAIT","HECHO","FATTO","GOTOWE","HOTOVO","GOTOVO","HOTOVO","KLAAR","FEITO"}
  ,{"AGAIN: BACK","ENCORE: RETOUR","OTRA VEZ: ATRAS","ANCORA: INDIETRO","PONOWNIE: WSTECZ","ZNOVU: ZPET","SNOVA: NAZAD","ZNOVU: NAZAD","NOGMAALS: TERUG","NOVAMENTE: VOLTAR"}
  ,{"NOT READY YET","PAS ENCORE PRET","AUN NO LISTO","NON ANCORA PRONTO","JESZCZE NIE GOTOWE","JESTE NEPRIPRAVENO","ESHCHO NE GOTOVO","SHCHE NE HOTOVO","NOG NIET KLAAR","AINDA NAO PRONTO"}
  ,{"SET UP","CONFIGURER","CONFIGURAR","CONFIGURA","KONFIGURUJ","NASTAVIT","NASTROIT","NALASHTUVATY","INSTELLEN","CONFIGURAR"}
  ,{"IN WEB FLASHER","DANS WEB FLASHER","EN WEB FLASHER","IN WEB FLASHER","W WEB FLASHER","V WEB FLASHER","V WEB FLASHER","U WEB FLASHER","IN WEB FLASHER","NO WEB FLASHER"}
  ,{"SOMETHING WENT WRONG","UNE ERREUR","ALGO SALIO MAL","QUALCOSA NON VA","COS POSZLO NIE TAK","NECO SE NEPOVEDLO","CHTO-TO NE TAK","SHCHOS NE TAK","ER GING IETS MIS","ALGO CORREU MAL"}
  ,{"MEMORIES","SOUVENIRS","RECUERDOS","RICORDI","WSPOMNIENIA","VZPOMINKY","VOSPOMINANIYA","SPHADY","HERINNERINGEN","MEMORIAS"}
};

const char* translatedUi(const char* english) {
  const String language = oracleUiLanguage();
  for (const auto& row : kUiTranslations) {
    if (strcmp(row.en, english)) continue;
    if (language == "fr") return row.fr; if (language == "es") return row.es;
    if (language == "it") return row.it; if (language == "pl") return row.pl;
    if (language == "cs") return row.cs; if (language == "ru") return row.ru;
    if (language == "uk") return row.uk; if (language == "nl") return row.nl;
    if (language == "pt") return row.pt; return english;
  }
  return english;
}

const char* uiText(const char* german, const char* english) {
  const String language = oracleUiLanguage();
  if (language == "de") return german;
  return translatedUi(english);
}

void labelLeft(int y, const char* text, int scale, uint8_t color, int maxWidth = kContentWidth) {
  if (!text || !strlen(text)) return;
  while (scale > 1 && textW(text, scale) > maxWidth) scale--;
  drawStrFit(kInset, y, maxWidth, text, scale, color);
}

void labelRight(int y, const char* text, int scale, uint8_t color, int maxWidth = kContentWidth) {
  if (!text || !strlen(text)) return;
  while (scale > 1 && textW(text, scale) > maxWidth) scale--;
  const int width = min(textW(text, scale), maxWidth);
  drawStrFit(kRight - width, y, width, text, scale, color);
}

// Oracle mark: five rounded voice-level bars (brand logo, viewBox 24x22,
// stroke 2.4). Rendered in one colour for e-paper at `scale` px per unit.
void drawOracleMark(int x, int y, float scale, uint8_t color) {
  static const float kBars[5][3] = {
    {3.0f, 9.0f, 13.0f}, {7.5f, 5.0f, 17.0f}, {12.0f, 8.0f, 14.0f},
    {16.5f, 3.0f, 19.0f}, {21.0f, 9.0f, 13.0f}};
  // Snap to an integer pitch so small sizes keep a clean gap between bars.
  const int width = max(2, static_cast<int>(2.4f * scale + 0.5f));
  const int pitch = max(width + 1, static_cast<int>(4.5f * scale + 0.5f));
  for (int i = 0; i < 5; i++) {
    const auto& bar = kBars[i];
    const int left = x + i * pitch;
    const int top = y + static_cast<int>((bar[1] - 1.2f) * scale + 0.5f);
    const int bottom = y + static_cast<int>((bar[2] + 1.2f) * scale + 0.5f);
    fillRoundRect(left, top, width, bottom - top, width / 2, color);
  }
}

void drawHeader(const char* right, uint8_t color) {
  drawOracleMark(kInset - 1, kHeaderY - 1, 0.6f, color);
  drawStr(kInset + 18, kHeaderY, "ORACLE", 1, color);
  labelRight(kHeaderY, right, 1, color, 88);
  hline(kInset, kHeaderRuleY, kContentWidth, color);
}

void drawFooter(const char* left, const char* right, uint8_t color) {
  hline(kInset, kFooterRuleY, kContentWidth, color);
  // Footer copy is interaction guidance, not user content. Never run it
  // through drawStrFit(), because an instruction such as ZURUECK must not turn
  // into an ambiguous ZUR... . All footer labels are deliberately authored to
  // fit at scale 1 and are drawn verbatim.
  if (left && strlen(left)) drawStr(kInset, kFooterTextY, left, 1, color);
  if (right && strlen(right))
    drawStr(kRight - textW(right, 1), kFooterTextY, right, 1, color);
}

void drawFooterCentered(const char* text, uint8_t color) {
  hline(kInset, kFooterRuleY, kContentWidth, color);
  if (text && strlen(text)) drawStrC(W / 2, kFooterTextY, text, 1, color);
}

void drawBattery(int percent) {
  if (percent < 0) return;
  percent = constrain(percent, 0, 100);
  constexpr int width = 23, height = 12;
  const int x = kRight - width - 2, y = 9;
  strokeRect(x, y, width, height, 1, BLACK);
  fillRect(x + width, y + 3, 2, 6, BLACK);
  const int bars = percent > 66 ? 3 : percent > 33 ? 2 : percent > 10 ? 1 : 0;
  for (int i = 0; i < bars; i++) fillRect(x + 3 + i * 6, y + 3, 5, 6, BLACK);
}

// 16x12 Wi-Fi glyph: two 2px arcs and a dot, MSB = leftmost pixel.
constexpr uint16_t kWifiGlyph[12] = {
  0b0000011111100000,
  0b0001111111111000,
  0b0111000000001110,
  0b1100000000000011,
  0b0000001111000000,
  0b0000111111110000,
  0b0001100000011000,
  0b0000000000000000,
  0b0000000110000000,
  0b0000001111000000,
  0b0000001111000000,
  0b0000000110000000,
};

void drawWifiIndicator(bool connected) {
  // Left of the sleep-lock slot (kRight - 43) and the battery.
  const int x = kRight - 64, y = 9;
  for (int row = 0; row < 12; row++)
    for (int col = 0; col < 16; col++)
      if (kWifiGlyph[row] & (0x8000 >> col)) px(x + col, y + row, BLACK);
  if (connected) return;
  // Disconnected: diagonal strike with a white halo so it reads on e-paper.
  thickLine(x + 1, y, x + 14, y + 11, 5, WHITE);
  thickLine(x + 1, y, x + 14, y + 11, 2, BLACK);
}

void drawHero(const char* text, int y, uint8_t color, int maxBottom = 122) {
  if (!text || !strlen(text)) return;
  for (int scale = 3; scale >= 1; scale--) {
    if (textW(text, scale) <= kContentWidth && y + uiFontHeight(scale) <= maxBottom) {
      labelLeft(y, text, scale, color);
      return;
    }
  }
  labelLeft(y, text, 1, color);
}

void drawTwoLineHero(const char* first, const char* second, int y, uint8_t color) {
  int scale = 3;
  while (scale > 1 && (textW(first, scale) > kContentWidth || textW(second, scale) > kContentWidth)) scale--;
  const int lineHeight = uiFontHeight(scale) + 4;
  labelLeft(y, first, scale, color);
  labelLeft(y + lineHeight, second, scale, color);
}

void drawStatusHero(const char* text, int y, uint8_t color) {
  if (!text || !*text) return;
  // Do not use labelLeft here: it silently falls back to regular-weight text.
  if (textW(text, 2) <= kContentWidth) {
    drawStr(kInset, y, text, 2, color);
    return;
  }
  // Never fall back to the thin body font for a long status. Split it into
  // two bold lines instead (for example SPAETER / WEITER).
  String value = text;
  int split = value.lastIndexOf(' ', value.length() / 2 + 1);
  if (split <= 0) split = value.indexOf(' ', value.length() / 2);
  if (split > 0) {
    const String first = value.substring(0, split);
    const String second = value.substring(split + 1);
    if (textW(first.c_str(), 2) <= kContentWidth && textW(second.c_str(), 2) <= kContentWidth) {
      drawStr(kInset, y - 7, first.c_str(), 2, color);
      drawStr(kInset, y + 19, second.c_str(), 2, color);
      return;
    }
  }
  // Long translations (including single words) wrap in a compact bold font.
  // Device status translations are ASCII/transliterated, unlike user content.
  String remaining = text;
  int lineY = y;
  while (remaining.length()) {
    unsigned int count = remaining.length();
    while (count > 1 && textW(remaining.substring(0, count).c_str(), -1) > kContentWidth) --count;
    if (count < remaining.length()) {
      const int space = remaining.lastIndexOf(' ', count);
      if (space > 0) count = space;
    }
    drawStr(kInset, lineY, remaining.substring(0, count).c_str(), -1, color);
    remaining = remaining.substring(count);
    remaining.trim();
    lineY += 18;
  }
}

void drawProgressSegments(int current, int total, int y, uint8_t color) {
  total = constrain(total, 1, 5);
  current = constrain(current, 0, total);
  constexpr int gap = 6;
  const int width = (kContentWidth - (total - 1) * gap) / total;
  for (int i = 0; i < total; i++) {
    const int x = kInset + i * (width + gap);
    if (i < current) fillRect(x, y, width, 6, color);
    else strokeRect(x, y, width, 6, 1, color);
  }
}

void drawTimer(uint32_t elapsedMs, uint8_t color) {
  const uint32_t seconds = elapsedMs / 1000;
  char timer[12];
  snprintf(timer, sizeof(timer), "%02lu:%02lu",
           static_cast<unsigned long>(seconds / 60),
           static_cast<unsigned long>(seconds % 60));
  drawBigStrC(100, 113, 39, timer, color);
}

int memoryStage(const char* phase) {
  if (!phase) return 0;
  if (!strcmp(phase, "CONNECTING")) return 1;
  if (!strcmp(phase, "DECODING")) return 2;
  if (!strcmp(phase, "UNDERSTANDING") || !strcmp(phase, "ORGANIZING") || !strcmp(phase, "REMEMBERING")) return 3;
  if (!strcmp(phase, "QUEUED")) return 0;
  return 0;
}

int askStage(const char* action) {
  if (!action) return 0;
  if (!strcmp(action, "CONNECTING")) return 1;
  if (!strcmp(action, "THINKING") || !strcmp(action, "DECODING") ||
      !strcmp(action, "UNDERSTANDING") || !strcmp(action, "PREPARING") ||
      !strcmp(action, "TODAY") || !strcmp(action, "SEARCHING")) return 2;
  if (!strcmp(action, "BUILDING")) return 3;
  return 0;
}

const char* memoryPhaseCopy(const char* phase) {
  if (!phase) return uiText("MOMENT...", "ONE MOMENT");
  if (!strcmp(phase, "CONNECTING")) return uiText("VERBINDE", "CONNECTING");
  if (!strcmp(phase, "DECODING")) return uiText("TRANSKRIBIERE", "TRANSCRIBING");
  if (!strcmp(phase, "UNDERSTANDING") || !strcmp(phase, "ORGANIZING")) return uiText("ORDNE EIN", "ORGANIZING");
  if (!strcmp(phase, "REMEMBERING")) return uiText("MERKE MIR", "REMEMBERING");
  if (!strcmp(phase, "QUEUED")) return uiText("SPAETER WEITER", "SAVED FOR LATER");
  if (!strcmp(phase, "DONE")) return uiText("FERTIG", "DONE");
  return uiText("MOMENT...", "ONE MOMENT");
}

const char* askPhaseCopy(const char* action) {
  if (!action) return uiText("DENKE NACH", "THINKING");
  if (!strcmp(action, "CONNECTING")) return uiText("VERBINDE", "CONNECTING");
  if (!strcmp(action, "DECODING") || !strcmp(action, "UNDERSTANDING")) return uiText("HOERE REIN", "LISTENING");
  if (!strcmp(action, "PREPARING")) return uiText("HOLE ES", "RECALLING");
  if (!strcmp(action, "SEARCHING")) return uiText("SUCHE...", "SEARCHING");
  if (!strcmp(action, "BUILDING")) return uiText("FAST DA", "ALMOST THERE");
  if (!strcmp(action, "TODAY")) return uiText("HEUTE", "TODAY");
  return uiText("DENKE NACH", "THINKING");
}

static void wrapParagraph(const String& raw, std::vector<String>& lines);

// Lines for answer pages. A '\n' in the text starts a new line.
std::vector<String> answerLines(const String& raw) {
  std::vector<String> lines;
  int start = 0;
  while (start <= static_cast<int>(raw.length())) {
    int end = raw.indexOf('\n', start);
    if (end < 0) end = raw.length();
    wrapParagraph(raw.substring(start, end), lines);
    start = end + 1;
  }
  if (lines.empty()) lines.push_back(uiText("KEINE ANTWORT", "NO ANSWER"));
  return lines;
}

static void wrapParagraph(const String& raw, std::vector<String>& lines) {
  const String text = normalizeForDisplay(raw);
  String line;
  int pos = 0;
  while (pos < static_cast<int>(text.length())) {
    while (pos < static_cast<int>(text.length()) && text[pos] == ' ') pos++;
    if (pos >= static_cast<int>(text.length())) break;
    int end = text.indexOf(' ', pos);
    if (end < 0) end = text.length();
    String word = text.substring(pos, end);
    pos = end + 1;
    while (textW(word.c_str(), 1) > kContentWidth) {
      int cut = word.length();
      while (cut > 1 && textW((word.substring(0, cut) + "-").c_str(), 1) > kContentWidth) cut--;
      if (line.length()) { lines.push_back(line); line = ""; }
      lines.push_back(word.substring(0, cut) + "-");
      word = word.substring(cut);
    }
    const String candidate = line.length() ? line + " " + word : word;
    if (textW(candidate.c_str(), 1) <= kContentWidth) line = candidate;
    else { if (line.length()) lines.push_back(line); line = word; }
  }
  if (line.length()) lines.push_back(line);
}

String homeDateLabel() {
  String local = utcToLocalDeviceLabel(currentUtcIso());
  if (local.length() < 10 || local == "time not set") return uiText("DATUM FEHLT", "DATE NOT SET");
  static const UiTranslation months[] = {
    {"JAN","JAN","ENE","GEN","STY","LED","YAN","SIC","JAN","JAN"},
    {"FEB","FEV","FEB","FEB","LUT","UNO","FEV","LYU","FEB","FEV"},
    {"MAR","MAR","MAR","MAR","MAR","BRE","MAR","BER","MRT","MAR"},
    {"APR","AVR","ABR","APR","KWI","DUB","APR","KVI","APR","ABR"},
    {"MAY","MAI","MAY","MAG","MAJ","KVE","MAY","TRA","MEI","MAI"},
    {"JUN","JUI","JUN","GIU","CZE","CER","IYU","CHE","JUN","JUN"},
    {"JUL","JUI","JUL","LUG","LIP","CEC","IYU","LYP","JUL","JUL"},
    {"AUG","AOU","AGO","AGO","SIE","SRP","AVG","SER","AUG","AGO"},
    {"SEP","SEP","SEP","SET","WRZ","ZAR","SEN","VER","SEP","SET"},
    {"OCT","OCT","OCT","OTT","PAZ","RIJ","OKT","ZHO","OKT","OUT"},
    {"NOV","NOV","NOV","NOV","LIS","LIS","NOY","LYS","NOV","NOV"},
    {"DEC","DEC","DIC","DIC","GRU","PRO","DEK","HRU","DEC","DEZ"}
  };
  const int month = constrain(local.substring(5, 7).toInt(), 1, 12);
  const String language = oracleUiLanguage();
  const UiTranslation& row = months[month - 1];
  const char* label = row.en;
  if (language == "fr") label = row.fr; else if (language == "es") label = row.es;
  else if (language == "it") label = row.it; else if (language == "pl") label = row.pl;
  else if (language == "cs") label = row.cs; else if (language == "ru") label = row.ru;
  else if (language == "uk") label = row.uk; else if (language == "nl") label = row.nl;
  else if (language == "pt") label = row.pt;
  return local.substring(8, 10) + " " + label;
}

String greetingLabel() {
  const String local = utcToLocalDeviceLabel(currentUtcIso());
  if (local.length() < 13 || local == "time not set") return "TODAY";
  const int hour = local.substring(11, 13).toInt();
  if (hour < 12) return "GUTEN MORGEN";
  if (hour < 18) return "GUTEN TAG";
  return "GUTEN ABEND";
}

String nextTodayItem(const std::vector<BrainTaskRecord>& tasks, const String& todayKey) {
  String candidate;
  String candidateDue;
  for (const BrainTaskRecord& task : tasks) {
    if (task.done || task.type == "event") continue;
    if (!task.dueUtc.length()) {
      if (!candidate.length()) candidate = task.text;
      continue;
    }
    if (!todayKey.length()) continue;
    const String localDue = utcToLocalDeviceLabel(task.dueUtc);
    if (localDue.length() < 10 || localDue == "time not set") continue;
    if (localDue.substring(0, 10) > todayKey) continue;
    if (!candidate.length() || localDue < candidateDue) {
      candidate = task.text;
      candidateDue = localDue;
    }
  }
  return normalizeForDisplay(candidate);
}

std::vector<BrainTaskRecord> todayAgendaItems(bool events) {
  const String nowUtc = currentUtcIso();
  const String localNow = utcToLocalDeviceLabel(nowUtc);
  const String todayKey = localNow.length() >= 10 && localNow != "time not set"
                            ? localNow.substring(0, 10) : "";
  std::vector<BrainTaskRecord> items;
  for (const BrainTaskRecord& item : loadBrainTasks()) {
    if (item.done) continue;
    if (!item.dueUtc.length()) {
      if (!events && undatedTaskVisible(item.type == "event", item.done)) items.push_back(item);
      continue;
    }
    if (!todayKey.length()) continue;
    const String localDue = utcToLocalDeviceLabel(item.dueUtc);
    if (localDue.length() < 10 || localDue == "time not set") continue;
    const String dueDay = localDue.substring(0, 10);
    if (events ? (item.type == "event" && dueDay == todayKey && eventStillRelevant(
                   dueDay.c_str(), todayKey.c_str(), item.dueUtc.c_str(), nowUtc.c_str(),
                   item.datePrecision == "day"))
               : (item.type != "event" && dueDay <= todayKey)) items.push_back(item);
  }
  std::sort(items.begin(), items.end(), [](const BrainTaskRecord& a, const BrainTaskRecord& b) {
    return a.dueUtc < b.dueUtc;
  });
  return items;
}

constexpr int kAgendaLinesPerPage = 5;

int agendaPages(const std::vector<BrainTaskRecord>& items) {
  int pages = 0;
  for (const BrainTaskRecord& item : items) {
    const int lines = max(1, static_cast<int>(answerLines(item.text).size()));
    pages += (lines + kAgendaLinesPerPage - 1) / kAgendaLinesPerPage;
  }
  return max(1, pages);
}
}  // namespace

RTC_DATA_ATTR static bool wifiIndicatorConnected = false;
void setWifiIndicator(bool connected) { wifiIndicatorConnected = connected; }
bool wifiIndicatorState() { return wifiIndicatorConnected; }
// The icon actually on the panel. Home is redrawn whenever it no longer matches
// the connection state, whichever code path connected or disconnected.
static bool shownWifiIcon = false;
bool homeWifiIconStale() { return shownWifiIcon != wifiIndicatorConnected; }

// Home's two counts survive deep sleep, so waking can draw Home before the
// SD card and the agenda are loaded (same day only; the data cannot change
// while asleep).
RTC_DATA_ATTR static int homeCacheTasks = -1, homeCacheEvents = -1;
RTC_DATA_ATTR static char homeCacheDay[11] = "";

static void drawHome(int taskCount, int eventCount, int batteryPercent);

bool showIdleFromCache(int batteryPercent) {
  const String localNow = utcToLocalDeviceLabel(currentUtcIso());
  if (homeCacheTasks < 0 || localNow.length() < 10 || localNow == "time not set" ||
      localNow.substring(0, 10) != homeCacheDay) return false;
  drawHome(homeCacheTasks, homeCacheEvents, batteryPercent);
  return true;
}

static void homeCounts(int& taskCount, int& eventCount, String& dayKey);

// True when the counts on a Home drawn from the sleep cache are out of date
// (e.g. an appointment passed while asleep). Needs the SD card.
bool homeCacheStale() {
  int tasks = 0, events = 0;
  String day;
  homeCounts(tasks, events, day);
  return tasks != homeCacheTasks || events != homeCacheEvents || day != homeCacheDay;
}

void showIdle(int pendingCount, int batteryPercent) {
  (void)pendingCount;
  int taskCount = 0, eventCount = 0;
  String todayKey;
  homeCounts(taskCount, eventCount, todayKey);
  homeCacheTasks = taskCount;
  homeCacheEvents = eventCount;
  strlcpy(homeCacheDay, todayKey.c_str(), sizeof(homeCacheDay));
  drawHome(taskCount, eventCount, batteryPercent);
}

static void homeCounts(int& taskCount, int& eventCount, String& todayKeyOut) {
  const String nowUtc = currentUtcIso();
  const String localNow = utcToLocalDeviceLabel(nowUtc);
  const String todayKey = localNow.length() >= 10 && localNow != "time not set" ? localNow.substring(0, 10) : "";
  todayKeyOut = todayKey;
  const std::vector<BrainTaskRecord> tasks = loadBrainTasks();
  taskCount = 0; eventCount = 0;
  for (const BrainTaskRecord& task : tasks) {
    if (task.done) continue;
    if (!task.dueUtc.length()) {
      if (undatedTaskVisible(task.type == "event", task.done)) taskCount++;
      continue;
    }
    if (!todayKey.length()) continue;
    const String localDue = utcToLocalDeviceLabel(task.dueUtc);
    if (localDue.length() < 10 || localDue == "time not set") continue;
    const String dueDay = localDue.substring(0, 10);
    if (task.type == "event") {
      if (dueDay == todayKey && eventStillRelevant(dueDay.c_str(), todayKey.c_str(),
          task.dueUtc.c_str(), nowUtc.c_str(), task.datePrecision == "day")) eventCount++;
    }
    else if (dueDay <= todayKey) taskCount++;
  }
}

static void drawHome(int taskCount, int eventCount, int batteryPercent) {
  clearWhite();
  drawHeader(nullptr, BLACK);
  drawBattery(batteryPercent);
  drawWifiIndicator(wifiIndicatorConnected);
  shownWifiIcon = wifiIndicatorConnected;
  const String date = homeDateLabel();
  labelLeft(kContentTop, date.c_str(), 1, BLACK);
  drawHero(uiText("HEUTE", "TODAY"), 63, BLACK, 109);
  hline(kInset, 116, kContentWidth, BLACK);
  labelLeft(128, uiText("AUFGABEN", "TASKS"), 1, BLACK, 120);
  char count[8];
  snprintf(count, sizeof(count), "%d", min(taskCount, 999));
  labelRight(124, count, 2, BLACK, 48);
  labelLeft(159, uiText("TERMINE", "EVENTS"), 1, BLACK, 120);
  snprintf(count, sizeof(count), "%d", min(eventCount, 999));
  labelRight(155, count, 2, BLACK, 48);
  refreshAtRest();
}

int todayAgendaPageCount(bool events) {
  return agendaPages(todayAgendaItems(events));
}

void showTodayAgenda(bool events, int page) {
  clearWhite();
  const std::vector<BrainTaskRecord> items = todayAgendaItems(events);
  const int pages = agendaPages(items);
  page = constrain(page, 0, pages - 1);
  char pageLabel[12];
  snprintf(pageLabel, sizeof(pageLabel), "%d/%d", page + 1, pages);
  drawHeader(items.empty() ? nullptr : pageLabel, BLACK);
  labelLeft(kContentTop, events ? uiText("TERMINE", "EVENTS") : uiText("AUFGABEN", "TASKS"), 1, BLACK, 112);

  if (items.empty()) {
    drawTwoLineHero(events ? uiText("KEINE", "NO") : uiText("NICHTS", "ALL"),
                    events ? uiText("TERMINE", "EVENTS") : uiText("OFFEN", "DONE"),
                    76, BLACK);
  } else {
    int remaining = page;
    int selected = 0;
    int firstLine = 0;
    std::vector<String> lines;
    for (int i = 0; i < static_cast<int>(items.size()); i++) {
      lines = answerLines(items[i].text);
      const int itemPages = max(1, (static_cast<int>(lines.size()) + kAgendaLinesPerPage - 1) /
                                       kAgendaLinesPerPage);
      if (remaining < itemPages) {
        selected = i;
        firstLine = remaining * kAgendaLinesPerPage;
        break;
      }
      remaining -= itemPages;
    }
    const String localDue = utcToLocalDeviceLabel(items[selected].dueUtc);
    String meta = events && items[selected].datePrecision != "day" && localDue.length() >= 16 ? localDue.substring(11, 16)
                                                    : String(selected + 1);
    labelRight(kContentTop, meta.c_str(), 1, BLACK, 58);
    for (int i = 0; i < kAgendaLinesPerPage && firstLine + i < static_cast<int>(lines.size()); i++)
      drawStr(kInset, 68 + i * 19, lines[firstLine + i].c_str(), 1, BLACK);
  }
  if (items.empty() || pages == 1) drawFooterCentered(uiText("NOCHMAL: ZURUECK", "AGAIN: BACK"), BLACK);
  else drawFooter(uiText("WEITER", "NEXT"), uiText("ZURUECK", "BACK"), BLACK);
  // Hand the panel update to core 0 so the buttons stay responsive.
  refreshAsync();
}

void showRecording() {
  fillRect(0, 0, W, H, BLACK);
  drawHeader(uiText("NOTIZ", "NOTE"), WHITE);
  drawStatusHero(uiText("HOERE ZU", "LISTENING"), 67, WHITE);
  fillCircle(kRight - 7, 146, 7, WHITE);
  labelLeft(139, uiText("AUFNAHME", "RECORDING"), 1, WHITE);
  refresh();
}

void showRecordingProgress(uint32_t elapsedMs, int moments) {
  fillRect(0, 0, W, H, BLACK);
  drawHeader(uiText("NOTIZ", "NOTE"), WHITE);
  labelLeft(kContentTop, uiText("AUFNAHME", "RECORDING"), 1, WHITE);
  drawTimer(elapsedMs, WHITE);
  refreshFast();
}

void showSaved(int noteNumber) {
  clearWhite();
  char number[12];
  snprintf(number, sizeof(number), "#%03d", noteNumber % 1000);
  drawHeader(number, BLACK);
  strokeCircle(36, 67, 14, 2, BLACK);
  thickLine(29, 67, 34, 73, 2, BLACK);
  thickLine(34, 73, 44, 60, 2, BLACK);
  drawStatusHero(uiText("GEMERKT", "SAVED"), 96, BLACK);
  refresh();
}

void showMemoryProcessing(int noteNumber, const char* phase) {
  clearWhite();
  char number[12];
  snprintf(number, sizeof(number), "#%03d", noteNumber % 1000);
  drawHeader(number, BLACK);
  labelLeft(kContentTop, uiText("NOTIZ", "NOTE"), 1, BLACK);
  drawStatusHero(memoryPhaseCopy(phase), 78, BLACK);
  const int stage = memoryStage(phase);
  drawProgressSegments(stage, 3, 151, BLACK);
  refreshAsync();
}

void showBacklogProcessing(int completed, int total, const char* phase) {
  clearWhite();
  completed = constrain(completed, 0, max(total, 1));
  total = max(total, 1);
  drawHeader("SYNC", BLACK);
  char progress[20];
  snprintf(progress, sizeof(progress), "%d / %d", completed, total);
  labelLeft(kContentTop, progress, 1, BLACK);
  drawStatusHero(memoryPhaseCopy(phase), 78, BLACK);
  strokeRect(kInset, 149, kContentWidth, 12, 1, BLACK);
  const int innerWidth = kContentWidth - 4;
  const int filled = total > 0 ? (innerWidth * completed) / total : 0;
  if (filled > 0) fillRect(kInset + 2, 151, filled, 8, BLACK);
  refreshAsync();
}

void showAskListening() {
  fillRect(0, 0, W, H, BLACK);
  drawHeader(uiText("FRAGE", "ASK"), WHITE);
  drawStatusHero(uiText("HOERE ZU", "LISTENING"), 67, WHITE);
  fillCircle(kRight - 7, 146, 7, WHITE);
  labelLeft(139, uiText("AUFNAHME", "RECORDING"), 1, WHITE);
  refresh();
}

void showAskThinking() { showAskProgress("THINKING", "WITH MEMORY"); }

void showAskProgress(const char* action, const char* object) {
  clearWhite();
  drawHeader(uiText("FRAGE", "ASK"), BLACK);
  drawStatusHero(askPhaseCopy(action), 78, BLACK);
  const int stage = askStage(action);
  drawProgressSegments(stage, 3, 151, BLACK);
  refreshAsync();
}

void showAskSpeaking(const String& answer, int sourceCount) {
  showAskSpeakingPage(answer, sourceCount, 0);
}

int askAnswerPageCount(const String& answer) {
  const int count = answerLines(answer).size();
  return max(1, (count + kAnswerLinesPerPage - 1) / kAnswerLinesPerPage);
}

// The question that was heard (web questions), shown before the answer and
// as the last page when cycling through the answer.
void showAskQuestion(const String& question) {
  const std::vector<String> lines = answerLines(question);
  clearWhite();
  drawHeader(nullptr, BLACK);
  labelLeft(kContentTop, uiText("FRAGE", "QUESTION"), 1, BLACK);
  for (int i = 0; i < kAnswerLinesPerPage && i < static_cast<int>(lines.size()); i++)
    labelLeft(65 + i * 18, lines[i].c_str(), 1, BLACK);
  drawFooter(uiText("ANTWORT", "ANSWER"), uiText("ZURUECK", "BACK"), BLACK);
  refreshAsync();
}

void showAskSpeakingPage(const String& answer, int sourceCount, int page) {
  const std::vector<String> lines = answerLines(answer);
  const int pages = max(1, (static_cast<int>(lines.size()) + kAnswerLinesPerPage - 1) / kAnswerLinesPerPage);
  page = constrain(page, 0, pages - 1);
  clearWhite();
  char pageLabel[12];
  snprintf(pageLabel, sizeof(pageLabel), "%d/%d", page + 1, pages);
  drawHeader(pageLabel, BLACK);
  labelLeft(kContentTop, sourceCount > 0 ? uiText("AUS ERINNERUNGEN", "FROM MEMORY")
                                         : uiText("ANTWORT", "ANSWER"), 1, BLACK);
  const int first = page * kAnswerLinesPerPage;
  for (int i = 0; i < kAnswerLinesPerPage && first + i < static_cast<int>(lines.size()); i++)
    labelLeft(65 + i * 18, lines[first + i].c_str(), 1, BLACK);
  drawFooter(page < pages - 1 ? uiText("WEITER", "NEXT") : uiText("ENDE", "END"),
             uiText("ZURUECK", "BACK"), BLACK);
  // On core 0, so a voice that is playing keeps getting data meanwhile.
  refreshAsync();
}

void showTransferConnecting() {
  clearWhite();
  drawHeader("LOCAL", BLACK);
  labelLeft(kContentTop, "COMPANION", 1, BLACK);
  drawStatusHero(uiText("VERBINDE", "CONNECTING"), 67, BLACK);
  drawProgressSegments(1, 3, 143, BLACK);
  drawFooter("1 / 3", "WORKING", BLACK);
  refresh();
}

void showTransferMode(const char* ip, const char* apPassword) {
  clearWhite();
  drawHeader("COMPANION", BLACK);
  if (apPassword && strlen(apPassword) > 0) {
    labelLeft(kContentTop, "JOIN WI-FI", 1, BLACK);
    drawHero("ORACLE LITE", 66, BLACK, 100);
    labelLeft(112, "PASSWORD", 1, BLACK);
    // Large type fits ~14 characters; longer fixed passwords use small type
    // (two lines beyond ~30 characters).
    const String pw = apPassword;
    if (pw.length() <= 14) labelLeft(132, apPassword, 2, BLACK);
    else if (pw.length() <= 30) labelLeft(132, apPassword, 1, BLACK);
    else {
      labelLeft(128, pw.substring(0, 30).c_str(), 1, BLACK);
      labelLeft(142, pw.substring(30).c_str(), 1, BLACK);
    }
    drawFooter("SETUP", ip, BLACK);
  } else {
    labelLeft(kContentTop, "OPEN IN BROWSER", 1, BLACK);
    fillRect(kInset, 70, kContentWidth, 52, BLACK);
    drawStrInBox(kInset, 70, kContentWidth, 52, ip, 2, WHITE);
    labelLeft(140, "SAME WI-FI", 1, BLACK);
    drawFooter("COMPANION", "READY", BLACK);
  }
  refresh();
}

void showVolumeSetting(int percent) {
  clearWhite();
  drawHeader("SETTINGS", BLACK);
  const int level = constrain(percent, 20, 100);
  char value[18];
  snprintf(value, sizeof(value), "VOLUME %d", (level + 19) / 20);
  labelLeft(kContentTop, "SPEAKER", 1, BLACK);
  drawHero(value, 67, BLACK, 108);
  const int bars = constrain((level + 19) / 20, 1, 5);
  for (int i = 0; i < 5; i++) {
    const int height = 8 + i * 7;
    if (i < bars) fillRect(18 + i * 34, 157 - height, 20, height, BLACK);
    else strokeRect(18 + i * 34, 157 - height, 20, height, 1, BLACK);
  }
  drawFooter("UPPER +", "LOWER -", BLACK);
  refresh();
}

void showSetupRequired() {
  clearWhite();
  drawHeader("SETUP", BLACK);
  labelLeft(kContentTop, uiText("NOCH NICHT BEREIT", "NOT READY YET"), 1, BLACK);
  drawHero(uiText("EINRICHTEN", "SET UP"), 70, BLACK, 120);
  labelLeft(143, uiText("IM WEB FLASHER", "IN WEB FLASHER"), 1, BLACK);
  drawFooter("USB", uiText("BEREIT", "READY"), BLACK);
  refresh();
}

void showError(const char* message) {
  Serial.printf("[UI] error: %s\n", message ? message : "unspecified");
  clearWhite();
  drawHeader("ORACLE", BLACK);
  drawStatusHero(uiText("NOCHMAL?", "TRY AGAIN"), 72, BLACK);
  labelLeft(143, uiText("HAT NICHT GEKLAPPT", "SOMETHING WENT WRONG"), 1, BLACK);
  if (message && *message) labelLeft(158, message, 1, BLACK);
  refresh();
}

void showSleepLocked() {
  waitForDisplay();   // draws over the current frame
  // Sleeping: the Wi-Fi icon's slot shows "Zzz"; the next wake starts from
  // disconnected and the icon returns once Wi-Fi has joined.
  wifiIndicatorConnected = false;
  shownWifiIcon = false;
  {
    // 16x12 "Zzz": large, medium and small Z rising to the right.
    static const uint16_t kZzz[12] = {
      0b0000000000001111,
      0b0000000000000010,
      0b0000000111110100,
      0b0000000000101111,
      0b0000000001000000,
      0b1111110010000000,
      0b1111110111110000,
      0b0001100000000000,
      0b0011000000000000,
      0b0110000000000000,
      0b1111110000000000,
      0b1111110000000000,
    };
    const int x0 = kRight - 64, y0 = 9;
    fillRect(x0, y0, 16, 12, WHITE);
    for (int row = 0; row < 12; row++)
      for (int col = 0; col < 16; col++)
        if (kZzz[row] & (0x8000 >> col)) px(x0 + col, y0 + row, BLACK);
  }
  // Preserve the useful home screen and add only a compact persistent sleep
  // indicator beside the battery. E-paper keeps this visible without power.
  constexpr int x = kRight - 43;
  constexpr int y = 10;
  strokeRect(x, y + 5, 11, 9, 1, BLACK);
  // Lock shackle: two vertical shoulders joined by a short top line.
  vline(x + 2, y + 2, 4, BLACK);
  vline(x + 8, y + 2, 4, BLACK);
  hline(x + 2, y + 1, 7, BLACK);
  fillRect(x + 5, y + 9, 2, 3, BLACK);
  refresh();
}

void showUltraSleepScreen() {
  clearWhite();
  drawHeader("RESTING", BLACK);
  drawOracleMark(W / 2 - 31, 44, 3.0f, BLACK);   // 63 x 66 px logo
  drawStrC(W / 2, 118, "ORACLE", 2, BLACK);
  drawStrC(W / 2, 150, "PRESS TO WAKE", 1, BLACK);
  refreshClean();
}

// ─── Meeting minutes ───────────────────────────────────────────────────────
void showMeetingRecording(uint32_t elapsedMs, bool fast) {
  // Time in the meeting only; the live transcript is on the Oracle Minutes server.
  fillRect(0, 0, W, H, BLACK);
  drawHeader(uiText("MEETING", "MEETING"), WHITE);
  labelLeft(kContentTop, minutesLiveConnected() ? "LIVE TO ORACLE" : "RECORDING ON DEVICE", 1, WHITE);
  fillCircle(kRight - 7, kContentTop + 6, 5, WHITE);
  char minutes[8];
  snprintf(minutes, sizeof(minutes), "%lu", static_cast<unsigned long>(elapsedMs / 60000UL));
  drawBigStrC(100, 74, 52, minutes, WHITE);
  drawStrC(W / 2, 136, "MIN", 1, WHITE);
  drawFooter("TOP: STOP", "", WHITE);
  if (fast) refreshFast(); else refresh();
}

void showMeetingStatus(const char* phase, int percent, const char* detail) {
  clearWhite();
  drawHeader("MINUTES", BLACK);
  labelLeft(kContentTop, "MEETING", 1, BLACK);
  drawStatusHero(phase ? phase : "", 74, BLACK);
  if (percent >= 0) {
    percent = constrain(percent, 0, 100);
    strokeRect(kInset, 132, kContentWidth, 10, 1, BLACK);
    fillRect(kInset + 2, 134, (kContentWidth - 4) * percent / 100, 6, BLACK);
    char label[8];
    snprintf(label, sizeof(label), "%d%%", percent);
    labelRight(150, label, 1, BLACK);
  }
  if (detail && *detail) labelLeft(150, detail, 1, BLACK, percent >= 0 ? 130 : kContentWidth);
  refreshAsync();
}
