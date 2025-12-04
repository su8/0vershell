# 0vershell
My custom made shell

# Compile

```bash
make -j8  # 8 cores/threads to use in parallel compile
sudo/doas make install
```

# Requirements

In Debian it's `libreadline-dev`, in your other OS's search for `readline`. It's used to store and retrieve history when you type or use the arrow keyboard keys.

---

### To do:

1. Parse `while` / `for` loops.

2. Parse longer lines and lines that contain `/` that act to append code on newline(s), e.g multiline code stored in a single variable.

3. `Completed`. ~Use `echo $myVar` instead of plain `$myVar` to retrieve the content of variable.~ ~Added in other file as it's still needs more testing.~ 

4. Parse and execute `$(systemCommand)` within variables too.
