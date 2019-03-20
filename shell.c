#include <unistd.h>
#include <stdlib.h>
#include <stdio.h>
#include <errno.h>
#include <string.h>
#include <fcntl.h>
#include <sys/stat.h>
#include <sys/types.h>
#include <sys/wait.h>
#include <signal.h>
#include <semaphore.h>

sem_t* mainBusy;

typedef enum processStatus {
    running, stopped, done, terminated
} processStatus;

typedef struct processRecord {
    char* command;
    pid_t pid;
    int exitCode;
    processStatus status;
    int background;

    struct processRecord* prev;
    struct processRecord* next;
} processRecord;

processRecord* procs = NULL;
processRecord* currentProc = NULL;
unsigned int nextJobNum = 1;

typedef struct statusUpdateItem {
    processRecord* proc;
    int exitCode;
    processStatus s;
    int backgr;
    unsigned int jobNum;

    struct statusUpdateItem* next;
} statusUpdateItem;

statusUpdateItem* statusBuffer = NULL;
statusUpdateItem* statusBufferTail = NULL;

void _printProcInfo(processRecord* p, unsigned int jobNum, int backgr, processStatus s, int exitCode) {
    char status[] = "Terminated";

    if (s == running) strcpy(status, "Running");
    else if (s == stopped) strcpy(status, "Stopped");
    else if (s == done) strcpy(status, "Done");

    printf("[%d] PID=%d\t%s", jobNum, p->pid, status);
    if (s != running) printf(" (status %d)", exitCode);
    printf("\t%s", p->command);
    if (backgr) printf(" &");
    printf("\n");
}

void flushStatusBuffer() {
    statusUpdateItem* prev;

    statusBufferTail = NULL;

    while (statusBuffer != NULL) {
        _printProcInfo(statusBuffer->proc, statusBuffer->jobNum, statusBuffer->backgr, statusBuffer->s, statusBuffer->exitCode);
        if (statusBuffer->proc->status == done || statusBuffer->proc->status == terminated) {
            if (statusBuffer->proc->next != NULL) {
                statusBuffer->proc->next->prev = statusBuffer->proc->prev;
            } else procs->prev = statusBuffer->proc->prev;
            if (statusBuffer->proc == procs) {
                procs = procs->next;
            } else statusBuffer->proc->prev->next = statusBuffer->proc->next;

            char* semname = (char*) malloc(33);
            if (!semname) {
                fprintf(stderr, "FATAL ERROR (MEMORY): Allocation failed. Terminating...\n");
                exit(2);
            }
            if (sprintf(semname, "/seashell10_%d", statusBuffer->proc->pid) < 0) {
                fprintf(stderr, "FATAL ERROR (UNKNOWN): Unable to create the name for the semaphore while deleting a process. Terminating...\n");
                exit(3);
            }

            if (sem_unlink(semname) != 0) {
                fprintf(stderr, "\nFATAL ERROR (UNKNOWN): unable to unlink the semaphore. Terminating...\n");
                exit(3);
            }

            free(semname);
            free(statusBuffer->proc->command);
            free(statusBuffer->proc);
            nextJobNum--;
        }
        prev = statusBuffer;
        statusBuffer = statusBuffer->next;
        free(prev);
    }
}

void pushStatusBuffer(processRecord* p, unsigned int jobN) {
    statusUpdateItem* newI = (statusUpdateItem*) malloc(sizeof(statusUpdateItem));
    if (!newI) {
        fprintf(stderr, "FATAL ERROR (MEMORY): Allocation failed. Terminating...\n");
        exit(2);
    }

    newI->proc = p;
    newI->jobNum = jobN;
    newI->exitCode = p->exitCode;
    newI->backgr = p->background;
    newI->s = p->status;
    newI->next = NULL;

    if (statusBuffer == NULL) {
        statusBuffer = newI;
    }
    else {
        statusBufferTail->next = newI;
    }

    statusBufferTail = newI;
}

