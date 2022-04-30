#include <stdio.h>
#include <signal.h>
#include <unistd.h>
#include <stdint.h>
#include <string.h>
#include <stdlib.h>
#include <err.h>
#include <errno.h>
#include <sys/wait.h>
#include <fcntl.h>
#include <sys/types.h>

#define MAX_ARGS 513
#define MAX_CMD_LEN 2048
#define MAX_BG_PROCS 20

struct Command {
    char *args[MAX_ARGS];
    int argc;
    char *inputf;
    char *outputf;
    int background;
};

struct Status {
    char *type;
    int val;
};

struct Status status = { .type = "exit value", .val = 0 };

pid_t bg_procs[MAX_BG_PROCS] = {0};
int num_bg_procs = 0;

static volatile sig_atomic_t foreground_only = 0;
static volatile sig_atomic_t process_in_foreground = 0;
static volatile sig_atomic_t TSTP_received = 0;


int get_input(char buf[]);
void parse_input(char buf[], struct Command * command);
void print_struct(struct Command * command);
void change_directory(struct Command * command);
void handle_redirect(struct Command * command);
void reset_SIGINT(struct Command * command);
void ignore_SIGINT();
void ignore_SIGTSTP();
void set_SIGTSTP();
void handle_SIGTSTP();
void add_bg_proc(pid_t childPid);
void cleanup_bg_procs();
void kill_bg_procs();
void fork_and_exec(struct Command * command);
void execute(struct Command * command);



int main (void) {


    set_SIGTSTP();
    ignore_SIGINT();

    char buf[MAX_CMD_LEN];
    struct Command command;

    while (1) {

        // CLEANUP 
        cleanup_bg_procs();

        // GET INPUT

        int n = get_input(buf);
        if (n == 0 || buf[0] == '#') continue; // if user entered nothing, or a comment
        
        // PARSE INPUT
        parse_input(buf, &command);

        // EXECUTE
        execute(&command);
        
    }
}


int get_input(char buf[]){
    int i = 0;
    char c;

    printf(":");
    fflush(stdout);

    // get user input char by char until user hits the Enter key
    while ((c = getc(stdin)) != '\n'){
        //variable expansion
        if (c == '$'){
            // if next char is $, add pid to buffer and continue
            if ((c = getc(stdin)) == '$'){
                i += sprintf(&buf[i], "%jd", (intmax_t) getpid());
                continue;
            // otherwise, add $ to buffer. if $ was the last char entered, break
            } else {
                buf[i++] = '$';
                if (c == '\n') break;
            }
        }

        buf[i++] = c;
    }

    buf[i] = '\0'; // null terminate
    return i; // return length

}




void parse_input(char buf[], struct Command * command){
    // clear struct
    command->argc = 0;
    command->inputf = NULL;
    command->outputf = NULL;
    command->background = 0;

    char* currtok;
    int i = 0;
    command->args[i++] = strtok(buf, " "); // set first argument to the first token (which is the command)

    // continue to parse space-delimited tokens from input
    while ((currtok = strtok(NULL, " ")) != NULL){
        if (strcmp(currtok, "&") == 0){
            char *amp = currtok;
            if ((currtok = strtok(NULL, " ")) == NULL){
                command->background = 1; // if & is last word entered, set background to 1.
                break;
            } else {
                command->args[i++] = amp; // otherwise, add it to args
            }
        } 
        
        if (strcmp(currtok, "<") == 0){
            command->inputf = strtok(NULL, " ");

        } else if (strcmp(currtok, ">") == 0){
            command->outputf = strtok(NULL, " ");

        } else {
            command->args[i++] = currtok;
        }
    }

    command->argc = i;
}

void execute(struct Command * command){

    char *command_name = command->args[0];

    if (strcmp(command_name, "cd") == 0){
        change_directory(command);
        // char cwd[100];
        // getcwd(cwd, 100);
        // printf("Current directory is now: %s\n", cwd);
    } else if (strcmp(command_name, "exit") == 0){
        kill_bg_procs();
        exit(0);
    } else if (strcmp(command_name, "status") == 0){
        printf("%s %i\n", status.type, status.val);
        fflush(stdout);
    } 
    // else if (strcmp(command_name, "print_bg_procs") == 0){
    //     print_bg_procs();

    // } 
    else {
        fork_and_exec(command);
    }
}

void fork_and_exec(struct Command * command){

    if (command->background && num_bg_procs == MAX_BG_PROCS){
        fprintf(stderr, "Too many background processes!");
        fflush(stdout);
        return;
    }

    char **argv = malloc((command->argc+1) * sizeof(char*));

    int i;
    for (i=0; i<command->argc; i++)
        argv[i] = command->args[i];
    argv[i] = NULL;

    // Below code taken from Modules

    int childStatus;
	pid_t childPid = fork();
    alarm(600);

    if(childPid == -1){
        perror("fork() failed!\n");
    } else if (childPid == 0){
        // IN CHILD
        handle_redirect(command);
        reset_SIGINT(command);
        ignore_SIGTSTP();

		execvp(argv[0], argv);
		fprintf(stderr, "%s: %s\n", argv[0], strerror(errno));
        fflush(stdout);
        exit(1);
    } else {
        // IN PARENT
        if (command->background && !foreground_only) {
            // BACKGROUND PROCESSS
            add_bg_proc(childPid);
            printf("background pid is %d\n", childPid);
            fflush(stdout);
        } else {
            // FOREGROUND PROCESS
            process_in_foreground = 1;
            childPid = waitpid(childPid, &childStatus, 0);
            process_in_foreground = 0;

            // if Ctrl-Z was received while process was in foreground, handle now
            char entering[] = "Entering foreground-only mode (& is now ignored)\n";
            char exiting[] = "Exiting foreground-only mode\n";
            if (TSTP_received){
                foreground_only == 1 ? printf("\n%s", entering) : printf("\n%s", exiting);
                fflush(stdout);
                TSTP_received = 0;
            }

            if(WIFEXITED(childStatus)){
                status.type = "exit value";
                status.val = WEXITSTATUS(childStatus);
            } else{
                status.type = "terminated by signal";
                status.val = WTERMSIG(childStatus);
                printf("%s %i\n", status.type, status.val);
                fflush(stdout);
            }
        }
    }

    free(argv);
}




