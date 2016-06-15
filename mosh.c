/***
 * Author:  Mike Williams
 * Class:   COP4610
 * Project: 1 - Implementing a Shell
 * File:    mosh.c
 ***/

/*** INCLUDES ***/

#include <errno.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <sys/types.h>
#include <sys/wait.h>
#include <ctype.h>
#include <time.h>
#include <fcntl.h>

/*** MACROS ***/

#define TIME_BUFFER_SIZE 7
#define HIST_GROWTH_SIZE 15
#define BUFFER_SIZE 128
#define PATH_DELIM ":"
#define COMMAND_DELIM " "
#define BEGIN_SLOT 0
#define END_SLOT 1

/*** VARIABLES ***/

char HOSTNAME[BUFFER_SIZE];
char *USER;
char *HOME;
char **PATH;
char *cwd;
char *buffer;
char **command;
char ***command_args;
int num_commands;

pid_t pid_self;
int stay_alive;
int num_paths;
int num_children;
int iter_status;

char *redirect;

int keep_input;
int keep_output;

// storage for history
char *time_buffer;
time_t timestamp;
struct tm * timeinfo;
int hist_log_size;
int hist_log_capacity;
char **hist_begin_time;
char **hist_end_time;
char **hist_command;
char *hist_status;
pid_t *hist_pid;
int *hist_state;

/*** FUNCTION PROTOTYPES ***/

void init_env();
void prompt();
void read_input();
void clear_buffer();
void execute(int background);
void execute_command(int command_num, int fd_in, int fd_out);

void viewproc(char *proc_file);
void history();
void extend_log();
void echo(char **input);
void cd(char **input);
void which(char **input, int list_all);
void swap_home(char **string);
int num_args(char **arguments);
int expand_env(int command_num, int index);
void set_time(int time_slot, int index);
void kill_child();

/*** MAIN FUNCTION ***/

int main(int argc, char **arg) {
  init_env();

  while (stay_alive == 1) {
    clear_buffer();
    prompt();
    read_input();
    if (strlen(buffer) != 0) {
      if (iter_status == 0) {
        execute(0);
      } else if (iter_status == 2) {
        execute(1);
      } else {
        set_time(END_SLOT,hist_log_size-1);
      }
    }
  }

  if (pid_self == getpid())
    printf("*exit*\n");

  return 0;
}

/*** PROGRAM FUNCTIONS ***/

/** init_env - initialize shell environment
 **/
void init_env() {
  USER = getenv("USER");
  HOME = getenv("HOME");

  // getenv() is not used on HOSTNAME because it is not a POSIX variable
  HOSTNAME[BUFFER_SIZE-1] = 0;
  gethostname(HOSTNAME,BUFFER_SIZE);

  // break up individual paths from PATH variable
  char *token;
  int num_paths = 0;
  for (token = strtok(getenv("PATH"),PATH_DELIM);
       token != NULL;
       token = strtok(NULL,PATH_DELIM)) {
    PATH = (char**)realloc(PATH,(++num_paths+1)*sizeof(char*));
    PATH[num_paths-1] = strdup(token);
  }
  free(token);

  // add NULL to end of PATH array
  PATH = (char**)realloc(PATH,(++num_paths+1)*sizeof(char*));
  PATH[num_paths-1] = NULL;

  cwd = NULL;
  buffer = NULL;

  // initialize command
  command = (char**)malloc(sizeof(char*));
  command[0] = NULL;

  // initialize command arguments
  command_args = (char***)malloc(sizeof(char**));
  command_args[0] = (char**)malloc(sizeof(char*));
  command_args[0][0] = NULL;

  num_commands = 0;
  pid_self = getpid();
  stay_alive = 1;
  iter_status = 0;

  redirect = (char*)malloc(sizeof(char));
  redirect[0] = 0;

  // store default stdin/stdout
  keep_input = dup(0);
  keep_output = dup(1);

  // initialize history
  hist_log_size = 0;
  hist_log_capacity = HIST_GROWTH_SIZE;
  hist_begin_time = (char**)malloc(HIST_GROWTH_SIZE*sizeof(char*));
  hist_end_time = (char**)malloc(HIST_GROWTH_SIZE*sizeof(char*));
  hist_command = (char**)malloc(HIST_GROWTH_SIZE*sizeof(char*));
  hist_status = (char*)malloc(HIST_GROWTH_SIZE*sizeof(char));
  hist_pid = (pid_t*)malloc(HIST_GROWTH_SIZE*sizeof(pid_t));
  hist_state = (int*)malloc(HIST_GROWTH_SIZE*sizeof(int));

  // tell parent to send completed children to the kill_child() function
  signal(SIGCHLD,&kill_child);
}

