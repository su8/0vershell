/*
 * Copyright 10/31/2026 12/03/2025 https://github.com/su8/0vershell
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
 *
 * Compile with: g++ -std=c++17 -Wall -Wextra -O2 -o 0vershell main.cpp -lreadline
 *
 * How a command line is processed (each command of a list, in this order):
 *   1. aliases           the first word of every command of a pipeline
 *   2. `cmd` and $(cmd)  are executed FIRST, their output is kept apart (it is never read again as shell
 *                        syntax) and is put in the line at the end, where it was
 *   3. the expansions    quotes, $var ${var} $? $$, ~, word splitting, * ? [ ]
 *   4. the command       builtin, or fork and exec, with pipes and redirections
 *
 * Compound commands are recognised at list level, before step 1 of their bodies, and their
 * bodies / conditions are fed back through the same pipeline:
 *
 *   for NAME [in WORDS]; do BODY; done
 *   while COND; do BODY; done
 *   until COND; do BODY; done
 *   if COND; then BODY [elif COND; then BODY]... [else BODY]; fi
 *
 * Compound commands can be nested, can be followed by ; && || and can span several lines
 * (a newline works like ;).  The condition of while / until / if is any command list, its last
 * exit status decides.  There is no `test` builtin: use the external `test` or `[`.
 *
 * There is no print command: write  echo $var  as in every shell.
 */

#include <algorithm>
#include <cerrno>
#include <csignal>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <iostream>
#include <map>
#include <set>
#include <string>
#include <unordered_map>
#include <utility>
#include <vector>
#include <dirent.h>
#include <fcntl.h>
#include <glob.h>
#include <pwd.h>
#include <sys/stat.h>
#include <sys/types.h>
#include <sys/wait.h>
#include <unistd.h>
#include <readline/readline.h>
#include <readline/history.h>

extern char **environ;

// ---------------------------------------------------------------------------
// Data
// ---------------------------------------------------------------------------

enum RedirectKind { R_IN, R_OUT, R_APPEND, R_DUP };

struct Redirect {
  int fd;
  RedirectKind kind;
  std::string path;
  int dupFd;
  Redirect() : fd(0), kind(R_IN), dupFd(-1) {}
};

struct Stage {
  std::vector<std::pair<std::string, std::string> > assigns;
  std::vector<std::string> argv;
  std::vector<Redirect> redirs;
  bool empty() const { return assigns.empty() && argv.empty() && redirs.empty(); }
};

struct Job {
  pid_t pgid;
  std::string command;
  bool running;
  std::vector<pid_t> pids;
  Job() : pgid(0), running(true) {}
};

// A for / while / until / if compound command found in a line
struct Compound {
  size_t begin;
  size_t end;
  std::string keyword;      // "for", "while", "until", "if"
  std::string var;          // for: loop variable
  std::string itemsText;    // for: raw text after `in`, or "" for `for x; do`
  bool haveIn;
  std::string cond;         // while/until: the condition text
  std::string body;         // for/while/until: the loop body
  // if: the (condition, body) of the `if` and of every `elif`, in order
  std::vector<std::pair<std::string, std::string> > clauses;
  std::string elseBody;     // if: the `else` part
  bool haveElse;
  Compound() : begin(0), end(0), haveIn(false), haveElse(false) {}
};

struct ListItem {
  std::string text;
  char op;
  bool isCompound;
  Compound compound;
  ListItem() : op(0), isCompound(false) {}
};

static std::map<int, Job> jobs;
static int nextJobId = 1;
static std::vector<std::string> commandList;
static std::string historyPath;
static std::unordered_map<std::string, std::string> aliases;
static std::unordered_map<std::string, std::string> variables;
static bool gInteractive = false;
static bool gSubshell = false;
static bool gExit = false;
static int gExitCode = 0;
static int gLastStatus = 0;
static int gSubstDepth = 0;
static pid_t gShellPgid = 0;
static pid_t gShellPid = 0;        // the pid `$$` returns, even inside a subshell

// Loop state
static int gLoopDepth = 0;
static int gBreakLevel = 0;        // break N
static int gContinueLevel = 0;     // continue N

static void runLine(const std::string &line);

// ---------------------------------------------------------------------------
// Small helpers
// ---------------------------------------------------------------------------

static void err(const std::string &msg) { std::cerr << "0vershell: " << msg << "\n"; }

static std::string trim(const std::string &s) {
  const size_t start = s.find_first_not_of(" \t\n\r");
  if (start == std::string::npos) { return ""; }
  return s.substr(start, s.find_last_not_of(" \t\n\r") - start + 1);
}