void change_directory(struct Command * command){
    if (command->argc == 1) {
        char *home = getenv("HOME");
        chdir(home);
    } else{
        if (chdir(command->args[1])) {
            fprintf(stderr, "Could not change directory\n");
            fflush(stdout);
        }
    }
}

void handle_redirect(struct Command * command){
    if (command->background){
        if (!command->inputf)
            command->inputf = "/dev/null";
        if (!command->outputf)
            command->outputf = "/dev/null";
    }
    
    if (command->inputf){
        int source = open(command->inputf, O_RDONLY);
        if (source == -1){
            fprintf(stderr, "cannot open %s for input\n", command->inputf);
            fflush(stdout);
            exit(1);
        }
        int result = dup2(source, 0);
        if (result == -1) {
            fprintf(stderr, "source dup2()\n");
            exit(2);
        }
    }
    if (command->outputf){
        int target = open(command->outputf, O_WRONLY | O_CREAT | O_TRUNC, 0644);
        if (target == -1){
            fprintf(stderr, "cannot open %s for output\n", command->outputf);
            fflush(stdout);
            exit(1);
        }
        int result = dup2(target, 1);
        if (result == -1) { 
            fprintf(stderr, "target dup2()\n"); 
            fflush(stdout);
            exit(2); 
        }
    }
}




void reset_SIGINT(struct Command * command){
    if (!command->background){
        struct sigaction ignore_action;
        ignore_action.sa_handler = SIG_DFL;
        ignore_action.sa_flags = SA_RESTART;
        sigemptyset(&ignore_action.sa_mask);
        sigaction(SIGINT, &ignore_action, NULL);
    }

}

void ignore_SIGINT(){
    struct sigaction ignore_action;
    ignore_action.sa_handler = SIG_IGN;
    ignore_action.sa_flags = SA_RESTART;
    sigemptyset(&ignore_action.sa_mask);
    sigaction(SIGINT, &ignore_action, NULL);
}

void set_SIGTSTP(){
    struct sigaction SIGTSTP_action;
	SIGTSTP_action.sa_handler = handle_SIGTSTP;
	sigfillset(&SIGTSTP_action.sa_mask);
	SIGTSTP_action.sa_flags = SA_RESTART;
	sigaction(SIGTSTP, &SIGTSTP_action, NULL);
}


void ignore_SIGTSTP(){
    struct sigaction ignore_action;
    ignore_action.sa_handler = SIG_IGN;
    ignore_action.sa_flags = SA_RESTART;
    sigemptyset(&ignore_action.sa_mask);
    sigaction(SIGTSTP, &ignore_action, NULL);

}



void handle_SIGTSTP(){
    char entering[] = "Entering foreground-only mode (& is now ignored)\n";
    char exiting[] = "Exiting foreground-only mode\n";

    foreground_only = foreground_only == 1 ? 0 : 1;
    if (process_in_foreground)
        TSTP_received = 1;
    else {
        write(STDOUT_FILENO, "\n", 1);
        foreground_only == 1 ? write(STDOUT_FILENO, entering, 49) : write(STDOUT_FILENO, exiting, 29);
        write(STDOUT_FILENO, ":", 2);
    }
}

void add_bg_proc(pid_t childPid){
    num_bg_procs++;

    for (int i=0; i<MAX_BG_PROCS; i++){
        if (bg_procs[i] == 0) {
            bg_procs[i] = childPid;
            break;
        }
    }
}


void cleanup_bg_procs(){
    int childStatus;

    for (int i=0; i<MAX_BG_PROCS; i++){
        if (bg_procs[i] > 0) {
            pid_t childPid = waitpid(bg_procs[i], &childStatus, WNOHANG);
            if (childPid > 0){
                printf("background pid %d is done: ", childPid);
                fflush(stdout);

                num_bg_procs--;

                if(WIFEXITED(childStatus)){
                    status.type = "exit value";
                    status.val = WEXITSTATUS(childStatus);
                } else{
                    status.type = "terminated by signal";
                    status.val = WTERMSIG(childStatus);
                }
                printf("%s %i\n", status.type, status.val);
                fflush(stdout);
                bg_procs[i] = 0;
            }
        }
    }
}

void kill_bg_procs(){
    int childStatus;

    for (int i = 0; i<MAX_BG_PROCS; i++){
        if (bg_procs[i] > 0){
            kill(bg_procs[i], 1);
            waitpid(bg_procs[i], &childStatus, 0);
        }
    }
}



// void print_struct(struct Command * command){

//     for (int i=0; i<command->argc; i++){
//         printf("%s\n", command->args[i]);
//     }

//     if (command->inputf)
//         printf("%s\n", command->inputf);
    
//     if (command->outputf)
//         printf("%s\n", command->outputf);
//     printf("%i\n", command->background);
// }


// void print_bg_procs(){
//     for (int i=0; i<MAX_BG_PROCS; i++){
//         if (bg_procs[i] > 0) {
//             printf("%d\n", bg_procs[i]);
//         }
//     }
// }