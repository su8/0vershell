/*
 * Copyright 12/03/2025 https://github.com/su8/0vershell
 * This program is free software; you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation; either version 2 of the License, or
 * (at your option) any later version.
 *
 * This program is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU General Public License for more details.
 *
 * You should have received a copy of the GNU General Public License
 * along with this program; if not, write to the Free Software
 * Foundation, Inc., 51 Franklin Street, Fifth Floor, Boston,
 * MA 02110-1301, USA.
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
#include <unordered_map>
#include <regex>
#include <filesystem>
#include <cctype>
#include <readline/readline.h>
#include <readline/history.h>

struct Command {
  std::vector<char*> args;
  std::string infile;
  std::string outfile;
  bool append = false;
};

struct Job {
  pid_t pgid;
  std::string command;
  bool running;
};

std::unordered_map<int, Job> jobs;
int nextJobId = 1;

std::string trim(const std::string &s);
void initHistoryPath(void);
void loadPersistentHistory(void);
void savePersistentHistory(void);
void sigchldHandler(int);
std::vector<char*> parseInput(std::string cmd);
void executeCommand(const std::string &cmd);
void loadSystemCommands(void);
char *commandGenerator(const char* text, int state);
char **myCompletion(const char *text, int start, int end);
void executePipeline(std::vector<Command> &commands, bool background, const std::string &fullCmd);
void freeArgs(std::vector<char*> &args);
Command parseSingleCommand(const std::string &cmdStr);
bool isValidName(const std::string &str);
std::string expandTile(const std::string &path);
std::string toAbsolutePath(const std::string &path);
std::string expandEnvVars(const std::string &path);
std::vector<char *> splitArgs(const std::string &cmd);
std::string expandAlias(const std::string &input);
void doAlias(const std::string &cmd2);
std::string doPrint(const std::string &input);
void doVarAssign(const std::string &cmd2, size_t eqPos);
void doAssignAlias(const std::string &cmd2);
void listJobs(void);
void fgJob(int jobId);
void bgJob(int jobId);

std::vector<std::string> commandList;
std::vector<std::string> historyList; // Command history
std::string historyPath;
std::unordered_map<std::string, std::string> aliases;
std::unordered_map<std::string, std::string> variables;

int main(void) {
  initHistoryPath();
  loadPersistentHistory();
  loadSystemCommands();
  rl_attempted_completion_function = myCompletion;
  signal(SIGCHLD, sigchldHandler);
  signal(SIGTTOU, SIG_IGN);
  while (true) {
    char *input = readline("0vershell> ");
    if (!input) { // EOF (Ctrl+D)
      std::cout << "\n";
      break;
    }
    std::string cmd2(trim(static_cast<std::string>(input)));
    free(input);
    if (cmd2.empty()) continue;
    add_history(cmd2.c_str());
    append_history(1, historyPath.c_str());
    if (cmd2 == "exit") break;
    if (cmd2 == "history") {
      HIST_ENTRY** histList = history_list();
      if (histList) {
        for (int x = 0; histList[x]; x++) { std::cout << (x + history_base) << ": " << histList[x]->line << "\n"; }
      }
      continue;
    }
    // Handle alias creation: alias name="command"
    if (cmd2.rfind("alias ", 0) == 0) {
      doAlias(cmd2);
      doAssignAlias(cmd2);
      continue;
    }
    // Handle unalias
    if (cmd2.rfind("unalias ", 0) == 0) {
      std::string name = cmd2.substr(8);
      if (aliases.erase(name)) {
        std::cout << "Alias removed: " << name << "\n";
      } else {
        std::cerr << "Alias not found: " << name << "\n";
      }
      continue;
    }
    // Expand alias if applicable
    cmd2 = expandAlias(cmd2);
    // Variable retrieval
    if (cmd2[0] == '$') {
      std::string var = cmd2.substr(1);
      for (const auto &[key, val] : variables) {
        if (key == var) {
          std::cout << val << std::endl;
          break;
        }
      }
      continue;
    }
    // Variable assignment
    size_t eqPos = cmd2.find('=');
    if (eqPos != std::string::npos) {
      doVarAssign(cmd2, eqPos);
      continue;
    }
    if (cmd2.rfind("print", 0) == 0) {
      std::cout << doPrint(cmd2.substr(5)) << std::endl;
      continue;
    }
    cmd2 = expandEnvVars(cmd2);
    // Built-in jobs
    if (cmd2 == "jobs") {
      listJobs();
      continue;
    }
    // Built-in fg or bg
    if (cmd2.rfind("fg", 0) == 0 || cmd2.rfind("bg", 0) == 0) {
      std::istringstream iss(cmd2);
      std::string cmd;
      int jobId;
      iss >> cmd >> jobId;
      if (cmd2.rfind("fg", 0) == 0) {
        fgJob(jobId);
        continue;
      }
      bgJob(jobId);
      continue;
    }
    // starts with cd
    if (cmd2.rfind("cd", 0) == 0) {
      std::string path = getenv("HOME") ? getenv("HOME") : "";
      if (cmd2.size() > 2) {
        path = cmd2.substr(3); // skip "cd "
      }
      std::filesystem::current_path(path);
      continue;
    }
    // Detect background execution
    bool background = false;
    if (!cmd2.empty() && cmd2.back() == '&') {
      background = true;
      cmd2.pop_back();
      if (!cmd2.empty() && cmd2.back() == ' ')
        cmd2.pop_back();
    }
    // Split by pipeline '|'
    std::vector<Command> commands;
    std::stringstream ss(cmd2);
    std::string segment;
    while (std::getline(ss, segment, '|')) {
      commands.push_back(parseSingleCommand(segment));
    }
    // Execute pipeline
    executePipeline(commands, background, cmd2);
    // Free allocated args
    for (auto &cmdz : commands) {
      freeArgs(cmdz.args);
    }
  }
  savePersistentHistory();
  return EXIT_SUCCESS;
}

// Alias to inherit data from other variable, e.g: alias l=$myVar
void doAssignAlias(const std::string &cmd2) {
  std::string aliasDef = cmd2.substr(6);
  size_t eqPos = aliasDef.find('=');
  unsigned int foundIt = 0U;
  if (eqPos == std::string::npos) {
    std::cerr << "Invalid alias format. Use: alias name=\"command\"\n";
    return;
  }
  std::string name = aliasDef.substr(0, eqPos);
  std::string value = aliasDef.substr(eqPos + 2);
  // Remove surrounding quotes if present
  if (!value.empty() && value.front() == '"' && value.back() == '"') {
    value = value.substr(1, value.size() - 2);
  }
  if (name.empty() || value.empty()) {
    std::cerr << "Alias name and value cannot be empty.\n";
    return;
  }
  for (const auto &[key, val] : variables) {
    if (key == value) {
      aliases[name] = val;
      foundIt = 1U;
      break;
    }
  }
  if (foundIt == 0U) {
    for (const auto &[key, val] : aliases) {
      if (key == value) {
        aliases[name] = val;
        break;
      }
    }
  }
}

// Variable assignment and if we assign it to another variable, then expand it
void doVarAssign(const std::string &cmd2, size_t eqPos) {
  unsigned int foundIt = 0U;
  std::string name = cmd2.substr(0, eqPos);
  std::string value = cmd2.substr(eqPos + 1);
  if (value.rfind('$', 0) == 0) {
    std::string str = value.substr(1);
    for (const auto &[key, val] : variables) {
      if (key == str) {
        variables[name] = val;
        foundIt = 1U;
        break;
      }
    }
  }
  if (!isValidName(name)) {
    std::cout << "Invalid variable name: " << name << "\n";
    return;
  }
  if (foundIt == 0U) {
    variables[name] = value;
  }
}

// Setup alias
void doAlias(const std::string &cmd2) {
  std::string aliasDef = cmd2.substr(6);
  size_t eqPos = aliasDef.find('=');
  if (eqPos == std::string::npos) {
    std::cerr << "Invalid alias format. Use: alias name=\"command\"\n";
    return;
  }
  std::string name = aliasDef.substr(0, eqPos);
  std::string value = aliasDef.substr(eqPos + 1);
  // Remove surrounding quotes if present
  if (!value.empty() && value.front() == '"' && value.back() == '"') {
    value = value.substr(1, value.size() - 2);
  }
  if (name.empty() || value.empty()) {
    std::cerr << "Alias name and value cannot be empty.\n";
    return;
  }
  aliases[name] = value;
  std::cout << "Alias set: " << name << " -> " << value << "\n";
}

// Function to replace variables in the input string based on a regex pattern and a map
std::string doPrint(const std::string &input) {
  //std::regex varPattern(R"(\$\{(\w+)\})"); // matches ${var}
  std::regex varPattern(R"(\$([A-Za-z_][A-Za-z0-9_]*))");
  std::string result;
  std::sregex_iterator currentMatch(input.begin(), input.end(), varPattern);
  std::sregex_iterator lastMatch;
  size_t lastPos = 0;
  while (currentMatch != lastMatch) {
    const std::smatch &match = *currentMatch;
    // Append text before the match
    result.append(input, lastPos, match.position() - lastPos);
    // Extract variable name from capture group 1
    std::string varName = match[1].str();
    // Replace with value from map if found, else empty string
    auto it = variables.find(varName);
    if (it != variables.end()) {
      result.append(it->second);
    }
    // Update last position
    lastPos = match.position() + match.length();
    ++currentMatch;
  }
  // Append remaining text after last match
  result.append(input, lastPos, std::string::npos);
  return result;
}

std::string expandTile(const std::string &path) {
  if (path.empty() || path[0] != '~') {
    return path;
  }
  return (getenv("HOME") ? getenv("HOME") : "");
}

// Function to expand environment variables in a string
std::string expandEnvVars(const std::string &path) {
  std::string result = path;
  // Pattern matches $VAR or ${VAR}
  std::regex varPattern(R"(\$([A-Za-z_][A-Za-z0-9_]*)|\$\{([A-Za-z_][A-Za-z0-9_]*)\})");
  std::smatch match;
  // Search and replace all matches
  auto searchStart = result.cbegin();
  while (std::regex_search(searchStart, result.cend(), match, varPattern)) {
    std::string varName = match[1].matched ? match[1].str() : match[2].str();
    const char *envValue = std::getenv(varName.c_str());
    // Replace with value or empty string if not found
    result.replace(match.position(0), match.length(0), envValue ? envValue : "");
    // Move search start forward
    searchStart = result.cbegin() + match.position(0) + (envValue ? std::string(envValue).length() : 0);
  }
  return result;
}

std::string toAbsolutePath(const std::string &path) {
  try {
    return std::filesystem::absolute(path).string();
  } catch (const std::exception &e) {
    throw std::runtime_error(std::string("Error: ") + e.what());
  }
}

// Free allocated args
void freeArgs(std::vector<char*> &args) {
  for (char* arg : args) delete[] arg;
  args.clear();
}

// Parse a single command string into Command struct
Command parseSingleCommand(const std::string &cmdStr) {
  Command cmd;
  std::stringstream ss(cmdStr);
  std::string token;
  while (ss >> token) {
    if (token == "<") {
      ss >> cmd.infile;
    } else if (token == ">") {
      ss >> cmd.outfile;
      cmd.append = false;
    } else if (token == ">>") {
      ss >> cmd.outfile;
      cmd.append = true;
    } else {
      char *arg = new char[token.size() + 1];
      std::strcpy(arg, token.c_str());
      cmd.args.push_back(arg);
    }
  }
  cmd.args.push_back(nullptr);
  return cmd;
}

// List jobs
void listJobs() {
  for (const auto &[key, val]: jobs) {
    std::cout << "[" << key << "] "
    << (val.running ? "Running" : "Stopped")
    << " " << val.command
    << " (PGID " << val.pgid << ")\n";
  }
}

// Bring job to foreground
void fgJob(int jobId) {
  if (jobs.find(jobId) == jobs.end()) {
    std::cerr << "fg: no such job\n";
    return;
  }
  Job &job = jobs[jobId];
  tcsetpgrp(STDIN_FILENO, job.pgid);
  kill(-job.pgid, SIGCONT);
  int status;
  waitpid(-job.pgid, &status, WUNTRACED);
  tcsetpgrp(STDIN_FILENO, getpgrp());
  if (WIFSTOPPED(status)) {
    job.running = false;
  } else {
    jobs.erase(jobId);
  }
}

// Resume job in background
void bgJob(int jobId) {
  if (jobs.find(jobId) == jobs.end()) {
    std::cerr << "bg: no such job\n";
    return;
  }
  Job &job = jobs[jobId];
  kill(-job.pgid, SIGCONT);
  job.running = true;
}

// Signal handler for background process completion
void sigchldHandler(int) {
  int status;
  pid_t pid;
  while ((pid = waitpid(-1, &status, WNOHANG)) > 0) {
    for (auto it = jobs.begin(); it != jobs.end();) {
      if (it->second.pgid == pid) {
        std::cout << "\n[" << it->first << "] Done " << it->second.command << "\n";
        it = jobs.erase(it);
      } else {
        ++it;
      }
    }
  }
}

// ======== Utility Functions ========

// Expand alias if it exists
std::string expandAlias(const std::string &input) {
  std::istringstream iss(input);
  std::string firstWord;
  iss >> firstWord;
  auto it = aliases.find(firstWord);
  if (it != aliases.end()) {
    // Replace first word with alias value
    std::string rest;
    std::getline(iss, rest);
    return it->second + rest;
  }
  return input;
}

// Check if string is valid variable name
bool isValidName(const std::string &str) {
  if (str.empty() || std::isdigit(str[0])) {
    return false;
  }
  for (char ch : str) {
    if (!std::isalnum(ch) && ch != '_') {
      return false;
    }
  }
  return true;
}

// Trim whitespace from both ends
std::string trim(const std::string &s) {
  size_t start = s.find_first_not_of(" \t\n\r");
  size_t end = s.find_last_not_of(" \t\n\r");
  return (start == std::string::npos) ? "" : s.substr(start, end - start + 1); }

  // Build history file path
  void initHistoryPath(void) {
    const char *home = getenv("HOME");
    if (!home) home = ".";
    historyPath = std::string(home) + "/.0vershell.txt";
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

  std::vector<char *> splitArgs(const std::string &cmd) {
    std::istringstream iss(cmd);
    std::string token;
    std::vector<char *> args;
    while (iss >> token) {
      char *arg = new char[token.size() + 1];
      std::strcpy(arg, token.c_str());
      args.push_back(arg);
    }
    args.push_back(nullptr);
    return args;
  }

  // Execute a single command with optional redirection
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
    char *pathEnv = getenv("PATH");
    if (!pathEnv) return;
    std::string pathCopy(pathEnv);
    std::istringstream iss(pathCopy);
    std::string dir;
    while (std::getline(iss, dir, ':')) {
      DIR* dp = opendir(dir.c_str());
      if (dp) {
        struct dirent *entry;
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
      const std::string &name = commandList[listIndex++];
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
  void executePipeline(std::vector<Command> &commands, bool background, const std::string &fullCmd) {
    int numCmds = commands.size();
    int pipefds[2 * (numCmds - 1)];
    // Create pipes
    for (int i = 0; i < numCmds - 1; i++) {
      if (pipe(pipefds + i*2) < 0) {
        perror("pipe");
        return;
      }
    }
    pid_t pgid = 0;
    for (int i = 0; i < numCmds; i++) {
      pid_t pid = fork();
      if (pid == 0) {
        // Child process
        if (pgid == 0) pgid = getpid();
        setpgid(0, pgid);
        // Redirect input from previous pipe
        if (i > 0) dup2(pipefds[(i-1)*2], STDIN_FILENO);
        // Redirect output to next pipe
        if (i < numCmds - 1) dup2(pipefds[i*2 + 1], STDOUT_FILENO);
        // Close all pipe fds
        for (int j = 0; j < 2*(numCmds-1); j++) close(pipefds[j]);
        // Handle input redirection
        if (!commands[i].infile.empty()) {
          int fd = open(commands[i].infile.c_str(), O_RDONLY);
          if (fd < 0) { perror("open infile"); exit(1); }
          dup2(fd, STDIN_FILENO);
          close(fd);
        }
        // Handle output redirection
        if (!commands[i].outfile.empty()) {
          int fd = open(commands[i].outfile.c_str(), O_WRONLY | O_CREAT | (commands[i].append ? O_APPEND : O_TRUNC), 0644);
          if (fd < 0) { perror("open outfile"); exit(1); }
          dup2(fd, STDOUT_FILENO);
          close(fd);
        }
        std::string expanded = expandEnvVars(commands[i].args[0]);
        std::vector<char *> args = splitArgs(expanded);
        // Execute command
        if (execvp(commands[i].args[0], commands[i].args.data()) == -1) {
          perror("execvp");
          exit(1);
        }
      }
      else if (pid > 0) {
        if (pgid == 0) pgid = pid;
        setpgid(pid, pgid);
      }
      else {
        perror("fork");
      }
    }
    // Close all pipe fds in parent
    for (int i = 0; i < 2*(numCmds-1); i++) close(pipefds[i]);
    if (background) {
      jobs[nextJobId] = {pgid, fullCmd, true};
      std::cout << "[" << nextJobId << "] " << pgid << "\n";
      nextJobId++;
    } else {
      tcsetpgrp(STDIN_FILENO, pgid);
      int status;
      waitpid(-pgid, &status, WUNTRACED);
      tcsetpgrp(STDIN_FILENO, getpgrp());
      if (WIFSTOPPED(status)) {
        jobs[nextJobId] = {pgid, fullCmd, false};
        std::cout << "[" << nextJobId << "] Stopped " << fullCmd << "\n";
        nextJobId++;
      }
    }
  }