void updateStatus() {
    int status;
    pid_t proc = waitpid(WAIT_ANY, &status, WNOHANG | WUNTRACED);

    while (1) {
        while (proc != 0) {
            if (proc < 0) {
                if (errno == ECHILD) break;
                fprintf(stderr, "\nFATAL ERROR (UNKNOWN): waitpid() system call failed. Terminating...\n");
                exit(3);
            }

            processRecord* i = procs;
            unsigned int jobNum = 1;
            while (i != NULL && i->pid != proc) {
                jobNum++;
                i = i->next;
            }
            if (i == NULL) {
                fprintf(stderr, "\nFATAL ERROR (UNKNOWN): waitpid() system call failed: returned PID is not a child. Terminating...\n");
                exit(3);
            }
            i->exitCode = status;
            if (WIFEXITED(status)) i->status = done;
            else if (WIFSIGNALED(status)) i->status = terminated;
            else if (WCOREDUMP(status)) i->status = terminated;
            else if (WIFSTOPPED(status)) i->status = stopped;

            pushStatusBuffer(i, jobNum);

            if (!i->background) {
                if (currentProc != i) {
                    fprintf(stderr, "\nFATAL ERROR (UNKNOWN): job table is corrupt\n");
                    exit(4);
                }
                else if (i->status != running) {
                    currentProc = NULL;
                }
            }

            proc = waitpid(WAIT_ANY, &status, WNOHANG | WUNTRACED);
        }

        if (currentProc == NULL) break;
        proc = waitpid(WAIT_ANY, &status, WUNTRACED);
    }
}