/** prompt - display shell prompt
 **/
void prompt() {
  if (cwd != NULL)
    free(cwd);
  cwd = getcwd(NULL,0);

  // check cwd for HOME to replace with ~
  int i;
  for (i = 0; i < strlen(HOME); i++) {
    if (HOME[i] != cwd[i]) {
      i = -1;
      break;
    }
  }
  if (i != -1) {
    char *old_cwd = strdup(cwd);
    char *extra = old_cwd + strlen(HOME);

    free(cwd);
    cwd = (char*)malloc((3+(strlen(old_cwd)-strlen(HOME)))*sizeof(char));
    cwd[0] = '~';
    cwd[1] = 0;
    strcat(cwd,extra);
  }

  // check for completed background jobs
  if (hist_log_size != 0) {
    for (i = 0; i < hist_log_size; i++) {
      if (hist_status[i] == 'R') {
        // create PID file string
        char *proc_status = (char*)malloc(20*sizeof(char));
        char pid_string[6];
        sprintf(pid_string,"%d",hist_pid[i]);
        strcpy(proc_status,"/proc/");
        strcat(proc_status,pid_string);
        strcat(proc_status,"/status");

        // check status of PID file
        if (access(proc_status,F_OK)) {
          hist_end_time[i] = (char*)malloc(TIME_BUFFER_SIZE*sizeof(char));
          set_time(END_SLOT,i);
        }
        free(proc_status);
      }
    }
  }

  // informative prompt
  /*
  printf("-[%s@%s]-[jobs:%d]-[pid:%d]\n",USER,HOSTNAME,num_children,pid_self);
  printf("-[%s]-> ",cwd);
  */

  // basic prompt, enabled for project write-up purposes
  printf("%s@%s:%s:::D ",USER,HOSTNAME,cwd);
}

/** read_input - read input to buffer and parse arguments
 **/
void read_input() {
  iter_status = 0;
  buffer = (char*)malloc((BUFFER_SIZE+1)*sizeof(char));
  fgets(buffer,BUFFER_SIZE,stdin);

  // remove tail newline/return
  int i;
  for (i = 0; i < BUFFER_SIZE; i++) {
    if (buffer[i] == '\n' || buffer[i] == '\r')
      buffer[i] = 0;
  }

  // check for log capacity limit
  if ((hist_log_capacity-hist_log_size) < 2)
    extend_log();

  char *token;
  for (i = 0, token = strtok(strdup(buffer),COMMAND_DELIM);
       token != NULL && iter_status == 0;
       token = strtok(NULL,COMMAND_DELIM), i++) {
    // first token is command
    if (i == 0) {
      num_commands++;
      command[num_commands-1] = strdup(token);
    } else if ((num_commands > 1 && strcmp(command[num_commands-2],"&") == 0) ||
               (i > 1 && strcmp(command_args[num_commands-1][i-2],"&") == 0)) {
      fprintf(stderr,"mosh: Misplaced & (argument %d).\n",i-1);
      iter_status = 1;
    } else if (num_commands > 1 &&
              (i > 1 && strcmp(command_args[num_commands-2][num_args(command_args[num_commands-2])-1],"&") == 0)) {
      fprintf(stderr,"mosh: Misplaced & (command %d).\n",num_commands-1);
      iter_status = 1;
    }
    // handle piping
    else if (strcmp(token,"|") == 0) {
      // add room for new command
      command = (char**)realloc(command,(num_commands+1)*sizeof(char*));
      command[num_commands] = NULL;

      // add room for new command's arguments
      command_args = (char***)realloc(command_args,(num_commands+1)*sizeof(char**));
      command_args[num_commands] = (char**)malloc(sizeof(char*));
      command_args[num_commands][0] = NULL;

      // reset argument counting variable
      i = -1;
    }
    // handle I/O redirection
    else if (strcmp(token,">") == 0 || strcmp(token,"<") == 0) {
      // check for previous redirection flags
      if (redirect[0] != 0) {
        fprintf(stderr,"mosh: Multiple I/O redirection arguments not supported.\n");
        iter_status = 1;
      }

      char direction;
      if (strcmp(token,">") == 0)
        direction = '>';
      else
        direction = '<';

      // get next token from input (redirection file)
      token = strtok(NULL,COMMAND_DELIM);

      // check for missing redirect file
      if (token == NULL) {
        fprintf(stderr,"mosh: No file specified after redirection.\n");
        iter_status = 1;
      } else {
        redirect = (char*)realloc(redirect,(strlen(token)+2)*sizeof(char));
        redirect[0] = direction;
        strcat(redirect,token);

        // set i back to account for redirection flag
        i--;
      }
    }
    // other tokens are arguments
    else {
      // make room for argument
      command_args[num_commands-1] = (char**)realloc(command_args[num_commands-1],(i+1)*sizeof(char*));
      command_args[num_commands-1][i-1] = strdup(token);
      command_args[num_commands-1][i] = NULL;

      // check for environment variable
      char *env_var;
      if ((env_var = strpbrk(command_args[num_commands-1][i-1],"$")) != NULL)
        iter_status = expand_env(num_commands-1,i-1);
    }
  }
  if (iter_status == 0)
    free(token);

  // check for no input
  if (i == 0 && num_commands == 0) {
    buffer[0] = 0;
    return;
  }

  if (iter_status == 0) {
    if (i > 1 && strcmp(command_args[num_commands-1][i-2],"&") == 0) {
      iter_status = 2;
      free(command_args[num_commands-1][i-2]);
      command_args[num_commands-1][i-2] = NULL;
    }
  }
}