static bool isBlank(char c) { return c == ' ' || c == '\t' || c == '\n' || c == '\r'; }
static bool isNameStart(char c) { return (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') || c == '_'; }
static bool isNameChar(char c) { return isNameStart(c) || (c >= '0' && c <= '9'); }

static bool isValidName(const std::string &s) {
  if (s.empty() || !isNameStart(s[0])) { return false; }
  for (size_t i = 1; i < s.size(); i++) { if (!isNameChar(s[i])) { return false; } }
  return true;
}

static void flushOutput() {
  std::cout.flush();
  std::fflush(NULL);
}

static std::string lookupVar(const std::string &name) {
  if (name == "?") { return std::to_string(gLastStatus); }
  if (name == "$") { return std::to_string(static_cast<long>(gShellPid)); }
  if (name == "0") { return "0vershell"; }
  std::unordered_map<std::string, std::string>::const_iterator it = variables.find(name);
  if (it != variables.end()) { return it->second; }
  const char *env = std::getenv(name.c_str());
  return env ? env : "";
}

static void setVariable(const std::string &name, const std::string &value) {
  variables[name] = value;
  if (std::getenv(name.c_str())) { setenv(name.c_str(), value.c_str(), 1); }
}

// ---------------------------------------------------------------------------
// Quote / $( ) / ` ` matching (used by every phase)
// ---------------------------------------------------------------------------

static size_t matchParen(const std::string &t, size_t open);
static size_t skipDouble(const std::string &t, size_t i);

static size_t matchBacktick(const std::string &t, size_t i) {
  for (size_t j = i + 1; j < t.size(); j++) {
    if (t[j] == '\\') { j++; continue; }
    if (t[j] == '`') { return j; }
  }
  return std::string::npos;
}

static size_t matchParen(const std::string &t, size_t open) {
  int depth = 0;
  for (size_t i = open; i < t.size(); i++) {
    const char c = t[i];
    if (c == '\\') { i++; continue; }
    if (c == '\'') { const size_t e = t.find('\'', i + 1); if (e == std::string::npos) { return e; } i = e; continue; }
    if (c == '"') { const size_t e = skipDouble(t, i); if (e == std::string::npos) { return e; } i = e; continue; }
    if (c == '`') { const size_t e = matchBacktick(t, i); if (e == std::string::npos) { return e; } i = e; continue; }
    if (c == '(') { depth++; }
    else if (c == ')') { if (--depth == 0) { return i; } }
  }
  return std::string::npos;
}

static size_t skipDouble(const std::string &t, size_t i) {
  for (size_t j = i + 1; j < t.size(); j++) {
    const char c = t[j];
    if (c == '\\') { j++; continue; }
    if (c == '"') { return j; }
    if (c == '`') { const size_t e = matchBacktick(t, j); if (e == std::string::npos) { return e; } j = e; continue; }
    if (c == '$' && j + 1 < t.size() && t[j + 1] == '(') { const size_t e = matchParen(t, j + 1); if (e == std::string::npos) { return e; } j = e; continue; }
  }
  return std::string::npos;
}

// ---------------------------------------------------------------------------
// Compound commands (for / while / until / if) — parsing helpers
// ---------------------------------------------------------------------------

// A reserved word must be followed by a blank, end of text or an operator character,
// so that `done.sh` or `fi-x` are ordinary command names.
static bool wordAt(const std::string &t, size_t pos, const char *word) {
  const size_t n = std::strlen(word);
  if (pos > t.size() || t.compare(pos, n, word) != 0) { return false; }
  const size_t e = pos + n;
  return e >= t.size() || isBlank(t[e]) || t[e] == ';' || t[e] == '&' || t[e] == '|' ||
         t[e] == '(' || t[e] == ')' || t[e] == '<' || t[e] == '>';
}

static size_t skipSpace(const std::string &t, size_t i) {
  while (i < t.size() && isBlank(t[i])) { i++; }
  return i;
}

static bool atCommandBoundary(const std::string &t, size_t p, size_t floor) {
  size_t q = p;
  // only spaces and tabs are skipped: a newline is itself a command boundary
  while (q > floor && (t[q - 1] == ' ' || t[q - 1] == '\t' || t[q - 1] == '\r')) { q--; }
  if (q == floor || q == 0) { return true; }
  const char b = t[q - 1];
  return b == ';' || b == '\n' || b == '(' || b == '{' || b == '|' || b == '&';
}

static bool isCompoundStart(const std::string &t, size_t p) {
  return wordAt(t, p, "for") || wordAt(t, p, "while") || wordAt(t, p, "until") || wordAt(t, p, "if");
}

static bool parseCompoundAt(const std::string &t, size_t i, Compound &c, std::string &error);

// Find the first of `words` that stands where a command starts, at the same nesting level as `from`:
// quotes, $( ), ` ` and whole nested compound commands are skipped.  `which` is the index of the word found.
static size_t scanFor(const std::string &t, size_t from, const char *const *words, size_t count, size_t &which, std::string &error) {
  size_t p = from;
  while (p < t.size()) {
    const char ch = t[p];
    if (ch == '\\') { p += 2; continue; }
    if (ch == '\'') { const size_t e = t.find('\'', p + 1); if (e == std::string::npos) { error = "syntax error: unterminated single quote"; return std::string::npos; } p = e + 1; continue; }
    if (ch == '"')  { const size_t e = skipDouble(t, p);    if (e == std::string::npos) { error = "syntax error: unterminated double quote"; return std::string::npos; } p = e + 1; continue; }
    if (ch == '`')  { const size_t e = matchBacktick(t, p); if (e == std::string::npos) { error = "syntax error: unterminated backtick"; return std::string::npos; } p = e + 1; continue; }
    if (ch == '$' && p + 1 < t.size() && t[p + 1] == '(') {
      const size_t e = matchParen(t, p + 1);
      if (e == std::string::npos) { error = "syntax error: unterminated $("; return std::string::npos; }
      p = e + 1; continue;
    }
    if (ch == '#' && (p == from || isBlank(t[p - 1]))) {
      while (p < t.size() && t[p] != '\n') { p++; }
      continue;
    }
    if (isNameStart(ch) && atCommandBoundary(t, p, from)) {
      if (isCompoundStart(t, p)) {
        Compound inner;
        if (!parseCompoundAt(t, p, inner, error)) { return std::string::npos; }
        p = inner.end;
        continue;
      }
      for (size_t w = 0; w < count; w++) {
        if (wordAt(t, p, words[w])) { which = w; return p; }
      }
    }
    p++;
  }
  return std::string::npos;
}

static void stripTrailingSemicolon(std::string &s) {
  while (!s.empty()) {
    const char c = s[s.size() - 1];
    if (c == ';' || isBlank(c)) { s.erase(s.size() - 1); } else { break; }
  }
}

static bool parseIfAt(const std::string &t, size_t i, Compound &c, std::string &error) {
  static const char *const thenWord[] = { "then" };
  static const char *const afterBody[] = { "elif", "else", "fi" };
  static const char *const fiWord[] = { "fi" };
  c.keyword = "if";
  size_t which = 0;
  size_t k = i + 2;
  while (true) {
    const size_t thenPos = scanFor(t, k, thenWord, 1, which, error);
    if (thenPos == std::string::npos) { if (error.empty()) { error = "syntax error: if: then not found"; } return false; }
    std::string cond = trim(t.substr(k, thenPos - k));
    stripTrailingSemicolon(cond);
    if (cond.empty()) { error = "syntax error: if: the condition is missing"; return false; }
    const size_t bodyStart = skipSpace(t, thenPos + 4);
    const size_t stop = scanFor(t, bodyStart, afterBody, 3, which, error);
    if (stop == std::string::npos) { if (error.empty()) { error = "syntax error: if: fi not found"; } return false; }
    const std::string body = t.substr(bodyStart, stop - bodyStart);
    if (trim(body).empty()) { error = "syntax error: if: the command list after then is empty"; return false; }
    c.clauses.push_back(std::make_pair(cond, body));
    if (which == 0) { k = stop + 4; continue; }          // elif: another condition follows
    if (which == 1) {                                    // else
      const size_t elseStart = skipSpace(t, stop + 4);
      const size_t fiPos = scanFor(t, elseStart, fiWord, 1, which, error);
      if (fiPos == std::string::npos) { if (error.empty()) { error = "syntax error: if: fi not found"; } return false; }
      c.elseBody = t.substr(elseStart, fiPos - elseStart);
      if (trim(c.elseBody).empty()) { error = "syntax error: if: the command list after else is empty"; return false; }
      c.haveElse = true;
      c.end = fiPos + 2;
      return true;
    }
    c.end = stop + 2;                                    // fi
    return true;
  }
}

static bool parseCompoundAt(const std::string &t, size_t i, Compound &c, std::string &error) {
  static const char *const doWord[] = { "do" };
  static const char *const doneWord[] = { "done" };
  c.begin = i;
  if (wordAt(t, i, "if")) { return parseIfAt(t, i, c, error); }
  size_t which = 0;
  if (wordAt(t, i, "for")) {
    c.keyword = "for";
    size_t k = skipSpace(t, i + 3);
    if (k >= t.size() || !isNameStart(t[k])) { error = "syntax error: for: a variable name is needed"; return false; }
    size_t v = k;
    while (v < t.size() && isNameChar(t[v])) { v++; }
    c.var = t.substr(k, v - k);
    k = skipSpace(t, v);
    c.haveIn = false;
    if (k < t.size() && wordAt(t, k, "in")) {
      c.haveIn = true;
      k = skipSpace(t, k + 2);
    }
    const size_t doPos = scanFor(t, k, doWord, 1, which, error);
    if (doPos == std::string::npos) { if (error.empty()) { error = "syntax error: for: do not found"; } return false; }
    if (c.haveIn) {
      c.itemsText = trim(t.substr(k, doPos - k));
      stripTrailingSemicolon(c.itemsText);
    }
    const size_t afterDo = skipSpace(t, doPos + 2);
    const size_t donePos = scanFor(t, afterDo, doneWord, 1, which, error);
    if (donePos == std::string::npos) { if (error.empty()) { error = "syntax error: for: done not found"; } return false; }
    c.body = t.substr(afterDo, donePos - afterDo);
    c.end = donePos + 4;
    return true;
  }
  const bool isWhile = wordAt(t, i, "while");
  const char *kw = isWhile ? "while" : "until";
  c.keyword = kw;
  const size_t k = i + std::strlen(kw);
  const size_t doPos = scanFor(t, k, doWord, 1, which, error);
  if (doPos == std::string::npos) { if (error.empty()) { error = c.keyword + ": do not found"; error = "syntax error: " + error; } return false; }
  c.cond = trim(t.substr(k, doPos - k));
  stripTrailingSemicolon(c.cond);
  if (c.cond.empty()) { error = "syntax error: " + c.keyword + ": the condition is missing"; return false; }
  const size_t afterDo = skipSpace(t, doPos + 2);
  const size_t donePos = scanFor(t, afterDo, doneWord, 1, which, error);
  if (donePos == std::string::npos) { if (error.empty()) { error = "syntax error: " + c.keyword + ": done not found"; } return false; }
  c.body = t.substr(afterDo, donePos - afterDo);
  c.end = donePos + 4;
  return true;
}

// ---------------------------------------------------------------------------
// The line is cut into commands at ; newline & && ||  (not in quotes, not inside $( ) or ` `).
// A compound at a command boundary is kept whole in a single ListItem.
// ---------------------------------------------------------------------------

static bool splitList(const std::string &s, std::vector<ListItem> &items, std::string &error) {
  std::string cur;
  size_t i = 0;
  const size_t n = s.size();
  while (i < n) {
    // A compound command begins here (at the start of the current item, after trimming)?
    if (trim(cur).empty() && isNameStart(s[i]) && isCompoundStart(s, i) && atCommandBoundary(s, i, 0)) {
      Compound c;
      if (!parseCompoundAt(s, i, c, error)) { return false; }
      ListItem item;
      item.text = s.substr(i, c.end - i);
      item.isCompound = true;
      item.compound = c;
      i = c.end;
      while (i < n && (s[i] == ' ' || s[i] == '\t' || s[i] == '\r')) { i++; }
      char op = 0;
      size_t used = 1;
      if (i < n) {
        const char ch = s[i];
        if (ch == ';' || ch == '\n') { op = ';'; }
        else if (ch == '&' && i + 1 < n && s[i + 1] == '&') { op = 'a'; used = 2; }
        else if (ch == '&') { op = '&'; }
        else if (ch == '|' && i + 1 < n && s[i + 1] == '|') { op = 'o'; used = 2; }
        else if (ch == '#') { while (i < n && s[i] != '\n') { i++; } op = ';'; used = 0; }
        else { error = std::string("syntax error near '") + ch + "' after the end of a compound command"; return false; }
        i += used;
      }
      item.op = op;
      items.push_back(item);
      cur.clear();
      continue;
    }

    const char c = s[i];
    char op = 0;
    size_t used = 1;
    if (c == '\\') { cur += c; if (i + 1 < n) { cur += s[i + 1]; } i += 2; continue; }
    if (c == '\'') {
      const size_t e = s.find('\'', i + 1);
      if (e == std::string::npos) { error = "syntax error: unterminated single quote"; return false; }
      cur.append(s, i, e - i + 1); i = e + 1; continue;
    }
    if (c == '"') {
      const size_t e = skipDouble(s, i);
      if (e == std::string::npos) { error = "syntax error: unterminated double quote"; return false; }
      cur.append(s, i, e - i + 1); i = e + 1; continue;
    }
    if (c == '`') {
      const size_t e = matchBacktick(s, i);
      if (e == std::string::npos) { error = "syntax error: unterminated backtick"; return false; }
      cur.append(s, i, e - i + 1); i = e + 1; continue;
    }
    if (c == '$' && i + 1 < n && s[i + 1] == '(') {
      const size_t e = matchParen(s, i + 1);
      if (e == std::string::npos) { error = "syntax error: unterminated $("; return false; }
      cur.append(s, i, e - i + 1); i = e + 1; continue;
    }
    if (c == '\n' && trim(cur).empty()) { cur.clear(); i++; continue; }
    if (c == '#' && (cur.empty() || isBlank(cur[cur.size() - 1]))) {
      while (i < n && s[i] != '\n') { i++; }      // a comment runs to the end of the line
      continue;
    }
    if (c == ';' || c == '\n') { op = ';'; }
    else if (c == '&' && i + 1 < n && s[i + 1] == '&') { op = 'a'; used = 2; }
    else if (c == '&' && !cur.empty() && (cur[cur.size() - 1] == '>' || cur[cur.size() - 1] == '<')) { cur += c; i++; continue; }
    else if (c == '&') { op = '&'; }
    else if (c == '|' && i + 1 < n && s[i + 1] == '|') { op = 'o'; used = 2; }
    if (!op) { cur += c; i++; continue; }
    ListItem item;
    item.text = trim(cur);
    item.op = op;
    if (item.text.empty()) { error = std::string("syntax error near '") + c + "'"; return false; }
    items.push_back(item);
    cur.clear();
    i += used;
  }
  const std::string last = trim(cur);
  if (last.empty()) {
    if (!items.empty() && (items.back().op == 'a' || items.back().op == 'o')) { error = "syntax error: the command after && or || is missing"; return false; }
    return true;
  }
  ListItem item;
  item.text = last;
  item.op = 0;
  items.push_back(item);
  return true;
}

// ---------------------------------------------------------------------------
// 1. Aliases
// ---------------------------------------------------------------------------

static size_t nextPipe(const std::string &t, size_t from) {
  for (size_t i = from; i < t.size(); i++) {
    const char c = t[i];
    if (c == '\\') { i++; continue; }
    if (c == '\'') { const size_t e = t.find('\'', i + 1); if (e == std::string::npos) { return e; } i = e; continue; }
    if (c == '"') { const size_t e = skipDouble(t, i); if (e == std::string::npos) { return e; } i = e; continue; }
    if (c == '`') { const size_t e = matchBacktick(t, i); if (e == std::string::npos) { return e; } i = e; continue; }
    if (c == '$' && i + 1 < t.size() && t[i + 1] == '(') { const size_t e = matchParen(t, i + 1); if (e == std::string::npos) { return e; } i = e; continue; }
    if (c == '|') { return i; }
  }
  return std::string::npos;
}

static std::string expandAliases(const std::string &input) {
  if (aliases.empty()) { return input; }
  std::string t = input;
  size_t pos = 0;
  std::set<std::string> seen;
  while (pos < t.size()) {
    while (pos < t.size() && isBlank(t[pos])) { pos++; }
    size_t end = pos;
    while (end < t.size() && (isNameChar(t[end]) || std::strchr(".+-:@%,", t[end]))) { end++; }
    const bool plain = end > pos && (end == t.size() || isBlank(t[end]) || t[end] == '|' || t[end] == '<' || t[end] == '>');
    if (plain) {
      const std::string word = t.substr(pos, end - pos);
      std::unordered_map<std::string, std::string>::const_iterator it = aliases.find(word);
      if (it != aliases.end() && !seen.count(word)) {
        seen.insert(word);
        t.replace(pos, end - pos, it->second);
        continue;
      }
    }
    const size_t pipe = nextPipe(t, pos);
    if (pipe == std::string::npos) { break; }
    pos = pipe + 1;
    seen.clear();
  }
  return t;
}

// ---------------------------------------------------------------------------
// 2. Command substitution
// ---------------------------------------------------------------------------

static std::string captureCommand(const std::string &cmd, int &status) {
  status = 0;
  int fd[2];
  if (pipe(fd) != 0) { err(std::string("pipe: ") + std::strerror(errno)); status = 1; return ""; }
  flushOutput();
  const pid_t pid = fork();
  if (pid < 0) { err(std::string("fork: ") + std::strerror(errno)); close(fd[0]); close(fd[1]); status = 1; return ""; }
  if (pid == 0) {
    close(fd[0]);
    dup2(fd[1], STDOUT_FILENO);
    close(fd[1]);
    gSubshell = true;
    gInteractive = false;
    gBreakLevel = 0; gContinueLevel = 0; gLoopDepth = 0;
    if (++gSubstDepth > 16) { err("command substitutions are nested too deep"); _exit(2); }
    signal(SIGINT, SIG_DFL);
    signal(SIGQUIT, SIG_DFL);
    signal(SIGTSTP, SIG_DFL);
    signal(SIGTTIN, SIG_DFL);
    signal(SIGTTOU, SIG_DFL);
    runLine(cmd);
    flushOutput();
    _exit(gExit ? gExitCode : (gLastStatus & 0xFF));
  }
  close(fd[1]);
  std::string out;
  char buf[4096];
  ssize_t got;
  while ((got = read(fd[0], buf, sizeof(buf))) != 0) {
    if (got < 0) { if (errno == EINTR) { continue; } break; }
    out.append(buf, static_cast<size_t>(got));
  }
  close(fd[0]);
  int st = 0;
  while (waitpid(pid, &st, 0) < 0 && errno == EINTR) { }
  status = WIFEXITED(st) ? WEXITSTATUS(st) : 128 + WTERMSIG(st);
  while (!out.empty() && out[out.size() - 1] == '\n') { out.erase(out.size() - 1); }
  return out;
}

static bool substituteCommands(const std::string &text, std::vector<std::string> &outputs, std::string &result, std::string &error) {
  result.clear();
  bool inSingle = false, inDouble = false;
  for (size_t i = 0; i < text.size();) {
    const char c = text[i];
    if (inSingle) { result += c; if (c == '\'') { inSingle = false; } i++; continue; }
    if (c == '\\') { result += c; if (i + 1 < text.size()) { result += text[i + 1]; } i += 2; continue; }
    if (c == '\'' && !inDouble) { inSingle = true; result += c; i++; continue; }
    if (c == '"') { inDouble = !inDouble; result += c; i++; continue; }
    std::string inner;
    size_t next = 0;
    bool found = false;
    if (c == '$' && i + 1 < text.size() && text[i + 1] == '(' && !(i + 2 < text.size() && text[i + 2] == '(')) {
      const size_t close = matchParen(text, i + 1);
      if (close == std::string::npos) { error = "syntax error: unterminated $("; return false; }
      inner = text.substr(i + 2, close - i - 2);
      next = close + 1;
      found = true;
    } else if (c == '`') {
      const size_t close = matchBacktick(text, i);
      if (close == std::string::npos) { error = "syntax error: unterminated backtick"; return false; }
      for (size_t k = i + 1; k < close; k++) {
        if (text[k] == '\\' && k + 1 < close && (text[k + 1] == '`' || text[k + 1] == '$' || text[k + 1] == '\\')) { k++; }
        inner += text[k];
      }
      next = close + 1;
      found = true;
    }
    if (!found) { result += c; i++; continue; }
    int status = 0;
    outputs.push_back(captureCommand(inner, status));
    gLastStatus = status;
    result += '\x01';
    result += std::to_string(outputs.size() - 1);
    result += '\x02';
    i = next;
  }
  return true;
}

// ---------------------------------------------------------------------------
// 3. Expansions and words
// ---------------------------------------------------------------------------

struct Field {
  std::string text;
  std::string pattern;
  bool glob;
  Field() : glob(false) {}
};

struct WordBuilder {
  std::vector<Field> done;
  Field cur;
  bool started;
  WordBuilder() : started(false) {}
  void literal(const std::string &s, bool quoted) {
    cur.text += s;
    if (quoted) {
      for (size_t i = 0; i < s.size(); i++) {
        if (s[i] == '*' || s[i] == '?' || s[i] == '[' || s[i] == '\\') { cur.pattern += '\\'; }
        cur.pattern += s[i];
      }
    } else {
      cur.pattern += s;
      if (s.find_first_of("*?[") != std::string::npos) { cur.glob = true; }
    }
    started = true;
  }
  void flush() {
    if (started) { done.push_back(cur); }
    cur = Field();
    started = false;
  }
  void splitValue(const std::string &v) {
    size_t i = 0;
    while (i < v.size()) {
      if (isBlank(v[i])) {
        flush();
        while (i < v.size() && isBlank(v[i])) { i++; }
      } else {
        size_t j = i;
        while (j < v.size() && !isBlank(v[j])) { j++; }
        literal(v.substr(i, j - i), true);
        i = j;
      }
    }
  }
};

static bool parseRedirectAt(const std::string &text, size_t &i, int fd, bool haveFd, Stage &st, const std::vector<std::string> &outputs, std::string &error);

class Parser {
 public:
  Parser(const std::string &t, const std::vector<std::string> &o) : text(t), outputs(o), i(0) {}
  bool parse(std::vector<Stage> &stages, std::string &error);
  bool parseWord(WordBuilder &wb, bool assignment, std::string &error);
 private:
  const std::string &text;
  const std::vector<std::string> &outputs;
  size_t i;
  bool parseDouble(WordBuilder &wb, std::string &error);
  bool expandDollar(WordBuilder &wb, bool quoted, bool assignment, std::string &error);
  void placeholder(WordBuilder &wb, bool quoted, bool assignment);
  void tilde(WordBuilder &wb);
  friend bool parseRedirectAt(const std::string &, size_t &, int, bool, Stage &, const std::vector<std::string> &, std::string &);
};

void Parser::placeholder(WordBuilder &wb, bool quoted, bool assignment) {
  size_t j = i + 1;
  size_t index = 0;
  while (j < text.size() && text[j] >= '0' && text[j] <= '9') { index = index * 10 + static_cast<size_t>(text[j] - '0'); j++; }
  if (j < text.size() && text[j] == '\x02') { j++; }
  const std::string value = index < outputs.size() ? outputs[index] : std::string();
  if (quoted || assignment) { wb.literal(value, true); } else { wb.splitValue(value); }
  i = j;
}

void Parser::tilde(WordBuilder &wb) {
  size_t j = i + 1;
  while (j < text.size() && (isNameChar(text[j]) || text[j] == '.' || text[j] == '-')) { j++; }
  const bool ends = j >= text.size() || text[j] == '/' || isBlank(text[j]) || text[j] == '|' || text[j] == '<' || text[j] == '>';
  if (ends) {
    const std::string user = text.substr(i + 1, j - i - 1);
    std::string home;
    if (user.empty()) {
      const char *h = std::getenv("HOME");
      if (h && *h) { home = h; } else { struct passwd *pw = getpwuid(getuid()); if (pw) { home = pw->pw_dir; } }
    } else {
      struct passwd *pw = getpwnam(user.c_str());
      if (pw) { home = pw->pw_dir; }
    }
    if (!home.empty()) { wb.literal(home, true); i = j; return; }
  }
  wb.literal("~", true);
  i++;
}

bool Parser::expandDollar(WordBuilder &wb, bool quoted, bool assignment, std::string &error) {
  if (i + 1 >= text.size()) { wb.literal("$", true); i++; return true; }
  const char d = text[i + 1];
  std::string name;
  if (d == '{') {
    const size_t e = text.find('}', i + 2);
    if (e == std::string::npos) { error = "syntax error: ${ without }"; return false; }
    name = text.substr(i + 2, e - i - 2);
    if (!isValidName(name) && name != "?" && name != "$") { error = "${" + name + "}: bad substitution"; return false; }
    i = e + 1;
  } else if (d == '?' || d == '$' || d == '0') {
    name = std::string(1, d);
    i += 2;
  } else if (isNameStart(d)) {
    size_t j = i + 1;
    while (j < text.size() && isNameChar(text[j])) { j++; }
    name = text.substr(i + 1, j - i - 1);
    i = j;
  } else if (d == '(') {
    error = "$(( )) and an unfinished $( are not supported";
    return false;
  } else {
    wb.literal("$", true);
    i++;
    return true;
  }
  const std::string value = lookupVar(name);
  if (quoted || assignment) { wb.literal(value, true); } else { wb.splitValue(value); }
  return true;
}

bool Parser::parseDouble(WordBuilder &wb, std::string &error) {
  i++;
  wb.started = true;
  while (i < text.size()) {
    const char c = text[i];
    if (c == '"') { i++; return true; }
    if (c == '\\' && i + 1 < text.size()) {
      const char nx = text[i + 1];
      if (nx == '$' || nx == '"' || nx == '\\' || nx == '`') { wb.literal(std::string(1, nx), true); i += 2; continue; }
      if (nx == '\n') { i += 2; continue; }
      wb.literal("\\", true); i++; continue;
    }
    if (c == '$') { if (!expandDollar(wb, true, false, error)) { return false; } continue; }
    if (c == '\x01') { placeholder(wb, true, false); continue; }
    wb.literal(std::string(1, c), true);
    i++;
  }
  error = "syntax error: unterminated double quote";
  return false;
}

bool Parser::parseWord(WordBuilder &wb, bool assignment, std::string &error) {
  while (i < text.size()) {
    const char c = text[i];
    if (isBlank(c) || c == '|' || c == '<' || c == '>') { break; }
    if (c == '\'') {
      const size_t e = text.find('\'', i + 1);
      if (e == std::string::npos) { error = "syntax error: unterminated single quote"; return false; }
      wb.literal(text.substr(i + 1, e - i - 1), true);
      i = e + 1;
      continue;
    }
    if (c == '"') { if (!parseDouble(wb, error)) { return false; } continue; }
    if (c == '\\') {
      if (i + 1 < text.size()) { wb.literal(std::string(1, text[i + 1]), true); i += 2; } else { wb.literal("\\", true); i++; }
      continue;
    }
    if (c == '$') { if (!expandDollar(wb, false, assignment, error)) { return false; } continue; }
    if (c == '\x01') { placeholder(wb, false, assignment); continue; }
    if (c == '~' && !wb.started && wb.done.empty() && wb.cur.text.empty()) { tilde(wb); continue; }
    wb.literal(std::string(1, c), false);
    i++;
  }
  return true;
}

static void expandField(const Field &f, std::vector<std::string> &out) {
  if (f.glob) {
    glob_t g;
    std::memset(&g, 0, sizeof(g));
    if (glob(f.pattern.c_str(), 0, NULL, &g) == 0 && g.gl_pathc > 0) {
      for (size_t k = 0; k < g.gl_pathc; k++) { out.push_back(g.gl_pathv[k]); }
      globfree(&g);
      return;
    }
    globfree(&g);
  }
  out.push_back(f.text);
}

static bool parseRedirectAt(const std::string &text, size_t &i, int fd, bool haveFd, Stage &st, const std::vector<std::string> &outputs, std::string &error) {
  Redirect r;
  r.dupFd = -1;
  if (text[i] == '<') {
    r.kind = R_IN;
    r.fd = haveFd ? fd : 0;
    i++;
  } else {
    r.fd = haveFd ? fd : 1;
    i++;
    if (i < text.size() && text[i] == '>') { r.kind = R_APPEND; i++; }
    else if (i < text.size() && text[i] == '&') {
      r.kind = R_DUP;
      i++;
      size_t j = i;
      int target = 0;
      while (j < text.size() && text[j] >= '0' && text[j] <= '9' && target < 1000) { target = target * 10 + (text[j] - '0'); j++; }
      if (j == i) { error = "syntax error: a number is needed after >&"; return false; }
      r.dupFd = target;
      i = j;
      st.redirs.push_back(r);
      return true;
    } else { r.kind = R_OUT; }
  }
  while (i < text.size() && isBlank(text[i])) { i++; }
  Parser p(text, outputs);
  p.i = i;
  WordBuilder wb;
  if (!p.parseWord(wb, false, error)) { return false; }
  wb.flush();
  i = p.i;
  if (wb.done.size() != 1) { error = "ambiguous redirect"; return false; }
  r.path = wb.done[0].text;
  st.redirs.push_back(r);
  return true;
}

bool Parser::parse(std::vector<Stage> &stages, std::string &error) {
  Stage st;
  while (true) {
    while (i < text.size() && isBlank(text[i])) { i++; }
    if (i >= text.size()) { break; }
    const char c = text[i];
    if (c == '|') {
      if (st.empty()) { error = "syntax error near '|'"; return false; }
      stages.push_back(st);
      st = Stage();
      i++;
      continue;
    }
    if (c == '<' || c == '>') { if (!parseRedirectAt(text, i, 0, false, st, outputs, error)) { return false; } continue; }
    // 2>file
    size_t j = i;
    int fd = 0;
    while (j < text.size() && text[j] >= '0' && text[j] <= '9' && fd < 1000) { fd = fd * 10 + (text[j] - '0'); j++; }
    if (j > i && j < text.size() && (text[j] == '<' || text[j] == '>')) {
      i = j;
      if (!parseRedirectAt(text, i, fd, true, st, outputs, error)) { return false; }
      continue;
    }
    // NAME=value in front of the command
    if (st.argv.empty() && isNameStart(c)) {
      size_t k = i;
      while (k < text.size() && isNameChar(text[k])) { k++; }
      if (k < text.size() && text[k] == '=') {
        const std::string name = text.substr(i, k - i);
        i = k + 1;
        WordBuilder wb;
        if (!parseWord(wb, true, error)) { return false; }
        wb.flush();
        std::string value;
        for (size_t f = 0; f < wb.done.size(); f++) { value += wb.done[f].text; }
        st.assigns.push_back(std::make_pair(name, value));
        continue;
      }
    }
    WordBuilder wb;
    if (!parseWord(wb, false, error)) { return false; }
    wb.flush();
    for (size_t f = 0; f < wb.done.size(); f++) { expandField(wb.done[f], st.argv); }
  }
  if (!st.empty()) { stages.push_back(st); }
  else if (!stages.empty()) { error = "syntax error: the command after '|' is missing"; return false; }
  return true;
}

// ---------------------------------------------------------------------------
// Builtins
// ---------------------------------------------------------------------------

static const char *const builtinNames[] = {
  "cd", "exit", "history", "alias", "unalias", "export", "unset", "set",
  "jobs", "fg", "bg", "break", "continue"
};

static bool isBuiltin(const std::string &name) {
  for (size_t i = 0; i < sizeof(builtinNames) / sizeof(builtinNames[0]); i++) { if (name == builtinNames[i]) { return true; } }
  return false;
}

static int builtinCd(const std::vector<std::string> &a) {
  std::string target;
  bool printIt = false;
  if (a.size() > 2) { err("cd: too many arguments"); return 1; }
  if (a.size() == 1) {
    const char *home = std::getenv("HOME");
    if (!home || !*home) { err("cd: HOME is not set"); return 1; }
    target = home;
  } else if (a[1] == "-") {
    const char *old = std::getenv("OLDPWD");
    if (!old || !*old) { err("cd: OLDPWD is not set"); return 1; }
    target = old;
    printIt = true;
  } else {
    target = a[1];
  }
  char before[4096];
  const bool haveBefore = getcwd(before, sizeof(before)) != NULL;
  if (chdir(target.c_str()) != 0) { err("cd: " + target + ": " + std::strerror(errno)); return 1; }
  if (haveBefore) { setenv("OLDPWD", before, 1); }
  char now[4096];
  if (getcwd(now, sizeof(now))) {
    setenv("PWD", now, 1);
    if (printIt) { std::cout << now << "\n"; }
  }
  return 0;
}

static int builtinHistory() {
  HIST_ENTRY **list = history_list();
  if (list) { for (int x = 0; list[x]; x++) { std::cout << (x + history_base) << ": " << list[x]->line << "\n"; } }
  return 0;
}

static int builtinAlias(const std::vector<std::string> &a) {
  if (a.size() == 1) {
    std::map<std::string, std::string> sorted(aliases.begin(), aliases.end());
    for (std::map<std::string, std::string>::const_iterator it = sorted.begin(); it != sorted.end(); ++it) { std::cout << "alias " << it->first << "='" << it->second << "'\n"; }
    return 0;
  }
  int status = 0;
  for (size_t i = 1; i < a.size(); i++) {
    const size_t eq = a[i].find('=');
    if (eq == std::string::npos) {
      std::unordered_map<std::string, std::string>::const_iterator it = aliases.find(a[i]);
      if (it == aliases.end()) { err("alias: " + a[i] + ": not found"); status = 1; } else { std::cout << "alias " << it->first << "='" << it->second << "'\n"; }
      continue;
    }
    const std::string name = a[i].substr(0, eq);
    const std::string value = a[i].substr(eq + 1);
    if (name.empty() || value.empty() || name.find_first_of(" \t|&;<>$`\"'\\") != std::string::npos) { err("alias: invalid alias: " + a[i]); status = 1; continue; }
    aliases[name] = value;
  }
  return status;
}

static int builtinUnalias(const std::vector<std::string> &a) {
  if (a.size() == 1) { err("unalias: usage: unalias [-a] name ..."); return 2; }
  if (a.size() == 2 && a[1] == "-a") { aliases.clear(); return 0; }
  int status = 0;
  for (size_t i = 1; i < a.size(); i++) { if (!aliases.erase(a[i])) { err("unalias: " + a[i] + ": not found"); status = 1; } }
  return status;
}

static int builtinExport(const std::vector<std::string> &a) {
  if (a.size() == 1) {
    for (char **e = environ; e && *e; e++) { std::cout << "export " << *e << "\n"; }
    return 0;
  }
  int status = 0;
  for (size_t i = 1; i < a.size(); i++) {
    const size_t eq = a[i].find('=');
    const std::string name = a[i].substr(0, eq);
    if (!isValidName(name)) { err("export: " + a[i] + ": not a valid name"); status = 1; continue; }
    if (eq != std::string::npos) { variables[name] = a[i].substr(eq + 1); setenv(name.c_str(), a[i].c_str() + eq + 1, 1); }
    else if (variables.count(name)) { setenv(name.c_str(), variables[name].c_str(), 1); }
  }
  return status;
}

static int builtinUnset(const std::vector<std::string> &a) {
  for (size_t i = 1; i < a.size(); i++) { variables.erase(a[i]); unsetenv(a[i].c_str()); }
  return 0;
}

static int builtinSet() {
  std::map<std::string, std::string> sorted(variables.begin(), variables.end());
  for (std::map<std::string, std::string>::const_iterator it = sorted.begin(); it != sorted.end(); ++it) { std::cout << it->first << "=" << it->second << "\n"; }
  return 0;
}

// --- jobs ---

static void giveTerminalTo(pid_t pgid) {
  if (gInteractive && !gSubshell) { tcsetpgrp(STDIN_FILENO, pgid); }
}

static bool waitJob(Job &job) {
  std::vector<pid_t> alive;
  bool stopped = false;
  for (size_t i = 0; i < job.pids.size(); i++) {
    if (stopped) { alive.push_back(job.pids[i]); continue; }
    int st = 0;
    pid_t r;
    do { r = waitpid(job.pids[i], &st, WUNTRACED); } while (r < 0 && errno == EINTR);
    if (r < 0) { continue; }
    if (WIFSTOPPED(st)) { stopped = true; alive.push_back(job.pids[i]); gLastStatus = 128 + WSTOPSIG(st); continue; }
    if (i + 1 == job.pids.size()) { gLastStatus = WIFEXITED(st) ? WEXITSTATUS(st) : 128 + WTERMSIG(st); }
  }
  job.pids.swap(alive);
  return stopped;
}

static void afterForeground(Job &job) {
  giveTerminalTo(gShellPgid);
  if (!job.pids.empty()) {
    job.running = false;
    jobs[nextJobId] = job;
    std::cout << "[" << nextJobId << "] Stopped " << job.command << "\n";
    nextJobId++;
  }
}

static int parseJobId(const std::vector<std::string> &a, const char *name) {
  if (jobs.empty()) { err(std::string(name) + ": no such job"); return -1; }
  if (a.size() < 2) { return jobs.rbegin()->first; }
  std::string s = a[1];
  if (!s.empty() && s[0] == '%') { s = s.substr(1); }
  char *end = NULL;
  const long id = std::strtol(s.c_str(), &end, 10);
  if (s.empty() || *end || jobs.find(static_cast<int>(id)) == jobs.end()) { err(std::string(name) + ": no such job"); return -1; }
  return static_cast<int>(id);
}

static int builtinFg(const std::vector<std::string> &a) {
  const int id = parseJobId(a, "fg");
  if (id < 0) { return 1; }
  Job job = jobs[id];
  jobs.erase(id);
  std::cout << job.command << "\n";
  giveTerminalTo(job.pgid);
  kill(-job.pgid, SIGCONT);
  job.running = true;
  if (waitJob(job)) { afterForeground(job); } else { giveTerminalTo(gShellPgid); }
  return gLastStatus;
}

static int builtinBg(const std::vector<std::string> &a) {
  const int id = parseJobId(a, "bg");
  if (id < 0) { return 1; }
  kill(-jobs[id].pgid, SIGCONT);
  jobs[id].running = true;
  std::cout << "[" << id << "] " << jobs[id].command << " &\n";
  return 0;
}

static int builtinJobs() {
  for (std::map<int, Job>::const_iterator it = jobs.begin(); it != jobs.end(); ++it) {
    std::cout << "[" << it->first << "] " << (it->second.running ? "Running" : "Stopped") << " " << it->second.command << " (PGID " << it->second.pgid << ")\n";
  }
  return 0;
}

static int builtinExit(const std::vector<std::string> &a) {
  gExit = true;
  if (a.size() > 1) {
    char *end = NULL;
    const long code = std::strtol(a[1].c_str(), &end, 10);
    if (end == a[1].c_str() || *end != '\0') {
      err("exit: " + a[1] + ": numeric argument required");
      gExitCode = 2;
      return 2;
    }
    gExitCode = static_cast<int>(code) & 0xFF;
  } else {
    gExitCode = gLastStatus;
  }
  return gExitCode;
}

// break [N] / continue [N]
static int builtinBreakOrContinue(const std::vector<std::string> &a, bool isBreak) {
  const char *name = isBreak ? "break" : "continue";
  int level = 1;
  if (a.size() > 2) { err(std::string(name) + ": too many arguments"); return 1; }
  if (a.size() > 1) {
    char *end = NULL;
    const long n = std::strtol(a[1].c_str(), &end, 10);
    if (end == a[1].c_str() || *end != '\0' || n <= 0) {
      err(std::string(name) + ": " + a[1] + ": numeric argument required");
      return 2;
    }
    level = n > 1000000 ? 1000000 : static_cast<int>(n);
  }
  if (gLoopDepth == 0) {
    err(std::string(name) + ": only meaningful in a loop");
    return 1;
  }
  if (level > gLoopDepth) { level = gLoopDepth; }
  if (isBreak) { gBreakLevel = level; } else { gContinueLevel = level; }
  return 0;
}

static int runBuiltinBody(const std::vector<std::string> &a) {
  const std::string &n = a[0];
  if (n == "cd") { return builtinCd(a); }
  if (n == "exit") { return builtinExit(a); }
  if (n == "history") { return builtinHistory(); }
  if (n == "alias") { return builtinAlias(a); }
  if (n == "unalias") { return builtinUnalias(a); }
  if (n == "export") { return builtinExport(a); }
  if (n == "unset") { return builtinUnset(a); }
  if (n == "set") { return builtinSet(); }
  if (n == "jobs") { return builtinJobs(); }
  if (n == "fg") { return builtinFg(a); }
  if (n == "bg") { return builtinBg(a); }
  if (n == "break") { return builtinBreakOrContinue(a, true); }
  if (n == "continue") { return builtinBreakOrContinue(a, false); }
  return 127;
}

// ---------------------------------------------------------------------------
// 4. Running commands
// ---------------------------------------------------------------------------

// Open the files and put them on the descriptors. In a child process, and for a builtin that runs here (then they are restored)
static bool applyRedirects(const std::vector<Redirect> &redirs, std::string &error) {
  for (size_t i = 0; i < redirs.size(); i++) {
    const Redirect &r = redirs[i];
    if (r.kind == R_DUP) {
      if (dup2(r.dupFd, r.fd) < 0) { error = std::to_string(r.dupFd) + ": " + std::strerror(errno); return false; }
      continue;
    }
    int flags = O_RDONLY;
    if (r.kind == R_OUT) { flags = O_WRONLY | O_CREAT | O_TRUNC; }
    else if (r.kind == R_APPEND) { flags = O_WRONLY | O_CREAT | O_APPEND; }
    const int fd = open(r.path.c_str(), flags, 0666);
    if (fd < 0) { error = r.path + ": " + std::strerror(errno); return false; }
    if (fd != r.fd) {
      if (dup2(fd, r.fd) < 0) { error = std::to_string(r.fd) + ": " + std::strerror(errno); close(fd); return false; }
      close(fd);
    }
    // clear FD_CLOEXEC so exec'ing children inherit the redirected descriptor
    const int fl = fcntl(r.fd, F_GETFD);
    if (fl >= 0) { fcntl(r.fd, F_SETFD, fl & ~FD_CLOEXEC); }
  }
  return true;
}

// A builtin (and "NAME=value" alone) in this process; its redirections are undone afterwards.
// Every descriptor the redirections touched is saved and restored, not only 0/1/2.
static int runInProcess(const Stage &st) {
  std::vector<int> touched;
  for (size_t i = 0; i < st.redirs.size(); i++) { touched.push_back(st.redirs[i].fd); }
  if (touched.empty()) { touched.push_back(0); touched.push_back(1); touched.push_back(2); }
  std::sort(touched.begin(), touched.end());
  touched.erase(std::unique(touched.begin(), touched.end()), touched.end());
  std::vector<int> saved(touched.size(), -1);
  for (size_t i = 0; i < touched.size(); i++) { saved[i] = fcntl(touched[i], F_DUPFD_CLOEXEC, 10); }
  flushOutput();
  std::string error;
  int status = 1;
  if (applyRedirects(st.redirs, error)) {
    if (st.argv.empty()) {
      for (size_t i = 0; i < st.assigns.size(); i++) { setVariable(st.assigns[i].first, st.assigns[i].second); }
      status = 0;
    } else {
      status = runBuiltinBody(st.argv);
    }
  } else {
    err(error);
  }
  flushOutput();
  for (size_t i = 0; i < touched.size(); i++) {
    if (saved[i] >= 0) { dup2(saved[i], touched[i]); close(saved[i]); }
  }
  return status;
}

static void childExec(const Stage &st) {
  std::string error;
  if (!applyRedirects(st.redirs, error)) { err(error); _exit(1); }
  for (size_t i = 0; i < st.assigns.size(); i++) { setenv(st.assigns[i].first.c_str(), st.assigns[i].second.c_str(), 1); }
  if (st.argv.empty()) { _exit(0); }
  if (isBuiltin(st.argv[0])) { const int status = runBuiltinBody(st.argv); flushOutput(); _exit(status & 0xFF); }
  std::vector<char *> args;
  for (size_t i = 0; i < st.argv.size(); i++) { args.push_back(const_cast<char *>(st.argv[i].c_str())); }
  args.push_back(NULL);
  execvp(args[0], args.data());
  const int e = errno;
  err(st.argv[0] + (e == ENOENT ? ": command not found" : std::string(": ") + std::strerror(e)));
  _exit(e == ENOENT ? 127 : 126);
}

static void launchPipeline(const std::vector<Stage> &stages, bool background, const std::string &commandText) {
  const size_t n = stages.size();
  std::vector<int> pipefds(n > 1 ? 2 * (n - 1) : 0);
  for (size_t i = 0; i + 1 < n; i++) {
    if (pipe(&pipefds[i * 2]) < 0) { err(std::string("pipe: ") + std::strerror(errno)); for (size_t k = 0; k < i * 2; k++) { close(pipefds[k]); } gLastStatus = 1; return; }
  }
  Job job;
  job.pgid = 0;
  job.command = commandText;
  job.running = true;
  const bool control = gInteractive && !gSubshell;
  flushOutput();
  for (size_t i = 0; i < n; i++) {
    const pid_t pid = fork();
    if (pid == 0) {
      if (control) {
        if (job.pgid == 0) { job.pgid = getpid(); }
        setpgid(0, job.pgid);
        if (!background) { tcsetpgrp(STDIN_FILENO, job.pgid); }
      }
      signal(SIGINT, SIG_DFL);
      signal(SIGQUIT, SIG_DFL);
      signal(SIGTSTP, SIG_DFL);
      signal(SIGTTIN, SIG_DFL);
      signal(SIGTTOU, SIG_DFL);
      signal(SIGCHLD, SIG_DFL);
      if (i > 0) { dup2(pipefds[(i - 1) * 2], STDIN_FILENO); }
      if (i + 1 < n) { dup2(pipefds[i * 2 + 1], STDOUT_FILENO); }
      for (size_t k = 0; k < pipefds.size(); k++) { close(pipefds[k]); }
      childExec(stages[i]);
    } else if (pid > 0) {
      if (job.pgid == 0) { job.pgid = pid; }
      if (control) { setpgid(pid, job.pgid); }
      job.pids.push_back(pid);
    } else {
      err(std::string("fork: ") + std::strerror(errno));
      gLastStatus = 1;
    }
  }
  for (size_t k = 0; k < pipefds.size(); k++) { close(pipefds[k]); }
  if (job.pids.empty()) { return; }
  if (background) {
    jobs[nextJobId] = job;
    std::cout << "[" << nextJobId << "] " << job.pgid << "\n";
    nextJobId++;
    gLastStatus = 0;
    return;
  }
  giveTerminalTo(job.pgid);
  if (waitJob(job)) { afterForeground(job); } else { giveTerminalTo(gShellPgid); }
}

static void executeStages(const std::vector<Stage> &stages, bool background, const std::string &commandText) {
  if (stages.size() == 1 && !background && (stages[0].argv.empty() || isBuiltin(stages[0].argv[0]))) {
    gLastStatus = runInProcess(stages[0]);
    return;
  }
  launchPipeline(stages, background, commandText);
}

static void reapJobs() {
  for (std::map<int, Job>::iterator it = jobs.begin(); it != jobs.end();) {
    Job &job = it->second;
    std::vector<pid_t> alive;
    for (size_t i = 0; i < job.pids.size(); i++) {
      int st = 0;
      const pid_t r = waitpid(job.pids[i], &st, WNOHANG | WUNTRACED | WCONTINUED);
      if (r == 0) { alive.push_back(job.pids[i]); continue; }
      if (r < 0) { continue; }
      if (WIFSTOPPED(st)) { job.running = false; alive.push_back(job.pids[i]); }
      else if (WIFCONTINUED(st)) { job.running = true; alive.push_back(job.pids[i]); }
    }
    job.pids.swap(alive);
    if (job.pids.empty()) {
      std::cout << "[" << it->first << "] Done " << job.command << "\n";
      jobs.erase(it++);
    } else {
      ++it;
    }
  }
}

// ---------------------------------------------------------------------------
// One command of the list, one line
// ---------------------------------------------------------------------------

static void runPipelineText(const std::string &raw, bool background) {
  const std::string aliased = expandAliases(raw);
  std::vector<std::string> outputs;
  std::string substituted, error;
  if (!substituteCommands(aliased, outputs, substituted, error)) { err(error); gLastStatus = 2; return; }
  std::vector<Stage> stages;
  Parser parser(substituted, outputs);
  if (!parser.parse(stages, error)) { err(error); gLastStatus = 2; return; }
  if (stages.empty()) { return; }
  executeStages(stages, background, raw);
}

// ---------------------------------------------------------------------------
// Compound commands: for / while / until / if
// ---------------------------------------------------------------------------

// Split a `for … in …` word list using the same pipeline as a normal command line.
static bool splitForItems(const std::string &itemsText, std::vector<std::string> &items, std::string &error) {
  if (itemsText.empty()) { return true; }
  std::vector<std::string> outputs;
  std::string sub;
  if (!substituteCommands(itemsText, outputs, sub, error)) { return false; }
  std::vector<Stage> stages;
  Parser p(sub, outputs);
  if (!p.parse(stages, error)) { return false; }
  if (stages.size() != 1 || !stages[0].redirs.empty() || !stages[0].assigns.empty()) {
    error = "syntax error: for: bad word list";
    return false;
  }
  items = stages[0].argv;
  return true;
}

static int runForLoop(const Compound &c) {
  std::vector<std::string> items;
  std::string error;
  if (c.haveIn) {
    if (!splitForItems(c.itemsText, items, error)) { err(error); return 2; }
  } else {
    // No `in` clause: iterate "$@" — this shell has no positional parameters, so zero iterations.
  }
  const int savedDepth = gLoopDepth;
  gLoopDepth++;
  int status = 0;
  for (size_t k = 0; k < items.size(); k++) {
    gBreakLevel = 0; gContinueLevel = 0;
    setVariable(c.var, items[k]);
    runLine(c.body);
    if (gBreakLevel > 0) {
      gBreakLevel--;
      if (gBreakLevel == 0) { status = gLastStatus; break; }
      gLoopDepth = savedDepth;
      return gLastStatus;
    }
    if (gContinueLevel > 0) {
      gContinueLevel--;
      if (gContinueLevel > 0) { gLoopDepth = savedDepth; return gLastStatus; }
      status = gLastStatus;
      continue;
    }
    status = gLastStatus;
    if (gExit) { break; }
  }
  gLoopDepth = savedDepth;
  return status;
}

static int runWhileLoop(const Compound &c) {
  const int savedDepth = gLoopDepth;
  gLoopDepth++;
  int status = 0;            // the status of the last body that ran (0 if none did)
  while (!gExit) {
    gBreakLevel = 0; gContinueLevel = 0;
    runLine(c.cond);
    if (gBreakLevel > 0) { gBreakLevel = 0; break; }
    if (gContinueLevel > 0) { gContinueLevel = 0; continue; }
    if (gExit) { break; }
    const bool condTrue = (gLastStatus == 0);
    const bool runBody = (c.keyword == "while") ? condTrue : !condTrue;
    if (!runBody) { break; }
    runLine(c.body);
    if (gBreakLevel > 0) {
      gBreakLevel--;
      if (gBreakLevel == 0) { status = gLastStatus; break; }
      gLoopDepth = savedDepth;
      return gLastStatus;
    }
    if (gContinueLevel > 0) {
      gContinueLevel--;
      if (gContinueLevel > 0) { gLoopDepth = savedDepth; return gLastStatus; }
      status = gLastStatus;
      continue;
    }
    status = gLastStatus;
    if (gExit) { break; }
  }
  gLoopDepth = savedDepth;
  return status;
}

// if COND; then BODY [elif COND; then BODY]... [else BODY]; fi
// The status is the one of the body that ran, or 0 when no condition was true and there is no else.
static int runIf(const Compound &c) {
  for (size_t k = 0; k < c.clauses.size(); k++) {
    runLine(c.clauses[k].first);
    // exit / break / continue inside a condition: stop here, the enclosing loop will act on it
    if (gExit || gBreakLevel > 0 || gContinueLevel > 0) { return gLastStatus; }
    if (gLastStatus == 0) {
      runLine(c.clauses[k].second);
      return gLastStatus;
    }
  }
  if (c.haveElse) {
    runLine(c.elseBody);
    return gLastStatus;
  }
  return 0;
}

// ---------------------------------------------------------------------------
// The list of one line
// ---------------------------------------------------------------------------

static void runLine(const std::string &line) {
  std::string clean;
  for (size_t i = 0; i < line.size(); i++) {
    if (line[i] != '\x01' && line[i] != '\x02') { clean += line[i]; }
  }
  std::vector<ListItem> items;
  std::string error;
  if (!splitList(clean, items, error)) { err(error); gLastStatus = 2; return; }
  char before = ';';
  // after a break / continue the rest of the list is skipped, the enclosing loop picks it up
  for (size_t k = 0; k < items.size() && !gExit && gBreakLevel == 0 && gContinueLevel == 0; k++) {
    const bool run = before == ';' || before == '&' || before == 0
                  || (before == 'a' && gLastStatus == 0)
                  || (before == 'o' && gLastStatus != 0);
    if (run) {
      if (items[k].isCompound) {
        const Compound &c = items[k].compound;
        int st;
        if (c.keyword == "for") { st = runForLoop(c); }
        else if (c.keyword == "if") { st = runIf(c); }
        else { st = runWhileLoop(c); }
        gLastStatus = st & 0xFF;
      } else {
        runPipelineText(items[k].text, items[k].op == '&');
      }
    }
    before = items[k].op;
  }
}

// ---------------------------------------------------------------------------
// History and tab completion
// ---------------------------------------------------------------------------

static void initHistoryPath() {
  const char *home = std::getenv("HOME");
  if (!home || !*home) { home = "."; }
  historyPath = std::string(home) + "/.0vershell.txt";
}

static void loadSystemCommands() {
  const char *pathEnv = std::getenv("PATH");
  if (!pathEnv) { return; }
  std::string path(pathEnv);
  size_t start = 0;
  while (start <= path.size()) {
    size_t colon = path.find(':', start);
    if (colon == std::string::npos) { colon = path.size(); }
    const std::string dir = colon > start ? path.substr(start, colon - start) : ".";
    DIR *dp = opendir(dir.c_str());
    if (dp) {
      struct dirent *entry;
      while ((entry = readdir(dp)) != NULL) {
        if (entry->d_name[0] == '.') { continue; }
        const std::string full = dir + "/" + entry->d_name;
        struct stat st;
        if (stat(full.c_str(), &st) == 0 && S_ISREG(st.st_mode) && access(full.c_str(), X_OK) == 0) { commandList.push_back(entry->d_name); }
      }
      closedir(dp);
    }
    start = colon + 1;
  }
  std::sort(commandList.begin(), commandList.end());
  commandList.erase(std::unique(commandList.begin(), commandList.end()), commandList.end());
}

static char *commandGenerator(const char *text, int state) {
  static std::vector<std::string> matches;
  static size_t index;
  if (!state) {
    matches.clear();
    index = 0;
    const std::string prefix(text);
    for (size_t i = 0; i < commandList.size(); i++) { if (commandList[i].compare(0, prefix.size(), prefix) == 0) { matches.push_back(commandList[i]); } }
    for (size_t i = 0; i < sizeof(builtinNames) / sizeof(builtinNames[0]); i++) { if (std::string(builtinNames[i]).compare(0, prefix.size(), prefix) == 0) { matches.push_back(builtinNames[i]); } }
    for (std::unordered_map<std::string, std::string>::const_iterator it = aliases.begin(); it != aliases.end(); ++it) { if (it->first.compare(0, prefix.size(), prefix) == 0) { matches.push_back(it->first); } }
    std::sort(matches.begin(), matches.end());
    matches.erase(std::unique(matches.begin(), matches.end()), matches.end());
  }
  if (index < matches.size()) { return strdup(matches[index++].c_str()); }
  return NULL;
}

// A word after which a new command starts
static bool startsCommand(const std::string &w) {
  return w == "then" || w == "else" || w == "elif" || w == "do" || w == "if" || w == "while" || w == "until";
}

static char **myCompletion(const char *text, int start, int end) {
  static_cast<void>(end);
  int k = start - 1;
  while (k >= 0 && isBlank(rl_line_buffer[k])) { k--; }
  if (k < 0 || std::strchr("|;&(", rl_line_buffer[k])) { return rl_completion_matches(text, commandGenerator); }
  int b = k;
  while (b >= 0 && !isBlank(rl_line_buffer[b]) && !std::strchr("|;&(", rl_line_buffer[b])) { b--; }
  if (startsCommand(std::string(rl_line_buffer + b + 1, static_cast<size_t>(k - b)))) { return rl_completion_matches(text, commandGenerator); }
  return NULL;
}

// ---------------------------------------------------------------------------

static void initShell() {
  gInteractive = isatty(STDIN_FILENO) != 0;
  if (!gInteractive) { return; }
  while (tcgetpgrp(STDIN_FILENO) != (gShellPgid = getpgrp())) { kill(-gShellPgid, SIGTTIN); }
  signal(SIGINT, SIG_IGN);
  signal(SIGQUIT, SIG_IGN);
  signal(SIGTSTP, SIG_IGN);
  signal(SIGTTIN, SIG_IGN);
  signal(SIGTTOU, SIG_IGN);
  gShellPgid = getpid();
  if (setpgid(gShellPgid, gShellPgid) < 0 && errno != EPERM) { perror("0vershell: setpgid"); }
  tcsetpgrp(STDIN_FILENO, gShellPgid);
}

int main(int argc, char *argv[]) {
  gShellPid = getpid();  // $$ must stay the same inside $( … )
  if (argc >= 3 && std::strcmp(argv[1], "-c") == 0) {
    gShellPgid = getpgrp();
    runLine(argv[2]);
    flushOutput();
    return gExit ? gExitCode : (gLastStatus & 0xFF);
  }
  initShell();
  initHistoryPath();
  using_history();
  stifle_history(2000);
  read_history(historyPath.c_str());
  loadSystemCommands();
  rl_attempted_completion_function = myCompletion;
  std::string lastLine;
  while (!gExit) {
    reapJobs();
    char *input = readline("0vershell> ");
    if (!input) {
      std::cout << "\n";
      break;
    }
    const std::string line = trim(input);
    free(input);
    if (line.empty()) { continue; }
    if (line != lastLine) {
      add_history(line.c_str());
      append_history(1, historyPath.c_str());
      lastLine = line;
    }
    runLine(line);
  }
  write_history(historyPath.c_str());
  chmod(historyPath.c_str(), 0600);
  flushOutput();
  return gExit ? gExitCode : (gLastStatus & 0xFF);
}