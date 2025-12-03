/*
Copyright 12/03/2025 https://github.com/su8/0vershell
This program is free software; you can redistribute it and/or modify
it under the terms of the GNU General Public License as published by
the Free Software Foundation; either version 2 of the License, or
(at your option) any later version.

This program is distributed in the hope that it will be useful,
but WITHOUT ANY WARRANTY; without even the implied warranty of
MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
GNU General Public License for more details.

You should have received a copy of the GNU General Public License
along with this program; if not, write to the Free Software
Foundation, Inc., 51 Franklin Street, Fifth Floor, Boston,
MA 02110-1301, USA.
*/
#include <iostream>
#include <fstream>
#include <sstream>
#include <vector>
#include <string>
#include <unistd.h> 
#include <sys/wait.h>
#include <fcntl.h>
#include <cstring>
#include <cstdlib>
#include <algorithm>
#include <signal.h>
#include <dirent.h>
#include <readline/readline.h>
#include <readline/history.h>

std::string trim(const std::string &s);
void initHistoryPath(void);
void loadPersistentHistory(void);
void savePersistentHistory(void);
void sigchldHandler(int);
std::vector<char*> parseInput(std::string cmd);
void loadHistory(void);
void executeCommand(const std::string &cmd);
void loadSystemCommands(void);
char* commandGenerator(const char* text, int state);
char **myCompletion(const char *text, int start, int end);
void executePipeline(const std::vector<std::string> &commands, bool background);

#define HISTORY_FILE ".0vershell"

std::vector<std::string> commandList;
std::vector<std::string> historyList; // Command history
std::string historyPath;


int main(int argc, char *argv[]) {
  initHistoryPath();
  loadPersistentHistory();
  loadSystemCommands();
  rl_attempted_completion_function = myCompletion;
  signal(SIGCHLD, sigchldHandler);
  while (true) {
    char* input = readline("myshell> ");
    if (!input) { // EOF (Ctrl+D)
      std::cout << "\n";
      break;
    }
    input = trim(input);
    std::string cmd(trim(static_cast<std::string>(input)));
    free(input);
    if (cmd.empty()) continue;
    add_history(cmd.c_str());
    append_history(1, historyPath.c_str());
    if (cmd == "exit") break;
    if (cmd == "history") {
      HIST_ENTRY** histList = history_list();
      if (histList) {
        for (int x = 0; histList[x]; x++) { std::cout << (x + history_base) << ": " << histList[x]->line << "\n"; }
      }
      continue;
    }
    // History execution: !!
    if (cmd == "!!") {
      HIST_ENTRY* last = previous_history();
      if (!last) {
        std::cout << "No commands in history.\n";
      } else {
        std::cout << "Running: " << last->line << "\n";
        executeCommand(last->line);
      }
      continue;
    }
    // History execution: !N
    if (cmd[0] == '!' && std::isdigit(cmd[1])) {
      int cmdNum = std::stoi(cmd.substr(1));
      HIST_ENTRY** histList = history_list();
      if (!histList || cmdNum < history_base || cmdNum >= history_base + history_length) {
        std::cout << "No such command in history.\n";
      } else {
        std::cout << "Running: " << histList[cmdNum - history_base]->line << "\n";
        executeCommand(histList[cmdNum - history_base]->line);
      }
      continue;
    }
    // Check for background process
    bool background = false;
    if (cmd.back() == '&') {
      background = true;
      cmd.pop_back();
      cmd = trim(cmd);
    }
    // Split by pipe
    std::vector<std::string> commands;
    std::stringstream ss(cmd);
    std::string segment;
    while (std::getline(ss, segment, '|')) {
      commands.push_back(trim(segment));
    }
    executePipeline(commands, background);
  }
  savePersistentHistory();
  return EXIT_SUCCESS;
}

// ======== Utility Functions ========

// Trim whitespace from both ends
std::string trim(const std::string &s) {
  size_t start = s.find_first_not_of(" \t");
  size_t end = s.find_last_not_of(" \t");
  return (start == std::string::npos) ? "" : s.substr(start, end - start + 1); }

// Build history file path
void initHistoryPath(void) {
  const char* home = getenv("HOME");
  if (!home) home = ".";
  historyPath = std::string(home) + "/" + HISTORY_FILE;
}

// Load persistent history
void loadPersistentHistory(void) {
  using_history();
  read_history(historyPath.c_str());
}

// Save persistent history
void savePersistentHistory(void) {
  write_history(historyPath.c_str());
}

