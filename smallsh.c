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

// CONSTANTS
#define MAX_ARGS 513
#define MAX_CMD_LEN 2048
#define MAX_BG_PROCS 20


// STRUCT DEFINITIONS
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

// GLOBALS
struct Status status = { .type = "exit value", .val = 0 }; // holds the exit value of last process

pid_t bg_procs[MAX_BG_PROCS] = {0}; // holds pids of currently running background processes
int num_bg_procs = 0; // number of currently runnig background processes

static volatile sig_atomic_t foreground_only = 0; // flag for foreground only mode
static volatile sig_atomic_t process_in_foreground = 0; // flag that indicated if process is currently in fg
static volatile sig_atomic_t TSTP_received = 0; // flag that idicates if a TSTP signal was received during fg execution

// FUNCTION DECLARATIONS
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

    char buf[MAX_CMD_LEN]; // holds user input as string
    struct Command command; // holds parsed user input

    // SIGNAL SETUP
    set_SIGTSTP();
    ignore_SIGINT();

    while (1) {

        // CLEANUP BACKGROUND PROCESSES
        cleanup_bg_procs();

        // GET INPUT FROM USER
        int n = get_input(buf);
        if (n == 0 || buf[0] == '#') continue; // if user entered nothing, or a comment
        
        // PARSE INPUT
        parse_input(buf, &command);

        // EXECUTE COMMAND
        execute(&command);
        
    }
}

/* 
Reads input from stdin and saves it into buf, until user hits Enter key
*/
int get_input(char buf[]){
    int i = 0;
    char c;

    printf(":"); // print prompt
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

/* 
Takes in a string of user input, and parses it into
components, filling struct Command 
*/
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
        
        // if < or >, set next token to be used as input file or output file, respectively
        if (strcmp(currtok, "<") == 0){
            command->inputf = strtok(NULL, " ");

        } else if (strcmp(currtok, ">") == 0){
            command->outputf = strtok(NULL, " ");

        // otherwise add token to list of arguments
        } else {
            command->args[i++] = currtok;
        }
    }

    command->argc = i; // set number of arguments
}


/*
Executes command in Command struct
*/
void execute(struct Command * command){

    char *command_name = command->args[0];

    // check if command is a built-in, otherwise fork
    if (strcmp(command_name, "cd") == 0){
        // BUILT-IN cd
        change_directory(command);
    } else if (strcmp(command_name, "exit") == 0){
        // BUILT-IN exit
        kill_bg_procs();
        exit(0);
    } else if (strcmp(command_name, "status") == 0){
        // BUILT-IN status
        printf("%s %i\n", status.type, status.val);
        fflush(stdout);
    } else {
        // NON BUILT-IN (will fork)
        fork_and_exec(command);
    }
}