void newProcess(char* command, char** args, int background) {
    signal(SIGINT, SIG_IGN);
    signal(SIGQUIT, SIG_IGN);
    signal(SIGTSTP, SIG_IGN);
    signal(SIGTTIN, SIG_IGN);
    signal(SIGTTOU, SIG_IGN);

    processRecord* newP = (processRecord*) malloc(sizeof(processRecord));
    if (!newP) {
        fprintf(stderr, "FATAL ERROR (MEMORY): Allocation failed. Terminating...\n");
        exit(2);
    }

    newP->command = (char*) malloc(strlen(command) + 1);
    if (!newP->command) {
        fprintf(stderr, "FATAL ERROR (MEMORY): Allocation failed. Terminating...\n");
        exit(2);
    }

    strcpy(newP->command, command);
    newP->exitCode = 0;
    newP->status = running;
    newP->background = background;

    newP->prev = newP;
    newP->next = NULL;

    if (procs != NULL) {
        newP->prev = procs->prev;
        procs->prev->next = newP;
        procs->prev = newP;
    }
    else {
        procs = newP;
    }

    int jobNum = nextJobNum;
    nextJobNum++;

    pid_t childPid = fork();

    if (childPid < 0) { // An error occured
        if (errno == EAGAIN) fprintf(stderr, "Couldn't create the process: process limit exceeded.\n");
        else if (errno == ENOMEM) fprintf(stderr, "Not enough memory to create the process.\n");
        else {
            fprintf(stderr, "\nFATAL ERROR (UNKNOWN): fork() system call failed. Terminating...\n");
            exit(3);
        }
    }
    else if (childPid == 0) {   // We're in the child process now
        signal(SIGINT, SIG_DFL);
        signal(SIGQUIT, SIG_DFL);
        signal(SIGTSTP, SIG_DFL);
        signal(SIGTTIN, SIG_DFL);
        signal(SIGTTOU, SIG_DFL);

        char* semname = (char*) malloc(33);
        if (!semname) {
            fprintf(stderr, "FATAL ERROR (MEMORY): Allocation failed. Terminating...\n");
            exit(2);
        }

        if (sprintf(semname, "/seashell10_%d", getpid()) < 0) {
            fprintf(stderr, "FATAL ERROR (UNKNOWN): Unable to create the name for the semaphore (child). Terminating...\n");
            exit(3);
        }

        if ((mainBusy = sem_open(semname, O_CREAT, S_IRWXU, 0)) == SEM_FAILED) {
            fprintf(stderr, "\nFATAL ERROR (UNKNOWN): unable to initiate a semaphore (child). Terminating...\n");
            exit(3);
        }

        sem_wait(mainBusy);

        if (sem_close(mainBusy) != 0) {
            fprintf(stderr, "\nFATAL ERROR (UNKNOWN): unable to close the semaphore (child). Terminating...\n");
            exit(3);
        }
        free(semname);

        // Execute the command
        execv(command, args);
        _exit(-1);  // If execv() failed, exit immediately
    }
    else if (childPid > 0) {    // We're in the parent process
        char* semname = (char*) malloc(33);
        if (!semname) {
            fprintf(stderr, "FATAL ERROR (MEMORY): Allocation failed. Terminating...\n");
            exit(2);
        }

        if (sprintf(semname, "/seashell10_%d", childPid) < 0) {
            fprintf(stderr, "FATAL ERROR (UNKNOWN): Unable to create the name for the semaphore (parent). Terminating...\n");
            exit(3);
        }

        if ((mainBusy = sem_open(semname, O_CREAT, S_IRWXU, 0)) == SEM_FAILED) {
            fprintf(stderr, "\nFATAL ERROR (UNKNOWN): unable to initiate a semaphore (parent). Terminating...\n");
            exit(3);
        }

        newP->pid = childPid;
        _printProcInfo(newP, jobNum, background, running, 0);

        if (setpgid (childPid, childPid) < 0) {
            fprintf(stderr, "\nFATAL ERROR (UNKNOWN): unable to set the PGID for the child. Terminating...\n");
            exit(3);
        }

        if (!background) {
            if (tcsetpgrp(STDIN_FILENO, childPid) < 0) {
                fprintf(stderr, "\nFATAL ERROR (UNKNOWN): unable to set the terminal foreground PGID. Terminating...\n");
                exit(3);
            }
        }

        sem_post(mainBusy);

        if (sem_close(mainBusy) != 0) {
            fprintf(stderr, "\nFATAL ERROR (UNKNOWN): unable to close the semaphore (parent). Terminating...\n");
            exit(3);
        }
        free(semname);

        if (!background) {
            currentProc = newP;
            updateStatus();
            if (tcsetpgrp(STDIN_FILENO, getpid()) < 0) {
                fprintf(stderr, "\nFATAL ERROR (UNKNOWN): unable to set the terminal foreground PGID. Terminating...\n");
                exit(3);
            }
        }
    }

    signal(SIGINT, SIG_DFL);
    signal(SIGQUIT, SIG_DFL);
    signal(SIGTSTP, SIG_DFL);
    signal(SIGTTIN, SIG_DFL);
    signal(SIGTTOU, SIG_DFL);
}

void resumeProcess(unsigned int jN, int backgr) {
    signal(SIGINT, SIG_IGN);
    signal(SIGQUIT, SIG_IGN);
    signal(SIGTSTP, SIG_IGN);
    signal(SIGTTIN, SIG_IGN);
    signal(SIGTTOU, SIG_IGN);

    processRecord* p = procs;
    unsigned int i;
    for (i = 1; i < jN; i++) p = p->next;

    if (p->background == backgr && p->status == running) {
        printf("Nothing to do.\n");
        return;
    }
    
    if (p->status == running) printf("Attempt to bring to ");
    else printf("Attempt to resume in ");

    if (backgr) printf("background: ");
    else printf("foreground: ");

    p->background = backgr;

    _printProcInfo(p, jN, backgr, running, 0);

    if (p->status != running)
        if (kill(p->pid, SIGCONT) != 0) {
            fprintf(stderr, "Unable to resume process: kill() system call failed.\n");
            return;
        }

    if (!backgr) {
        if (tcsetpgrp(STDIN_FILENO, p->pid) < 0) {
            fprintf(stderr, "\nFATAL ERROR (UNKNOWN): unable to set the terminal foreground PGID. Terminating...\n");
            exit(3);
        }
        currentProc = p;
        updateStatus();
        if (tcsetpgrp(STDIN_FILENO, getpid()) < 0) {
            fprintf(stderr, "\nFATAL ERROR (UNKNOWN): unable to set the terminal foreground PGID. Terminating...\n");
            exit(3);
        }
    }

    signal(SIGINT, SIG_DFL);
    signal(SIGQUIT, SIG_DFL);
    signal(SIGTSTP, SIG_DFL);
    signal(SIGTTIN, SIG_DFL);
    signal(SIGTTOU, SIG_DFL);
}

