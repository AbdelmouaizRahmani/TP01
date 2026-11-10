/*
 * Smart Attendance Assistant - Raspberry Pi edition (pure C)
 *
 * Two modes:
 *
 *   ./smart_attendance admin   Administration console. Add / edit / delete lectures
 *                              (date, time, module, teacher) and manage the student
 *                              list (8-digit IDs). Can be used at any time, even while
 *                              the service is running: the service reloads the files
 *                              automatically when they change.
 *
 *   ./smart_attendance run     Attendance service (leave it running on the Pi).
 *                              - Reads student IDs sent by the face-scanning device
 *                                (serial port, USB, or a FIFO/file path from config.txt).
 *                              - Finds the lecture in progress from the schedule.
 *                              - When the lecture ends it builds the list of ABSENT
 *                                students only (enrolled but never scanned) and sends it
 *                                to the administration. If sending fails, it is kept and
 *                                retried every minute until it succeeds.
 *
 * Files (all in the working directory):
 *   config.txt     scanner=/dev/ttyUSB0
 *                  admin_url=https://admin.example.edu/api/attendance
 *   students.txt   one 8-digit student ID per line
 *   schedule.txt   id|YYYY-MM-DD|HH:MM|HH:MM|module|teacher
 *   reports/       report_<lectureId>_<date>.txt  (+ ".sent" marker once delivered)
 *
 * The scanner must send one ID per line, e.g. "20241234\n" (\r\n also accepted).
 *
 * Compile: gcc -Wall -Wextra -o smart_attendance smart_attendance.c
 * Needs:   the "curl" command on the Pi (sudo apt install curl) to send reports.
 */

#define _POSIX_C_SOURCE 200809L
#define _DEFAULT_SOURCE

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <ctype.h>
#include <time.h>
#include <errno.h>
#include <signal.h>
#include <unistd.h>
#include <fcntl.h>
#include <termios.h>
#include <dirent.h>
#include <sys/select.h>
#include <sys/stat.h>
#include <sys/wait.h>

#define MAX_STUDENTS  2000
#define MAX_LECTURES  500
#define NAME_LEN      64
#define ID_LEN        8

#define STUDENTS_FILE "students.txt"
#define SCHEDULE_FILE "schedule.txt"
#define CONFIG_FILE   "config.txt"
#define REPORT_DIR    "reports"

typedef struct {
    int    id;
    char   date[11];          /* YYYY-MM-DD */
    char   start[6];          /* HH:MM */
    char   end[6];            /* HH:MM */
    char   module[NAME_LEN];
    char   teacher[NAME_LEN];
    time_t startT, endT;
} Lecture;

static char    roster[MAX_STUDENTS][ID_LEN + 1];
static int     rosterN = 0;
static Lecture lectures[MAX_LECTURES];
static int     lectureN = 0;

static char cfgScanner[256] = "/dev/ttyUSB0";
static char cfgUrl[512]     = "";

static volatile sig_atomic_t stopFlag = 0;

/* =====================================================================
 *  Generic helpers
 * ===================================================================== */