// Handle SIGCHLD to prevent zombies
void sigchldHandler(int) {
  while (waitpid(-1, nullptr, WNOHANG) > 0);
}

// Parse input into args
std::vector<char*> parseInput(std::string cmd) {
  std::vector<char*> args;
  std::istringstream iss(cmd);
  std::string token;
  while (iss >> token) {
    char* arg = new char[token.size() + 1];
    std::strcpy(arg, token.c_str());
    args.push_back(arg);
  }
  args.push_back(nullptr);
  return args;
}

// Load history from file
void loadHistory(void) {
  std::ifstream file(historyPath);
  std::string line;
  while (std::getline(file, line)) {
    if (!line.empty()) historyList.push_back(line);
  }
}

/* Append a command to history file
// Split a command string into tokens
std::vector<char*> parseCommand(const std::string &cmd) {
    std::stringstream ss(cmd);
    std::string token;
    std::vector<char*> args;
    while (ss >> token) {
        char *arg = new char[token.size() + 1];
        std::strcpy(arg, token.c_str());
        args.push_back(arg);
    }
    args.push_back(nullptr);
    return args;
}*/

// Execute a single command with optional redirection
// Execute a command
void executeCommand(const std::string &cmd) {
  auto args = parseInput(cmd);
  if (args.size() <= 1) return;
  bool background = false;
  if (args.size() > 2 && std::string(args[args.size() - 2]) == "&") {
    background = true;
    delete[] args[args.size() - 2];
    args[args.size() - 2] = nullptr;
  }
  pid_t pid = fork();
  if (pid < 0) {
    perror("fork failed");
  } else if (pid == 0) {
    execvp(args[0], args.data());
    perror("exec failed");
    exit(EXIT_FAILURE);
  } else {
    if (!background) {
      waitpid(pid, nullptr, 0);
    } else {
      std::cout << "[Background PID: " << pid << "]\n";
    }
  }
  for (size_t i = 0; i < args.size(); ++i) {
    delete[] args[i];
  }
}

// ======== Tab Completion ========

// Load all commands from PATH into commandList
void loadSystemCommands(void) {
  char* pathEnv = getenv("PATH");
  if (!pathEnv) return;

  std::string pathCopy(pathEnv);
  std::istringstream iss(pathCopy);
  std::string dir;
  while (std::getline(iss, dir, ':')) {
    DIR* dp = opendir(dir.c_str());
    if (dp) {
      struct dirent* entry;
      while ((entry = readdir(dp)) != nullptr) {
        if (entry->d_type == DT_REG || entry->d_type == DT_LNK || entry->d_type == DT_UNKNOWN) {
          commandList.push_back(entry->d_name);
        }
      }
      closedir(dp);
    }
  }
}

// Generator function for readline
char *commandGenerator(const char *text, int state) {
  static size_t listIndex;
  static size_t len;
  if (!state) {
    listIndex = 0;
    len = std::strlen(text);
  }
  while (listIndex < commandList.size()) {
    const std::string& name = commandList[listIndex++];
    if (name.compare(0, len, text) == 0) {
      return strdup(name.c_str());
    }
  }
  return nullptr;
}

// Completion function
char **myCompletion(const char *text, int start, int end) {
  static_cast<void>(end);
  if (start == 0) {
    return rl_completion_matches(text, commandGenerator);
  }
  return nullptr; // Default filename completion
}

// Execute a pipeline of commands
void executePipeline(const std::vector<std::string> &commands, bool background) {
  int numCmds = commands.size();
  int pipefd[2], in_fd = 0;

  for (int i = 0; i < numCmds; i++) {
    if (i < numCmds - 1) {
      if (pipe(pipefd) == -1) { perror("pipe"); exit(EXIT_FAILURE); }
    }
    pid_t pid = fork();
    if (pid < 0) {
      perror("fork");
      exit(EXIT_FAILURE);
    } else if (pid == 0) {
      // Child process
      if (i > 0) { // Not first command
        dup2(in_fd, STDIN_FILENO);
        close(in_fd);
      }
      if (i < numCmds - 1) { // Not last command
        close(pipefd[0]);
        dup2(pipefd[1], STDOUT_FILENO);
        close(pipefd[1]);
      }
      executeCommand(commands[i]);
    } else {
      // Parent process
      if (i > 0) close(in_fd);
      if (i < numCmds - 1) {
        close(pipefd[1]);
        in_fd = pipefd[0];
      }
      if (!background || i < numCmds - 1) {
        waitpid(pid, nullptr, 0);
      } else {
        std::cout << "[Background PID] " << pid << "\n";
      }
    }
  }
}