char* getdir() {
    unsigned long size = 0;
    char* status;
    char* buffer = NULL;

    do {
        size += 256;
        buffer = (char*) malloc(size);
        if (!buffer) {
           fprintf(stderr, "FATAL ERROR (MEMORY): Allocation failed. Terminating...\n");
           exit(2);
        }

        status = getcwd(buffer, size);
        if (status < 0 && errno != ERANGE) {
            free(buffer);
            fprintf(stderr, "FATAL ERROR (UNKNOWN): getcwd() system call failed. Terminating...\n");
            exit(3);
            return NULL;
        }

        if (status >= 0) break;

        free(buffer);
    } while (1);
    return buffer;
}

char** strsplit(char* str, const char* delims) {
    if (strpbrk(delims, "\"\\") != NULL) {
       fprintf(stderr, "FATAL ERROR [strsplit()]: An illegal character used as a delimiter. You cannot use \" and \\ as delimiters. Terminating...\n");
       exit(5);
    }

    unsigned int bufsize = 16, pos = 0;
    char** parts = (char**) malloc(bufsize * sizeof(char*));
    char* temp;
    char* curchar = str;
    unsigned int shift;
    int doublequotes = 0;

    if (!parts) {
       fprintf(stderr, "FATAL ERROR (MEMORY): Allocation failed. Terminating...\n");
       exit(2);
    }

	while (*curchar != '\0') {
		curchar += strspn(curchar, delims);
        temp = curchar;
        shift = 0;
        if (*curchar == '\0') break;

        while (*curchar != '\0') {
            if (*curchar == '\\') {
                curchar++;
                shift++;
                *(curchar - shift) = *curchar;
                if (*curchar != '\0') curchar++;
            }
            else if ((strchr(delims, *curchar) != NULL) && !doublequotes) {
                break;
            }
            else if (*curchar == '"') {
                doublequotes = !doublequotes;
                curchar++;
                shift++;
            }
            else {
                *(curchar - shift) = *curchar;
                curchar++;
            }
        }

        if (*curchar != '\0') {
            *(curchar - shift) = '\0';
            curchar++;
        }
        else *(curchar - shift) = '\0';

        parts[pos] = temp;
        pos++;

        if (pos >= bufsize) {
			bufsize += 16;
			parts = (char**) realloc(parts, bufsize * sizeof(char*));
			if (!parts) {
				fprintf(stderr, "FATAL ERROR (MEMORY): Allocation failed. Terminating...\n");
				exit(2);
			}
		}
	}

	parts[pos] = NULL;
	return parts;
}