static void readLine(const char *prompt, char *buf, int size) {
    printf("%s", prompt);
    fflush(stdout);
    if (fgets(buf, size, stdin) == NULL) { buf[0] = '\0'; return; }
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

static int isValidId(const char *s) {
    if (strlen(s) != ID_LEN) return 0;
    for (int i = 0; i < ID_LEN; i++)
        if (!isdigit((unsigned char)s[i])) return 0;
    return 1;
}

static void trim(char *s) {
    size_t n = strlen(s), i = 0;
    while (n > 0 && isspace((unsigned char)s[n - 1])) s[--n] = '\0';
    while (s[i] && isspace((unsigned char)s[i])) i++;
    if (i) memmove(s, s + i, strlen(s + i) + 1);
}

static void sanitizeText(char *s) {          /* '|' is our field separator */
    for (; *s; s++) if (*s == '|' || *s == '\n' || *s == '\r') *s = '-';
}

static void timestamp(char *out, size_t size) {
    time_t t = time(NULL);
    strftime(out, size, "%Y-%m-%d %H:%M:%S", localtime(&t));
}

static void logMsg(const char *fmt, ...) __attribute__((format(printf, 1, 2)));
#include <stdarg.h>
static void logMsg(const char *fmt, ...) {
    char ts[32];
    va_list ap;
    timestamp(ts, sizeof ts);
    printf("[%s] ", ts);
    va_start(ap, fmt);
    vprintf(fmt, ap);
    va_end(ap);
    printf("\n");
    fflush(stdout);
}

/* ---------- date / time validation ---------- */

static int vDate(const char *s) {
    int y, m, d;
    struct tm tm;
    if (strlen(s) != 10 || sscanf(s, "%4d-%2d-%2d", &y, &m, &d) != 3) return 0;
    memset(&tm, 0, sizeof tm);
    tm.tm_year = y - 1900; tm.tm_mon = m - 1; tm.tm_mday = d; tm.tm_hour = 12;
    if (mktime(&tm) == (time_t)-1) return 0;
    return tm.tm_year == y - 1900 && tm.tm_mon == m - 1 && tm.tm_mday == d;
}

static int vTime(const char *s) {
    int h, m;
    if (strlen(s) != 5 || s[2] != ':' || sscanf(s, "%2d:%2d", &h, &m) != 2) return 0;
    return h >= 0 && h < 24 && m >= 0 && m < 60;
}

static int vText(const char *s) { return s[0] != '\0'; }

static time_t toTime(const char *date, const char *hhmm) {
    int y, mo, d, h, mi;
    struct tm tm;
    sscanf(date, "%4d-%2d-%2d", &y, &mo, &d);
    sscanf(hhmm, "%2d:%2d", &h, &mi);
    memset(&tm, 0, sizeof tm);
    tm.tm_year = y - 1900; tm.tm_mon = mo - 1; tm.tm_mday = d;
    tm.tm_hour = h; tm.tm_min = mi; tm.tm_isdst = -1;
    return mktime(&tm);
}

/* returns 1 if the lecture times are consistent (end after start) */
static int computeTimes(Lecture *l) {
    l->startT = toTime(l->date, l->start);
    l->endT   = toTime(l->date, l->end);
    return l->endT > l->startT;
}

/* =====================================================================
 *  Config
 * ===================================================================== */

static void loadConfig(void) {
    FILE *f = fopen(CONFIG_FILE, "r");
    char line[600];
    if (!f) return;
    while (fgets(line, sizeof line, f)) {
        char *eq = strchr(line, '=');
        if (!eq || line[0] == '#') continue;
        *eq = '\0';
        char *key = line, *val = eq + 1;
        trim(key); trim(val);
        if (strcmp(key, "scanner") == 0)
            snprintf(cfgScanner, sizeof cfgScanner, "%s", val);
        else if (strcmp(key, "admin_url") == 0)
            snprintf(cfgUrl, sizeof cfgUrl, "%s", val);
    }
    fclose(f);
}

/* =====================================================================
 *  Student roster (8-digit IDs)
 * ===================================================================== */

static int rosterFind(const char *id) {
    for (int i = 0; i < rosterN; i++)
        if (strcmp(roster[i], id) == 0) return i;
    return -1;
}

static void loadRoster(void) {
    FILE *f = fopen(STUDENTS_FILE, "r");
    char line[64];
    rosterN = 0;
    if (!f) return;
    while (fgets(line, sizeof line, f) && rosterN < MAX_STUDENTS) {
        trim(line);
        if (isValidId(line) && rosterFind(line) < 0)
            strcpy(roster[rosterN++], line);
    }
    fclose(f);
}

static int saveRoster(void) {
    FILE *f = fopen(STUDENTS_FILE ".tmp", "w");
    if (!f) return 0;
    for (int i = 0; i < rosterN; i++) fprintf(f, "%s\n", roster[i]);
    if (fclose(f) != 0) return 0;
    return rename(STUDENTS_FILE ".tmp", STUDENTS_FILE) == 0;
}

/* =====================================================================
 *  Lecture schedule
 * ===================================================================== */

static int cmpLecture(const void *a, const void *b) {
    const Lecture *x = a, *y = b;
    return (x->startT > y->startT) - (x->startT < y->startT);
}

static void loadSchedule(void) {
    FILE *f = fopen(SCHEDULE_FILE, "r");
    char line[400];
    lectureN = 0;
    if (!f) return;
    while (fgets(line, sizeof line, f) && lectureN < MAX_LECTURES) {
        Lecture l;
        memset(&l, 0, sizeof l);
        if (sscanf(line, "%d|%10[^|]|%5[^|]|%5[^|]|%63[^|]|%63[^\r\n]",
                   &l.id, l.date, l.start, l.end, l.module, l.teacher) != 6)
            continue;
        if (!vDate(l.date) || !vTime(l.start) || !vTime(l.end) || !computeTimes(&l))
            continue;
        lectures[lectureN++] = l;
    }
    fclose(f);
    qsort(lectures, lectureN, sizeof lectures[0], cmpLecture);
}

static int saveSchedule(void) {
    FILE *f = fopen(SCHEDULE_FILE ".tmp", "w");
    if (!f) return 0;
    for (int i = 0; i < lectureN; i++)
        fprintf(f, "%d|%s|%s|%s|%s|%s\n", lectures[i].id, lectures[i].date,
                lectures[i].start, lectures[i].end, lectures[i].module,
                lectures[i].teacher);
    if (fclose(f) != 0) return 0;
    return rename(SCHEDULE_FILE ".tmp", SCHEDULE_FILE) == 0;   /* atomic */
}

static int lectureIndexById(int id) {
    for (int i = 0; i < lectureN; i++)
        if (lectures[i].id == id) return i;
    return -1;
}

static int nextLectureId(void) {
    int max = 0;
    for (int i = 0; i < lectureN; i++)
        if (lectures[i].id > max) max = lectures[i].id;
    return max + 1;
}

/* only one scanner => two lectures may not overlap */
static int overlaps(const Lecture *l, int ignoreId) {
    for (int i = 0; i < lectureN; i++) {
        if (lectures[i].id == ignoreId) continue;
        if (l->startT < lectures[i].endT && lectures[i].startT < l->endT) return 1;
    }
    return 0;
}

/* =====================================================================
 *  Administration console
 * ===================================================================== */

static void askField(const char *label, const char *current, char *out, int size,
                     int (*valid)(const char *), int keepAllowed) {
    char buf[128], prompt[200];
    for (;;) {
        if (keepAllowed) snprintf(prompt, sizeof prompt, "  %s [%s]: ", label, current);
        else             snprintf(prompt, sizeof prompt, "  %s: ", label);
        readLine(prompt, buf, sizeof buf);
        trim(buf);
        if (keepAllowed && buf[0] == '\0') { snprintf(out, size, "%s", current); return; }
        sanitizeText(buf);
        if (valid(buf)) { snprintf(out, size, "%s", buf); return; }
        printf("  Invalid value, try again.\n");
    }
}

/* editing = 1: blank answer keeps the current value */
static int promptLecture(Lecture *l, int editing) {
    Lecture n = *l;
    askField("Date (YYYY-MM-DD)", l->date,    n.date,    sizeof n.date,    vDate, editing);
    askField("Start time (HH:MM)", l->start,  n.start,   sizeof n.start,   vTime, editing);
    askField("End time (HH:MM)",   l->end,    n.end,     sizeof n.end,     vTime, editing);
    askField("Module",             l->module, n.module,  sizeof n.module,  vText, editing);
    askField("Teacher name",       l->teacher,n.teacher, sizeof n.teacher, vText, editing);

    if (!computeTimes(&n)) {
        printf("  The end time must be after the start time. Nothing saved.\n");
        return 0;
    }
    if (overlaps(&n, l->id)) {
        printf("  This lecture overlaps another one. Nothing saved.\n");
        return 0;
    }
    *l = n;
    return 1;
}

static void listLectures(void) {
    if (lectureN == 0) { printf("\n  (no lectures scheduled)\n"); return; }
    printf("\n  %-4s %-10s %-11s %-24s %s\n", "ID", "Date", "Time", "Module", "Teacher");
    for (int i = 0; i < lectureN; i++)
        printf("  %-4d %-10s %s-%s %-24s %s\n", lectures[i].id, lectures[i].date,
               lectures[i].start, lectures[i].end, lectures[i].module,
               lectures[i].teacher);
}

static void adminMode(void) {
    loadSchedule();
    loadRoster();
    printf("=== ADMINISTRATION CONSOLE ===\n");

    for (;;) {
        printf("\n 1. List lectures\n 2. Add lecture\n 3. Edit lecture\n"
               " 4. Delete lecture\n 5. List students\n 6. Add student\n"
               " 7. Remove student\n 0. Quit\n");
        int choice = readInt("Choice: ", 0, 7);

        /* always work on the latest files (the service only reads them) */
        loadSchedule();
        loadRoster();

        if (choice == 0) break;

        if (choice == 1) {
            listLectures();

        } else if (choice == 2) {
            if (lectureN >= MAX_LECTURES) { printf("  Schedule is full.\n"); continue; }
            Lecture l;
            memset(&l, 0, sizeof l);
            l.id = nextLectureId();
            strcpy(l.date, "");
            if (promptLecture(&l, 0)) {
                lectures[lectureN++] = l;
                qsort(lectures, lectureN, sizeof lectures[0], cmpLecture);
                printf(saveSchedule() ? "  Lecture %d added.\n" : "  Could not save (lecture %d).\n", l.id);
            }

        } else if (choice == 3 || choice == 4) {
            listLectures();
            if (lectureN == 0) continue;
            int id = readInt("Lecture ID: ", 1, 1000000);
            int idx = lectureIndexById(id);
            if (idx < 0) { printf("  No such lecture.\n"); continue; }
            if (choice == 3) {
                Lecture l = lectures[idx];
                if (promptLecture(&l, 1)) {
                    lectures[idx] = l;
                    qsort(lectures, lectureN, sizeof lectures[0], cmpLecture);
                    printf(saveSchedule() ? "  Lecture updated.\n" : "  Could not save.\n");
                }
            } else {
                memmove(&lectures[idx], &lectures[idx + 1],
                        (size_t)(lectureN - idx - 1) * sizeof lectures[0]);
                lectureN--;
                printf(saveSchedule() ? "  Lecture deleted.\n" : "  Could not save.\n");
            }

        } else if (choice == 5) {
            printf("\n  %d student(s):\n", rosterN);
            for (int i = 0; i < rosterN; i++) printf("  %s\n", roster[i]);

        } else if (choice == 6) {
            char id[64];
            readLine("  New student ID (8 digits): ", id, sizeof id);
            trim(id);
            if (!isValidId(id))            printf("  An ID must be exactly 8 digits.\n");
            else if (rosterFind(id) >= 0)  printf("  Already registered.\n");
            else if (rosterN >= MAX_STUDENTS) printf("  Student list is full.\n");
            else {
                strcpy(roster[rosterN++], id);
                printf(saveRoster() ? "  Student added.\n" : "  Could not save.\n");
            }

        } else if (choice == 7) {
            char id[64];
            readLine("  Student ID to remove: ", id, sizeof id);
            trim(id);
            int idx = rosterFind(id);
            if (idx < 0) { printf("  Not found.\n"); continue; }
            memmove(roster[idx], roster[idx + 1], (size_t)(rosterN - idx - 1) * (ID_LEN + 1));
            rosterN--;
            printf(saveRoster() ? "  Student removed.\n" : "  Could not save.\n");
        }
    }
}

/* =====================================================================
 *  Reports + delivery to the administration
 * ===================================================================== */

typedef struct {
    int     active;
    Lecture lec;
    unsigned char present[MAX_STUDENTS];   /* parallel to roster[] */
    int     presentCount;
    int     unknownScans;                  /* IDs not in the roster */
} Session;

static void reportPath(const Lecture *l, char *out, size_t size) {
    snprintf(out, size, REPORT_DIR "/report_%d_%s.txt", l->id, l->date);
}

static int fileExists(const char *p) { return access(p, F_OK) == 0; }

/* POST the report to the administration server. Returns 1 on success. */
static int sendReport(const char *path) {
    if (cfgUrl[0] == '\0') {
        logMsg("No admin_url in %s: report kept locally.", CONFIG_FILE);
        return 0;
    }
    char dataArg[400];
    snprintf(dataArg, sizeof dataArg, "@%s", path);

    pid_t pid = fork();
    if (pid < 0) return 0;
    if (pid == 0) {
        execlp("curl", "curl", "-fsS", "--max-time", "30", "-o", "/dev/null",
               "-X", "POST", "-H", "Content-Type: text/plain; charset=utf-8",
               "--data-binary", dataArg, cfgUrl, (char *)NULL);
        _exit(127);
    }
    int status = 0;
    if (waitpid(pid, &status, 0) < 0) return 0;
    return WIFEXITED(status) && WEXITSTATUS(status) == 0;
}

static void markSent(const char *path) {
    char marker[420];
    snprintf(marker, sizeof marker, "%s.sent", path);
    FILE *f = fopen(marker, "w");
    if (f) fclose(f);
}

/* Absent list only: enrolled students who were never detected */
static void finalizeSession(const Session *s) {
    char path[300];
    int absent = rosterN - s->presentCount;
    reportPath(&s->lec, path, sizeof path);

    FILE *f = fopen(path, "w");
    if (!f) { logMsg("Cannot write %s", path); return; }

    fprintf(f, "ATTENDANCE REPORT\n");
    fprintf(f, "Module : %s\n", s->lec.module);
    fprintf(f, "Teacher: %s\n", s->lec.teacher);
    fprintf(f, "Date   : %s  (%s-%s)\n", s->lec.date, s->lec.start, s->lec.end);
    fprintf(f, "Enrolled: %d | Present: %d | Absent: %d\n", rosterN, s->presentCount, absent);
    fprintf(f, "Scans of IDs not in the student list: %d\n", s->unknownScans);
    fprintf(f, "ABSENT STUDENTS (ID):\n");
    int n = 0;
    for (int i = 0; i < rosterN; i++)
        if (!s->present[i]) fprintf(f, "  %d. %s\n", ++n, roster[i]);
    if (n == 0) fprintf(f, "  (nobody)\n");
    fclose(f);

    logMsg("Lecture %d ended: %d present, %d absent. Report %s", s->lec.id,
           s->presentCount, absent, path);

    if (sendReport(path)) { markSent(path); logMsg("Report sent to administration."); }
    else logMsg("Sending failed, will retry every minute.");
}

/* retry every report that has no ".sent" marker */
static void retryUnsent(void) {
    DIR *d = opendir(REPORT_DIR);
    struct dirent *e;
    if (!d) return;
    while ((e = readdir(d)) != NULL) {
        size_t len = strlen(e->d_name);
        if (strncmp(e->d_name, "report_", 7) != 0 || len < 5 ||
            strcmp(e->d_name + len - 4, ".txt") != 0)
            continue;
        char path[400], marker[420];
        snprintf(path, sizeof path, REPORT_DIR "/%s", e->d_name);
        snprintf(marker, sizeof marker, "%s.sent", path);
        if (fileExists(marker)) continue;
        if (sendReport(path)) { markSent(path); logMsg("Pending report %s sent.", e->d_name); }
    }
    closedir(d);
}

/* =====================================================================
 *  Scanner input
 * ===================================================================== */

static int openScanner(void) {
    int fd = open(cfgScanner, O_RDONLY | O_NOCTTY | O_NONBLOCK);
    if (fd < 0) return -1;
    if (isatty(fd)) {
        struct termios t;
        if (tcgetattr(fd, &t) == 0) {
            cfmakeraw(&t);
            cfsetispeed(&t, B115200);
            cfsetospeed(&t, B115200);
            tcsetattr(fd, TCSANOW, &t);
        }
    }
    logMsg("Scanner opened: %s", cfgScanner);
    return fd;
}

static void handleScan(Session *s, const char *id) {
    if (!isValidId(id)) { logMsg("Ignored malformed scan."); return; }
    if (!s->active)     { logMsg("Scan %s ignored: no lecture in progress.", id); return; }
    int idx = rosterFind(id);
    if (idx < 0)        { s->unknownScans++; logMsg("Scan %s: not in student list.", id); return; }
    if (s->present[idx]) return;                      /* already counted */
    s->present[idx] = 1;
    s->presentCount++;
    logMsg("Present: %s (%d/%d)", id, s->presentCount, rosterN);
}

static void onSignal(int sig) { (void)sig; stopFlag = 1; }

/* =====================================================================
 *  Attendance service
 * ===================================================================== */

static void runMode(void) {
    static Session sess;
    time_t lastRetry = 0, lastOpenTry = 0, scheduleMtime = 0;
    char linebuf[64];
    int len = 0, fd = -1;

    loadConfig();
    mkdir(REPORT_DIR, 0755);
    signal(SIGINT, onSignal);
    signal(SIGTERM, onSignal);

    logMsg("Attendance service started (scanner: %s).", cfgScanner);

    while (!stopFlag) {
        time_t now = time(NULL);

        /* pick up schedule updates made by the administration */
        struct stat st;
        time_t mt = (stat(SCHEDULE_FILE, &st) == 0) ? st.st_mtime : 0;
        if (mt != scheduleMtime) {
            scheduleMtime = mt;
            loadSchedule();
            logMsg("Schedule reloaded: %d lecture(s).", lectureN);
        }

        /* lecture finished -> build and send the absent list */
        if (sess.active && now >= sess.lec.endT) {
            finalizeSession(&sess);
            sess.active = 0;
        }

        /* lecture starting -> open a session */
        if (!sess.active) {
            for (int i = 0; i < lectureN; i++) {
                char path[300];
                reportPath(&lectures[i], path, sizeof path);
                if (now >= lectures[i].startT && now < lectures[i].endT && !fileExists(path)) {
                    loadConfig();
                    loadRoster();
                    if (rosterN == 0) { logMsg("Student list is empty, cannot start."); break; }
                    memset(&sess, 0, sizeof sess);
                    sess.active = 1;
                    sess.lec = lectures[i];
                    logMsg("Lecture %d started: %s (%s), %d enrolled.", sess.lec.id,
                           sess.lec.module, sess.lec.teacher, rosterN);
                    break;
                }
            }
        }

        /* scanner connection */
        if (fd < 0 && now - lastOpenTry >= 5) {
            fd = openScanner();
            lastOpenTry = now;
        }

        if (fd >= 0) {
            fd_set rf;
            struct timeval tv = {1, 0};
            FD_ZERO(&rf);
            FD_SET(fd, &rf);
            int r = select(fd + 1, &rf, NULL, NULL, &tv);
            if (r > 0) {
                char chunk[64];
                ssize_t n = read(fd, chunk, sizeof chunk);
                if (n <= 0) {
                    if (n == 0 || (errno != EAGAIN && errno != EINTR)) {
                        logMsg("Scanner disconnected, retrying...");
                        close(fd); fd = -1; len = 0;
                    }
                } else {
                    for (ssize_t i = 0; i < n; i++) {
                        if (chunk[i] == '\n' || chunk[i] == '\r') {
                            if (len > 0) {
                                linebuf[len] = '\0';
                                handleScan(&sess, linebuf);
                                len = 0;
                            }
                        } else if (len < (int)sizeof linebuf - 1) {
                            linebuf[len++] = chunk[i];
                        } else {
                            len = 0;               /* overlong garbage: drop */
                        }
                    }
                }
            }
        } else {
            sleep(1);
        }

        /* retry undelivered reports */
        if (now - lastRetry >= 60) {
            retryUnsent();
            lastRetry = now;
        }
    }
    logMsg("Service stopped.");
}

/* =====================================================================
 *  Entry point
 * ===================================================================== */

int main(int argc, char **argv) {
    if (argc == 2 && strcmp(argv[1], "admin") == 0) { adminMode(); return 0; }
    if (argc == 2 && strcmp(argv[1], "run") == 0)   { runMode();   return 0; }
    fprintf(stderr, "Usage: %s admin | run\n", argv[0]);
    return 1;
}