/** clear_buffer - free up memory taken by buffer, command, and command_args
 **/
void clear_buffer() {
  free(buffer);
  buffer = NULL;

  int i, j;
  for (j = num_commands-1; j >= 0; j--) {
    free(command[j]);
    command[j] = NULL;

    for (i = num_args(command_args[j])-1; i >= 0; i--) {
      free(command_args[j][i]);
      command_args[j][i] = NULL;
    }
  }
  num_commands = 0;

  command = (char**)malloc(sizeof(char*));
  command[0] = NULL;

  command_args = (char***)malloc(sizeof(char**));
  command_args[0] = (char**)malloc(sizeof(char*));
  command_args[0][0] = NULL;

  free(redirect);
  redirect = (char*)malloc(sizeof(char));
  redirect[0] = 0;
}

/** execute - analyze user input and execute all given commands
 **/
void execute(int background) {
  int command_num;
  int fd_in = -1;
  int fd_out;
  int i;

  // check for exit
  for (command_num = 0; command_num < num_commands; command_num++) {
    if (strcmp(command[command_num],"exit") == 0) {
      stay_alive = 0;
      return;
    }
  }

  // set history timestamp
  set_time(BEGIN_SLOT,hist_log_size);
  hist_command[hist_log_size] = strdup(buffer);
  hist_end_time[hist_log_size] = "--:--";
  hist_log_size++;

  // check components of command for ~ to replace
  for (command_num = 0; command_num < num_commands; command_num++) {
    // check command
    if (command[command_num][0] == '~')
      swap_home(&command[command_num]);
    // check arguments
    for (i = 0; command_args[command_num][i] != NULL; i++)
      if (command_args[command_num][i][0] == '~')
        swap_home(&command_args[command_num][i]);
  }

  // fork and execute
  pid_t child_pid = fork();
  if (child_pid == 0) {
    stay_alive = 0;

    // iterate through each command
    for (command_num = 0; command_num < num_commands; command_num++) {
      int fd_pipe[2];

      // account for I/O redirection
      if (redirect[0] != 0) {
        char *redirect_file = redirect+1;
        if (redirect[0] == '<') {
          // set input fd to opened file
          fd_in = open(redirect_file,O_RDONLY);
        } else {
          // set output fd to opened file
          // create if missing
          // replace current text if it exists
          fd_out = open(redirect_file,O_RDWR|O_CREAT|O_TRUNC);
        }
      }

      // set pipe for stdout if there is a next command
      else if (command_num != num_commands-1) {
        pipe(fd_pipe);
        fd_out = fd_pipe[1];
      } else {
        // return stdout fd to default
        fd_out = keep_output;
      }

      // execute command with modified stdin/stdout
      execute_command(command_num,fd_in,fd_out);

      // close fds to prevent any fd leaks
      close(fd_in);
      close(fd_out);

      // set stdin for next command
      fd_in = fd_pipe[0];
    }
  } else {
    hist_pid[hist_log_size-1] = child_pid;
    if (background == 1) {
      // don't wait for child to finish executing
      waitpid(child_pid,&hist_state[hist_log_size-1],WNOHANG);
    } else {
      // wait for child's termination then set history end time
      waitpid(child_pid,&hist_state[hist_log_size-1],0);
      set_time(END_SLOT,hist_log_size-1);
    }
  }
}