int main(int argc, char** argv) {
    char* prompt;
    char* inputstr;
    char* pathvar;
    char* command;
    char** paths;
    char** searchpaths;
    char** args;
    unsigned int i;
    processRecord* p;
    size_t zero = 0;
    ssize_t linelength;
    int bckgr;
    pid_t proc;
    int status;

    if (setpgid (getpid(), getpid()) < 0) {
        fprintf(stderr, "\nFATAL ERROR (UNKNOWN): unable to set the PGID for the main process. Terminating...\n");
        exit(3);
    }

    printf("\nSea Shell v1.0\nCopyright Volodymyr Lapytskyi, 2019\n");
    printf("Developed as the Programming Assignment 1 for the COS 331 \"Operating Systems\" course at AUBG\n");
    printf("Submitted to Professor Anton Stoilov on March 20, 2019\n\n");

    while (1) {
        updateStatus();
        flushStatusBuffer();

        prompt = getdir();
        printf("%s> ", prompt);

        zero = 0;
        inputstr = NULL;
        linelength = getline(&inputstr, &zero, stdin);

        if (linelength < 0) {
            if (feof(stdin)) {
                break;
            }
            else {
                fprintf(stderr, "FATAL ERROR (I/O): Unable to read the command. Terminating...\n");
                exit(1);
            }
        }

		args = strsplit(inputstr, " \t\v\r\n\a");

        i = 0;
        while (args[i] != NULL) i++;
        if (i > 0) {
            i--;
            if (strlen(args[i]) > 0 && (args[i][strlen(args[i]) - 1] == '&')) {
                bckgr = 1;
                if (strlen(args[i]) < 2) {
                    args[i] = NULL;
                }
                else {
                    args[i][strlen(args[i]) - 1] = '\0';
                }
            }
            else {
                bckgr = 0;
            }
        }

        if (args[0] == NULL || strlen(args[0]) < 1) {
            free(inputstr);
            free(args);
            free(prompt);
            continue;
        }

        // Process built-in commands...
        if (strcmp(args[0], "cd") == 0) {
            fprintf(stderr, "cd is a built-in command\n");

            if (args[1] == NULL || strlen(args[1]) < 1) printf("cd: please specify a proper directory.\n");
            else {
                printf("Switching to [%s]...\n", args[1]);
                if (chdir(args[1]) != 0) {
                    if (errno == EACCES) printf("cd: access denied.\n");
                    else if (errno == ENOENT) printf("cd: directory not found.\n");
                    else if (errno == ENOTDIR) printf("cd: the specified path is not a directory.\n");
                    else if (errno == ENAMETOOLONG) printf("cd: the path is too long.\n");
                    else {
                        fprintf(stderr, "\nFATAL ERROR (UNKNOWN): chdir() system call failed. Terminating...\n");
                        exit(3);
                    }
                }
            }
        }
        else if (strcmp(args[0], "exit") == 0) {
            fprintf(stderr, "exit is a built-in command\n");

            free(inputstr);
            free(args);
            free(prompt);
            break;
        }
        else if (strcmp(args[0], "jobs") == 0) {
            fprintf(stderr, "jobs is a built-in command\n");

            printf("%d jobs in total.\n\n", nextJobNum - 1);
            p = procs;
            for (i = 1; p != NULL; i++) {
                _printProcInfo(p, i, p->background, p->status, p->exitCode);
                p = p->next;
            }
        }
        else if (strcmp(args[0], "fg") == 0) {
            fprintf(stderr, "fg is a built-in command\n");

            if (args[1] == NULL || strlen(args[1]) < 1) printf("fg: please specify a proper job number.\n");
            else {
                char* firstNonNumber = args[1];
                unsigned int jN = strtoul(args[1], &firstNonNumber, 0);
                if (jN < 1 || jN >= nextJobNum || *firstNonNumber != '\0') printf("fg: please specify a proper job number.\n");
                else {
                    resumeProcess(jN, 0);
                }
            }
        }
        else if (strcmp(args[0], "bg") == 0) {
            fprintf(stderr, "bg is a built-in command\n");

            if (args[1] == NULL || strlen(args[1]) < 1) printf("bg: please specify a proper job number.\n");
            else {
                char* firstNonNumber = args[1];
                unsigned int jN = strtoul(args[1], &firstNonNumber, 0);
                if (jN < 1 || jN >= nextJobNum || *firstNonNumber != '\0') printf("bg: please specify a proper job number.\n");
                else {
                    resumeProcess(jN, 1);
                }
            }
        }
        else {  // Process external commands
            if (strchr(args[0], '/') == NULL) {
                pathvar = (char*) malloc(strlen(getenv("PATH")) + 1);

                if (!pathvar) {
                    fprintf(stderr, "FATAL ERROR (MEMORY): Allocation failed. Terminating...\n");
                    exit(2);
                }

                strcpy(pathvar, getenv("PATH"));
                paths = strsplit(pathvar, ":");

                i = 0;
                while (paths[i] != NULL) i++;

                searchpaths = (char**) malloc((i + 1) * sizeof(char*));

                if (!searchpaths) {
                    fprintf(stderr, "FATAL ERROR (MEMORY): Allocation failed. Terminating...\n");
                    exit(2);
                }

                i = 0;
                while (paths[i] != NULL) {
                    searchpaths[i] = (char*) malloc(strlen(paths[i]) + 2 + strlen(args[0]));
                    if (!(searchpaths[i])) {
                        fprintf(stderr, "FATAL ERROR (MEMORY): Allocation failed. Terminating...\n");
                        exit(2);
                    }

                    strcpy(searchpaths[i], paths[i]);
                    strcat(searchpaths[i], "/");
                    strcat(searchpaths[i], args[0]);
                    i++;
                }
                searchpaths[i] = NULL;

                free(pathvar);
                free(paths);
            }
            else {
                searchpaths = (char**) malloc(2 * sizeof(char*));

                if (!searchpaths) {
                    fprintf(stderr, "FATAL ERROR (MEMORY): Allocation failed. Terminating...\n");
                    exit(2);
                }

                searchpaths[0] = (char*) malloc(strlen(args[0]) + 1);
                if (!searchpaths[0]) {
                    fprintf(stderr, "FATAL ERROR (MEMORY): Allocation failed. Terminating...\n");
                    exit(2);
                }

                strcpy(searchpaths[0], args[0]);
                searchpaths[1] = NULL;
            }

            command = NULL;
            i = 0;
            while (searchpaths[i] != NULL) {
                fprintf(stderr, "File [%s]: ", searchpaths[i]);
                if (access(searchpaths[i], F_OK) != 0) {
                    if (errno == EACCES) fprintf(stderr, "access denied.\n");
                    else if (errno == ELOOP) fprintf(stderr, "too many symbolic links.\n");
                    else if (errno == ENAMETOOLONG) fprintf(stderr, "the path is too long.\n");
                    else if (errno == ENOENT) fprintf(stderr, "not found.\n");
                    else if (errno == ENOTDIR) fprintf(stderr, "wrong path.\n");
                    else {
                        fprintf(stderr, "\nFATAL ERROR (UNKNOWN): access() system call failed. Terminating...\n");
                        exit(3);
                    }
                    i++;
                    continue;
                } else {
                    fprintf(stderr, "exists; ");
                    if (access(searchpaths[i], X_OK) != 0) {
                        if (errno == EFAULT || errno == EINVAL || errno == EIO || errno == ENOMEM || errno == ETXTBSY) {
                            fprintf(stderr, "\nFATAL ERROR (UNKNOWN): access() system call failed. Terminating...\n");
                            exit(3);
                        } else {
                            fprintf(stderr, "cannot be executed.\n");
                            i++;
                            continue;
                        }
                    } else {
                        fprintf(stderr, "executable.\n");
                        command = searchpaths[i];
                        break;
                    }
                }
                i++;
            }

            if (command == NULL) {
                printf("[%s]: not a command\n", args[0]);
            } else {
                fprintf(stderr, "Executing [%s]...\n", command);

                // Executing the command
                newProcess(command, args, bckgr);
            }

            i = 0;
            while (searchpaths[i] != NULL) {
                free(searchpaths[i]);
                i++;
            }
            free(searchpaths);
        }

        free(inputstr);
        free(args);
        free(prompt);
    }

    updateStatus();
    flushStatusBuffer();

    printf("\nBye.\n");
    return 0;
}