/*
If command is not built-in, executes command by forking and then calling execvp
*/
void fork_and_exec(struct Command * command){

    // check if background process limit has been reached
    if (command->background && num_bg_procs == MAX_BG_PROCS){
        fprintf(stderr, "Too many background processes!");
        fflush(stdout);
        return;
    }

    // copy command and arguments from buffer to newly allocated array
    char **argv = malloc((command->argc+1) * sizeof(char*));
    int i;
    for (i = 0; i<command->argc; i++)
        argv[i] = command->args[i];
    argv[i] = NULL;


    int childStatus;
	pid_t childPid = fork();
    alarm(600);

    if (childPid == -1){
        // FORK FAILED
        perror("fork() failed!\n");
    } else if (childPid == 0){
        // IN CHILD
        handle_redirect(command);
        reset_SIGINT(command);
        ignore_SIGTSTP();

		execvp(argv[0], argv);
        
        // reach here only if exec failed
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

            // retrieve status of exit
            if (WIFEXITED(childStatus)){
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

/*
Changes current working directory, either to the passed argument, or 
to home directory if no argument was passed
*/
void change_directory(struct Command * command){
    // if no argument was passed to cd, change cwd to home directory
    if (command->argc == 1) {
        char *home = getenv("HOME");
        chdir(home);
    // otherwise, attempt to change cwd to passed directory
    } else {
        if (chdir(command->args[1])) {
            fprintf(stderr, "Could not change directory\n");
            fflush(stdout);
        }
    }
}

/*
Changes stdin and stdout of process, if redirection was specified by command
*/
void handle_redirect(struct Command * command){
    // if command is to be run in the background, and no redirection was specified,
    // redirect both stdin and stdout to /dev/null
    if (command->background){
        if (!command->inputf)
            command->inputf = "/dev/null";
        if (!command->outputf)
            command->outputf = "/dev/null";
    }
    
    // if input redirection was specified
    if (command->inputf){
        // open input file
        int source = open(command->inputf, O_RDONLY);
        if (source == -1){
            fprintf(stderr, "cannot open %s for input\n", command->inputf);
            fflush(stdout);
            exit(1);
        }
        // redirect stdin to input file
        int result = dup2(source, 0);
        if (result == -1) {
            fprintf(stderr, "source dup2()\n");
            exit(2);
        }
        // close input file
        if (close(source) == -1) fprintf(stderr, "cannot close %s\n", command->inputf);

    }

    // if output redirection was specified
    if (command->outputf){
        // open output file
        int target = open(command->outputf, O_WRONLY | O_CREAT | O_TRUNC, 0644);
        if (target == -1){
            fprintf(stderr, "cannot open %s for output\n", command->outputf);
            fflush(stdout);
            exit(1);
        }
        //redirect stdout to output file
        int result = dup2(target, 1);
        if (result == -1) { 
            fprintf(stderr, "target dup2()\n"); 
            fflush(stdout);
            exit(2); 
        }
        // close output file
        if (close(target) == -1) fprintf(stderr, "cannot close %s\n", command->outputf);
    }
}

/* 
Reset SIGINT handler to default
*/
void reset_SIGINT(struct Command * command){
    if (!command->background){
        struct sigaction ignore_action;
        ignore_action.sa_handler = SIG_DFL;
        ignore_action.sa_flags = SA_RESTART;
        sigemptyset(&ignore_action.sa_mask);
        sigaction(SIGINT, &ignore_action, NULL);
    }
}

/*
set SIGINT to be ignored
*/
void ignore_SIGINT(){
    struct sigaction ignore_action;
    ignore_action.sa_handler = SIG_IGN;
    ignore_action.sa_flags = SA_RESTART;
    sigemptyset(&ignore_action.sa_mask);
    sigaction(SIGINT, &ignore_action, NULL);
}

/*
Set SIGTSTP to use custom handler
*/
void set_SIGTSTP(){
    struct sigaction SIGTSTP_action;
	SIGTSTP_action.sa_handler = handle_SIGTSTP;
	sigfillset(&SIGTSTP_action.sa_mask);
	SIGTSTP_action.sa_flags = SA_RESTART;
	sigaction(SIGTSTP, &SIGTSTP_action, NULL);
}

/*
Set SIGTSTP to be ignored
*/
void ignore_SIGTSTP(){
    struct sigaction ignore_action;
    ignore_action.sa_handler = SIG_IGN;
    ignore_action.sa_flags = SA_RESTART;
    sigemptyset(&ignore_action.sa_mask);
    sigaction(SIGTSTP, &ignore_action, NULL);

}

/*
Custom SIGTSTP handler that sets/unsets foreground only mode
*/
void handle_SIGTSTP(){
    char entering[] = "Entering foreground-only mode (& is now ignored)\n";
    char exiting[] = "Exiting foreground-only mode\n";

    // change global flag
    foreground_only = foreground_only == 1 ? 0 : 1;

    // if there is a process currently in foreground, set flag so message is printed later
    if (process_in_foreground)
        TSTP_received = 1;
    // otherwise, print message immediately
    else {
        write(STDOUT_FILENO, "\n", 1);
        foreground_only == 1 ? write(STDOUT_FILENO, entering, 49) : write(STDOUT_FILENO, exiting, 29);
        write(STDOUT_FILENO, ":", 2);
    }
}


/*
adds pid of background process to global array
*/
void add_bg_proc(pid_t childPid){

    num_bg_procs++; // increment number of current background processes

    for (int i=0; i<MAX_BG_PROCS; i++){
        // find an empty spot in the array to insert pid
        if (bg_procs[i] == 0) {
            bg_procs[i] = childPid;
            break;
        }
    }
}

/*
Checks all current background processes for termination
*/
void cleanup_bg_procs(){
    int childStatus;

    for (int i=0; i<MAX_BG_PROCS; i++){
        if (bg_procs[i] > 0) {
            // wait for pid without blocking
            pid_t childPid = waitpid(bg_procs[i], &childStatus, WNOHANG);
            if (childPid > 0){
                // if process terminated, print message
                printf("background pid %d is done: ", childPid);
                fflush(stdout);

                num_bg_procs--; // decrement number of current background processes

                // retrieve exit status
                if(WIFEXITED(childStatus)){
                    status.type = "exit value";
                    status.val = WEXITSTATUS(childStatus);
                } else{
                    status.type = "terminated by signal";
                    status.val = WTERMSIG(childStatus);
                }
                
                // print exit status
                printf("%s %i\n", status.type, status.val);
                fflush(stdout);

                bg_procs[i] = 0; // set current index to 0 to allow for reuse
            }
        }
    }
}

/*
Used upon shell exit to kill all currently running background processes
*/
void kill_bg_procs(){
    int childStatus;

    for (int i = 0; i<MAX_BG_PROCS; i++){
        if (bg_procs[i] > 0){
            // kill background process and wait for it
            kill(bg_procs[i], 1); 
            waitpid(bg_procs[i], &childStatus, 0);
        }
    }
}

