/*
 * Smart Attendance Assistant (pure C)
 *
 * For every lecture session the program counts present and absent students,
 * then lists the SMALLER group to keep the report short:
 *   - present > absent  -> list the ABSENT students
 *   - absent  > present -> list the PRESENT students
 *   - tie               -> list the ABSENT students
 *
 * A full report is also saved to "attendance_report.txt".
 *
 * Compile: gcc -Wall -o smart_attendance smart_attendance.c
 * Run:     ./smart_attendance
 */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <ctype.h>

#define MAX_STUDENTS 200
#define MAX_SESSIONS 50
#define NAME_LEN     64

typedef struct {
    char title[NAME_LEN];
    int  present[MAX_STUDENTS];   /* 1 = present, 0 = absent */
    int  presentCount;
    int  absentCount;
} Session;

/* ---------- safe input helpers ---------- */

static void readLine(const char *prompt, char *buf, int size) {
    printf("%s", prompt);
    if (fgets(buf, size, stdin) == NULL) {
        buf[0] = '\0';
        return;
    }
    buf[strcspn(buf, "\r\n")] = '\0';
}

static int readInt(const char *prompt, int min, int max) {
    char line[64];
    int value;
    for (;;) {
        readLine(prompt, line, sizeof line);
        if (sscanf(line, "%d", &value) == 1 && value >= min && value <= max)
            return value;
        printf("  Please enter a number between %d and %d.\n", min, max);
    }
}

static int readPresence(const char *name) {
    char line[16];
    for (;;) {
        printf("  %-30s (P = present, A = absent): ", name);
        if (fgets(line, sizeof line, stdin) == NULL) return 0;
        char c = (char)toupper((unsigned char)line[0]);
        if (c == 'P') return 1;
        if (c == 'A') return 0;
        printf("  Invalid input, type P or A.\n");
    }
}

/* ---------- report printing (works for screen and file) ---------- */

static void printSessionReport(FILE *out, const Session *s,
                               char names[][NAME_LEN], int n, int index) {
    int listAbsent = (s->presentCount > s->absentCount) || (s->presentCount == s->absentCount);
    int wanted     = listAbsent ? 0 : 1;   /* status of students to list */
    int count      = 0;

    fprintf(out, "\n==== Session %d: %s ====\n", index + 1, s->title);
    fprintf(out, "Present: %d | Absent: %d | Total: %d\n",
            s->presentCount, s->absentCount, n);
    fprintf(out, "Listing %s students (smaller group):\n",
            listAbsent ? "ABSENT" : "PRESENT");

    for (int i = 0; i < n; i++) {
        if (s->present[i] == wanted) {
            fprintf(out, "  %d. %s\n", ++count, names[i]);
        }
    }
    if (count == 0)
        fprintf(out, "  (nobody)\n");
}

int main(void) {
    static char names[MAX_STUDENTS][NAME_LEN];
    static Session sessions[MAX_SESSIONS];

    printf("=== SMART ATTENDANCE ASSISTANT ===\n\n");

    int n = readInt("Number of students in the class: ", 1, MAX_STUDENTS);
    for (int i = 0; i < n; i++) {
        char prompt[40];
        snprintf(prompt, sizeof prompt, "  Name of student %d: ", i + 1);
        readLine(prompt, names[i], NAME_LEN);
        if (names[i][0] == '\0')
            snprintf(names[i], NAME_LEN, "Student %d", i + 1);
    }

    int m = readInt("\nNumber of lecture sessions: ", 1, MAX_SESSIONS);

    for (int k = 0; k < m; k++) {
        Session *s = &sessions[k];
        char prompt[48];

        snprintf(prompt, sizeof prompt, "\nTitle/date of session %d: ", k + 1);
        readLine(prompt, s->title, NAME_LEN);
        if (s->title[0] == '\0')
            snprintf(s->title, NAME_LEN, "Lecture %d", k + 1);

        s->presentCount = s->absentCount = 0;
        for (int i = 0; i < n; i++) {
            s->present[i] = readPresence(names[i]);
            if (s->present[i]) s->presentCount++;
            else               s->absentCount++;
        }
    }

    /* Show on screen */
    printf("\n\n########## ATTENDANCE REPORT ##########");
    for (int k = 0; k < m; k++)
        printSessionReport(stdout, &sessions[k], names, n, k);

    /* Save to file */
    FILE *f = fopen("attendance_report.txt", "w");
    if (f) {
        fprintf(f, "ATTENDANCE REPORT\n");
        for (int k = 0; k < m; k++)
            printSessionReport(f, &sessions[k], names, n, k);
        fclose(f);
        printf("\nReport saved to attendance_report.txt\n");
    } else {
        printf("\nCould not save the report file.\n");
    }

    return 0;
}