/** execute_command - execute individual command
 **/
void execute_command(int command_num, int fd_in, int fd_out) {
  int i;

  // change stdin if necessary
  if (fd_in != -1 && fd_in != 0) {
    dup2(fd_in,0);
    close(fd_in);
  }
  // change stdout if necessary
  if (fd_out != -1 && fd_in != 1) {
    dup2(fd_out,1);
    close(fd_out);
  }

  // check for built-ins
  if (strcmp(command[command_num],"viewproc") == 0) {
    if (num_args(command_args[command_num]) > 1)
      fprintf(stderr,"viewproc: Too many arguments.\n");
    else
      viewproc(command_args[command_num][0]);
  } else if (strcmp(command[command_num],"history") == 0)
    history();
  else if (strcmp(command[command_num],"cd") == 0)
    cd(command_args[command_num]);
  else if (strcmp(command[command_num],"echo") == 0)
    echo(command_args[command_num]);
  else if (strcmp(command[command_num],"which") == 0) {
    if (command_args[command_num][0] == NULL)
      which(NULL,0);
    else if (strcmp(command_args[command_num][0],"-a") == 0)
      which(command_args[command_num],1);
    else
      which(command_args[command_num],0);
  }

  // execute external command
  else {
    int num_exec_args = num_args(command_args[command_num]);
    command_args[command_num] = (char**)realloc(command_args[command_num],(num_exec_args+2)*sizeof(char*));
    command_args[command_num][num_exec_args+1] = NULL;
    for (i = num_args(command_args[command_num]); i > 0; i--)
        command_args[command_num][i] = strdup(command_args[command_num][i-1]);
    command_args[command_num][0] = strdup(command[command_num]);

    // check through PATH for executable
    char *temp_path;
    for (i = 0; PATH[i] != NULL; i++) {
      temp_path = (char*)malloc((strlen(command[command_num])+strlen(PATH[i])+1)*sizeof(char));
      strcpy(temp_path,PATH[i]);
      strcat(temp_path,"/");
      strcat(temp_path,command[command_num]);

      // check for executable in PATH
      if (!access(temp_path,X_OK)) {
        command[command_num] = strdup(temp_path);
        command_args[command_num][0] = strdup(temp_path);
        free(temp_path);
        break;
      }
      free(temp_path);
    }

    // fork and execute
    pid_t pid;
    int status;
    if (!access(command[command_num],F_OK)) {
      if (!access(command[command_num],X_OK)) {
        if ((pid = fork()) == 0) {
          execv(command[command_num],command_args[command_num]);
        } else {
          waitpid(pid,&status,0);
        }
      } else {
        fprintf(stderr,"%s: Permission denied.\n",command[command_num]);
      }
    } else {
      fprintf(stderr,"%s: Command not found.\n",command[command_num]);
    }
  }
}

/** viewproc - view information about the /proc filesystem
 **/
void viewproc(char *proc_file) {
  if (proc_file == NULL) {
    fprintf(stderr,"viewproc: No file specified.\n");
    return;
  }

  // piece together proc file absolute path
  char *proc_path = "/proc/";
  char *proc_abs_path = (char*)malloc((strlen(proc_path)+strlen(proc_file)+1)*sizeof(char));

  strcpy(proc_abs_path,proc_path);
  strcat(proc_abs_path,proc_file);

  if (!access(proc_abs_path,F_OK)) {
    if (!access(proc_abs_path,R_OK)) {
      FILE *procfile = fopen(proc_abs_path,"r");
      if (!procfile)
        perror(proc_abs_path);
      else {
        char proc_buffer[BUFFER_SIZE];
        while (fgets(proc_buffer,BUFFER_SIZE,procfile) != NULL) {
          printf("%s",proc_buffer);
        }
        fclose(procfile);
      }
    } else {
      fprintf(stderr,"viewproc: Permission denied.\n");
    }
  } else {
    fprintf(stderr,"viewproc: %s was not found in /proc.\n",proc_file);
  }
}

