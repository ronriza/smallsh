#include <stdio.h>
#include <unistd.h>
#include <stdint.h>
#include <string.h>
#include <stdlib.h>
#include <err.h>
#include <errno.h>
#include <sys/wait.h>
#include <fcntl.h>
#include <sys/types.h>

struct Command {
    char *args[513];
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

int get_input(char buf[]);
void parse_input(char buf[], struct Command * command);
void print_struct(struct Command * command);
void change_directory(struct Command * command);

void handle_redirect(struct Command * command);


void fork_and_exec(struct Command * command){
    char **argv = malloc(command->argc *sizeof(char*) +1 );

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
        // printf("CHILD(%d) running command\n", getpid());
		execvp(argv[0], argv);
		fprintf(stderr, "%s: %s\n", argv[0], strerror(errno));
        exit(1);
    } else {
        // IN PARENT
		childPid = waitpid(childPid, &childStatus, 0);
		// printf("PARENT(%d): child(%d) terminated. Exiting\n", getpid(), spawnPid);

        if(WIFEXITED(childStatus)){
            status.type = "exit value";
            status.val = WEXITSTATUS(childStatus);
		} else{
            status.type = "terminated by signal";
            status.val = WTERMSIG(childStatus);
		}
    }


}

void execute(struct Command * command){

    char *command_name = command->args[0];

    if (strcmp(command_name, "cd") == 0){
        change_directory(command);

        // char cwd[100];
        // getcwd(cwd, 100);
        // printf("Current directory is now: %s\n", cwd);

    } else if (strcmp(command_name, "exit") == 0){

    } else if (strcmp(command_name, "status") == 0){
        printf("%s %i\n", status.type, status.val);
        fflush(stdout);

    } else {

        fork_and_exec(command);

    }

}


int main (void) {

    char buf[2048];
    struct Command command;

    while (1) {

        // GET INPUT
        int n = get_input(buf);
        if (n == 0 || buf[0] == '#') continue; // if user entered nothing, or a comment
        
        // PARSE INPUT
        parse_input(buf, &command);

        // EXECUTE
        execute(&command);

    }
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

void change_directory(struct Command * command){
    if (command->argc == 1) {
        char *home = getenv("HOME");
        chdir(home);
    } else{
        if (chdir(command->args[1])) fprintf(stderr, "Could not change directory\n");
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
            exit(1);
        }
        int result = dup2(target, 1);
        if (result == -1) { 
            fprintf(stderr, "target dup2()\n"); 
            exit(2); 
        }
    }
}




void print_struct(struct Command * command){

    for (int i=0; i<command->argc; i++){
        printf("%s\n", command->args[i]);
    }

    if (command->inputf)
        printf("%s\n", command->inputf);
    
    if (command->outputf)
        printf("%s\n", command->outputf);
    printf("%i\n", command->background);
}