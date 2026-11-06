#pragma once
#include <cstring>
enum AnswerMessage { NoMemory, NoAgenda, UpdateFailed, NoChange };
inline const char* answerMessage(const char* language, AnswerMessage kind) {
  struct Row { const char* code; const char* text[4]; };
  static const Row rows[] = {
    {"de", {"Dazu finde ich keine verlässliche Erinnerung.", "Es gibt keine passenden offenen Aufgaben oder anstehenden Termine.", "Ich konnte die Aufgabe nicht aktualisieren.", "Ich habe keine Aufgabe geändert."}},
    {"en", {"I cannot find a reliable memory about that.", "There are no matching open tasks or upcoming events.", "I could not update that task.", "I did not change any task."}},
    {"fr", {"Je ne trouve aucun souvenir fiable à ce sujet.", "Il n’y a aucune tâche ouverte ni aucun rendez-vous à venir correspondant.", "Je n’ai pas pu mettre à jour cette tâche.", "Je n’ai modifié aucune tâche."}},
    {"es", {"No encuentro un recuerdo fiable sobre eso.", "No hay tareas pendientes ni citas próximas que coincidan.", "No pude actualizar esa tarea.", "No he cambiado ninguna tarea."}},
    {"it", {"Non trovo un ricordo affidabile al riguardo.", "Non ci sono attività aperte o appuntamenti futuri corrispondenti.", "Non ho potuto aggiornare questa attività.", "Non ho modificato alcuna attività."}},
    {"pl", {"Nie znajduję wiarygodnego wspomnienia na ten temat.", "Nie ma pasujących otwartych zadań ani nadchodzących spotkań.", "Nie udało mi się zaktualizować tego zadania.", "Nie zmieniłem żadnego zadania."}},
    {"cs", {"K tomu nenacházím spolehlivou vzpomínku.", "Nejsou žádné odpovídající otevřené úkoly ani nadcházející schůzky.", "Tento úkol se nepodařilo aktualizovat.", "Žádný úkol jsem nezměnil."}},
    {"ru", {"Я не нахожу достоверного воспоминания об этом.", "Нет подходящих незавершённых задач или предстоящих встреч.", "Не удалось обновить эту задачу.", "Я не изменил ни одной задачи."}},
    {"uk", {"Я не знаходжу достовірного спогаду про це.", "Немає відповідних незавершених завдань або майбутніх зустрічей.", "Не вдалося оновити це завдання.", "Я не змінив жодного завдання."}},
    {"nl", {"Ik vind hierover geen betrouwbare herinnering.", "Er zijn geen passende open taken of komende afspraken.", "Ik kon die taak niet bijwerken.", "Ik heb geen taak gewijzigd."}},
    {"pt", {"Não encontro uma memória fiável sobre isso.", "Não há tarefas pendentes nem compromissos futuros correspondentes.", "Não consegui atualizar essa tarefa.", "Não alterei nenhuma tarefa."}}
  };
  for (const Row& row : rows) if (!std::strcmp(language, row.code)) return row.text[kind];
  return rows[1].text[kind];
}
