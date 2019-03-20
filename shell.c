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

void printAbout() {
    printf("\nSea Shell v1.0\nCopyright Volodymyr Lapytskyi, 2019\n");
    printf("Developed as the Programming Assignment 1 for the\nCOS 331 \"Operating Systems\" course at AUBG\n");
    printf("Submitted to Professor Anton Stoilov on March 21, 2019\n\n");
    printf("Type \"help\" for the help message.\n\n");

    return;
}

void printHelp() {
    printAbout();

    printf("The command prompt displays the current working directory.\n\n");

    printf("In order to execute a command, just type it, followed by a space and the relevant arguments,\n%s",
        "also separated with spaces, in the command line, just as you would do with any other command interpreter.\n\n");

    printf("Built-in commands:\n");

    printf("%-5s    %s\n", "bg", "Resume a job in the background;");
    printf("%-5s    %s\n", "", "supply the job number in the first argument.");
    printf("%-5s    %s\n", "", "See also the \"Job Control\" section of this help message.\n");

    printf("%-5s    %s\n", "cd", "Change working directory; the path to the new working");
    printf("%-5s    %s\n", "", "directory should be supplied as the first argument.\n");

    printf("%-5s    %s\n", "exit", "Exit the shell. Instead, you can press Ctrl-D.\n");

    printf("%-5s    %5s\n", "fg", "Resume a job in/bring a job to the foreground;");
    printf("%-5s    %s\n", "", "supply the job number in the first argument.");
    printf("%-5s    %s\n", "", "See also the \"Job Control\" section of this help message.\n");

    printf("%-5s    %s\n", "help", "Display this help message.\n");

    printf("%-5s    %s\n", "jobs", "Display all the jobs currently controlled by this instance of Sea Shell.");
    printf("%-5s    %s\n", "", "See also the \"Job Control\" section of this help message.\n");

    printf("All other commands are treated as external, thus the name of the command\n%s",
        "is to be treated as the path to an executable.\n\n");

    printf("If an external command contains the '/' character, the command will be seen as\n%s%s%s%s",
        "an explicit absolute/relative path to the executable, so the file will be searched for\n",
        "in the respective location relative to the current working directory.\n",
        "If the command does NOT include a forward slash, the respective file will be searched for\n",
        "in all directories specified in the PATH environment variable.\n\n");

    printf("If a command/an argument, or a part of one, is enclosed in double quotes (\"),\n%s%s%s",
        "any whitespaces contained in it will NOT be considered as the delimiters\n",
        "(i.e. an argument with a space will not be considered as two arguments). In such case,\n",
        "the double quotes will be removed from the argument when it is passed to the command.\n\n");
    printf("Also, any character in your command line can be escaped with the '\\' character,\n%s%s%s%s",
        "so that it will be treated \"as is\" (e.g. an escaped whitespace will be included\n",
        "in the parameter, without being counted as the delimiter; escaped double quotes\n",
        "will also appear in the parameter and will not be seen as double quotes).\n",
        "'\\' character can be escaped itself (i.e. '\\\\' results in '\\').\n\n");
    
    printf("Examples:\n\n");

    printf("%-20s    %s\n", "mkdir foo bar", "2 folders created: foo and bar;");
    printf("%-20s    %s\n", "mkdir \"foo bar\"", "folder 'foo bar' created;");
    printf("%-20s    %s\n", "mkdir fo\"o b\"ar", "folder 'foo bar' created;");
    printf("%-20s    %s\n", "mkdir foo\\ bar", "folder 'foo bar' created;");
    printf("%-20s    %s\n", "mkdir foo\\\\ bar", "2 folders created: foo\\ and bar;");
    printf("%-20s    %s\n", "mkdir \"\\foo\\ b\\ar\"", "folder 'foo bar' created;");
    printf("%-20s    %s\n", "mkdir fo\\\"o b\\\"ar", "2 folders created: fo\"o and b\"ar.");

    printf("\n\n    ==== Job Control ====\n\n");

    printf("Sea Shell v1.0 can keep track of several jobs simultaneously, each of\n%s%s",
        "them being either stopped (suspended), running in the background or running in\n",
        "the foreground (obviously, there can be no more than 1 job in the foreground).\n\n");

    printf("The following actions are available:\n\n");

    printf("%-42s    %s\n", "Display the list of all", "");
    printf("%-42s    %s\n", "    suspended & background jobs", "Type \"jobs\"\n");
    printf("%-42s    %s\n", "Run a command in the background", "Append the command line");
    printf("%-42s    %s\n", "", "    with the '&' sign\n");
    printf("%-42s    %s\n", "Terminate current foreground process", "");
    printf("%-42s    %s\n", "    and return to the shell", "Press Ctrl-C\n");
    printf("%-42s    %s\n", "Suspend current foreground process", "");
    printf("%-42s    %s\n", "    and return to the shell", "Press Ctrl-Z\n");
    printf("%-42s    %s\n", "Continue a suspended job in the background", "Type \"bg job_number\"\n");
    printf("%-42s    %s\n", "Continue a suspended job in the foreground", "Type \"fg job_number\"\n");
    printf("%-42s    %s\n", "Bring a background job to the foreground", "Type \"fg job_number\"\n");

    printf("\nEvery time before the shell prints the command prompt, it will notify you if\n%s%s",
        "the execution status of any job has changed since the previous prompt was displayed.\n",
        "After every such update, all the jobs that have been marked as 'Done' are forgotten.\n\n");

    printf("Both the \"jobs\" command and the real-time status updates share the same\n%s",
        "format of the process record information:\n\n");

    printf("1) [job_number]\n%s%s%s%s%s",
        "2) PID=process_ID\n",
        "3) execution_status\n",
        "4) If process is not running: (status last_exit_code)\n",
        "5) Path to the executable\n",
        "6) If the process is in background: &\n");

    printf("\n\n    ==== Author Information ====\n\n");
    printf("I am Volodymyr Lapytskyi, a sophomore student at the\n%s%s%s",
        "American University in Bulgaria, majoring in\n",
        "Computer Science and Economics.\n",
        "Email: VNL170@aubg.edu\n\n");

    printf("The source code for this program can be found on GitHub at\nhttps://github.com/vlapytskyi-aubg/COS331-PA1_SeaShell\n\n");

    return;
}

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

    printAbout();

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
        if (strcmp(args[0], "help") == 0) {
            fprintf(stderr, "help is a built-in command\n");

            printHelp();
        }
        else if (strcmp(args[0], "cd") == 0) {
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