/** history - show command history
 **/
void history() {
  int i;
  printf(" PID   State  Begin   End    Command\n");
  for (i = 0; i < hist_log_size-1; i++) {
    printf("%d\t[%c]   %s  %s   %s\n",hist_pid[i],hist_status[i],hist_begin_time[i],hist_end_time[i],hist_command[i]);
  }
}

/** extend_log - increase capacity for history log
 **/
void extend_log() {
  // expand history memory by predefined HIST_GROWTH_SIZE
  hist_begin_time = (char**)realloc(hist_begin_time,(hist_log_capacity+HIST_GROWTH_SIZE)*sizeof(char*));
  hist_end_time = (char**)realloc(hist_end_time,(hist_log_capacity+HIST_GROWTH_SIZE)*sizeof(char*));
  hist_command = (char**)realloc(hist_command,(hist_log_capacity+HIST_GROWTH_SIZE)*sizeof(char*));
  hist_status = (char*)realloc(hist_status,(hist_log_capacity+HIST_GROWTH_SIZE)*sizeof(char));
  hist_pid = (pid_t*)realloc(hist_pid,(hist_log_capacity+HIST_GROWTH_SIZE)*sizeof(pid_t));
  hist_state = (int*)realloc(hist_state,(hist_log_capacity+HIST_GROWTH_SIZE)*sizeof(int));

  hist_log_capacity += HIST_GROWTH_SIZE;
}

/** echo - print out arguments to screen
 **/
void echo(char **input) {
  int i;
  for (i = 0; input[i] != NULL; i++) {
    printf("%s",input[i]);
    if (input[i+1] != NULL)
      printf(" ");
  }
  printf("\n");
}

/** cd - change current directory to new_dir
 **/
void cd(char **input) {
  // use HOME for no arguments
  if (num_args(input) == 0)
    chdir(HOME);

  // check for bad syntax
  else if (num_args(input) > 1)
    fprintf(stderr,"cd: Too many arguments.\n");

  else {
    // replace leading ~ with HOME path
    if (input[0][0] == '~')
      swap_home(&input[0]);

    if (!access(input[0],F_OK)) {
      if (chdir(input[0]) == -1)
        if (errno == ENOTDIR) {
          fprintf(stderr,"cd: %s: Not a directory.\n",input[0]);
          errno = 0;
        }
    } else
      fprintf(stderr,"cd: %s: No such file or directory.\n",input[0]);
  }
}

/** which - show full path of executable if existant
 **/
void which(char **input, int list_all) {
  // check for no input
  if (num_args(input) == 0 ||
     (num_args(input) == 1 && list_all == 1)) {
    fprintf(stderr,"which: No command provided.\n");
    return;
  }

  // search through every PATH for every input value
  char *test_path = NULL;
  int i, j;
  for (j = list_all; input[j] != NULL; j++) {
    // check for built-ins
    if (strcmp(input[j],"exit") == 0 ||
        strcmp(input[j],"history") == 0 ||
        strcmp(input[j],"cd") == 0 ||
        strcmp(input[j],"echo") == 0 ||
        strcmp(input[j],"which") == 0)
      printf("%s: Built-in command.\n",input[j]);

    else {
      for (i = 0; PATH[i] != NULL; i++, free(test_path)) {
        test_path = (char*)malloc((strlen(PATH[i])+strlen(input[j])+2)*sizeof(char));
        strcpy(test_path,PATH[i]);
        strcat(test_path,"/");
        strcat(test_path,input[j]);

        if (!access(test_path,X_OK)) {
          printf("%s\n",test_path);
          if (list_all == 0) {
            free(test_path);
            break;
          }
        }
      }
    }
  }
}

/** swap_home - swap ~ with absolute HOME path
 **/
void swap_home(char **string) {
  char *temp = (char*)malloc((strlen(HOME)+1)*sizeof(char));
  char *good_input = *string + 1;

  strcpy(temp,HOME);
  temp = (char*)realloc(temp,(strlen(HOME)+strlen(good_input)+1)*sizeof(char));
  strcat(temp,good_input);

  *string = strdup(temp);
  free(temp);
}

/** num_args - return number of arguments passed in
 **/
int num_args(char **arguments) {
  // check for no arguments
  if (arguments == NULL)
    return 0;

  // count total non-NULL arguments and return value
  int total_args;
  for (total_args = 0; arguments[total_args] != NULL; total_args++) {}

  return total_args;
}

/** expand_env - expand environment variable
 **/
int expand_env(int command_num, int index) {
  char *variable_begin = strpbrk(command_args[command_num][index],"$");
  char *variable_name = (char*)malloc((strlen(variable_begin+1)+1)*sizeof(char));
  char *value = NULL;
  char *pre_var = NULL;
  char *post_var = NULL;
  char *temp = NULL;
  int extra_chars = 0;
  int i;

  strcpy(variable_name,variable_begin+1);

  // check for content in argument before variable
  if (variable_begin != command_args[command_num][index]) {
    for (i = 0; command_args[command_num][index][i] != '$'; i++) {}
    pre_var = (char*)malloc((i+1)*sizeof(char));
    strncpy(pre_var,command_args[command_num][index],i);
    pre_var[i] = 0;
    extra_chars += strlen(pre_var);
  }

  // check for content in argument after variable
  for (i = 0; i < strlen(variable_name); i++) {
    if (ispunct(variable_name[i])) {
      post_var = (char*)malloc((strlen(variable_name+i)+1)*sizeof(char));
      strcpy(post_var,variable_name+i);
      extra_chars += strlen(post_var);
      temp = (char*)malloc((i+1)*sizeof(char));
      strncpy(temp,variable_name,i);
      temp[i] = 0;
      free(variable_name);
      variable_name = strdup(temp);
      free(temp);
      break;
    }
  }

  // leave variable_arg alone if nothing more than $ found
  if (strlen(variable_name) == 0) {
    free(variable_name);
    if (pre_var != NULL) {
      free(pre_var);
    }
    if (post_var != NULL) {
      free(post_var);
    }
    return 0;
  }

  value = getenv(variable_name);

  // replace variable with value (if it exists)
  if (value != NULL) {
    free(command_args[command_num][index]);

    command_args[command_num][index] = (char*)malloc((strlen(value)+1)*sizeof(char));
    command_args[command_num][index][0] = 0;

    // check for extra pre/post characters
    if (post_var != NULL || pre_var != NULL) {
      command_args[command_num][index] = (char*)realloc(command_args[command_num][index],(extra_chars+strlen(value)+1)*sizeof(char));
    }

    // piece together final product
    if (pre_var != NULL)
      strcat(command_args[command_num][index],pre_var);
    strcat(command_args[command_num][index],value);
    if (post_var != NULL)
      strcat(command_args[command_num][index],post_var);

    // free variables and exit
    free(variable_name);
    if (pre_var != NULL) {
      free(pre_var);
    }
    if (post_var != NULL) {
      free(post_var);
    }
    return 0;
  }
  // handle variable not found
  else {
    fprintf(stderr,"mosh: Environment variable %s not found.\n",variable_name);
    free(variable_name);
    if (pre_var != NULL) {
      free (pre_var);
    }
    if (post_var != NULL) {
      free (post_var);
    }
    return 1;
  }
}

/** set_time - get and store time
 **/
void set_time(int time_slot, int index) {
  timestamp = time(NULL);
  timeinfo = localtime(&timestamp);
  time_buffer = (char*)malloc(TIME_BUFFER_SIZE*sizeof(char));
  strftime(time_buffer,6,"%I:%M",timeinfo);

  if (time_slot == 0) {
    hist_status[index] = 'R';
    hist_begin_time[index] = (char*)malloc(TIME_BUFFER_SIZE*sizeof(char));
    strcpy(hist_begin_time[index],time_buffer);
    num_children++;
  } else {
    hist_status[index] = 'C';
    hist_end_time[index] = (char*)malloc(TIME_BUFFER_SIZE*sizeof(char));
    strcpy(hist_end_time[index],time_buffer);
    num_children--;
  }

  free(time_buffer);
}

/** kill_child - kill child after its completion
 **/
void kill_child() {
  int status;

  // kill child and reset signal
  waitpid(0,&status,WNOHANG);
  signal(SIGCHLD,&kill_child);
}
