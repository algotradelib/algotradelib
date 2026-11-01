# Prints file:line for a code line indented more than 2 past the previous code line, and exits 1 if any.
# Skips comment lines, block comments, raw-string bodies and preprocessor lines.
# Allows the hang after `: ` (initializer lists, base clauses): 4 past a line whose first non-blank characters are `: `.
# It cannot see alignment after a dedent, alignment within 2 columns, or a comment continued at a trailing comment's column.
# A block comment opened on a preprocessor line is not tracked.
function report(msg) {
  print FILENAME ":" FNR ": " msg
  found = 1
}

# Scans one line for string literals, comments and raw strings, updating the carried state.
function scan(line,    i, n, c, rest, before, close_at, open_at, delim) {
  n = length(line)
  i = 1
  while (i <= n) {
    c = substr(line, i, 1)
    rest = substr(line, i)
    if (substr(rest, 1, 2) == "//") {
      return
    }
    if (substr(rest, 1, 2) == "/*") {
      close_at = index(substr(line, i + 2), "*/")
      if (close_at == 0) {
        in_block = 1
        return
      }
      i += close_at + 3
      continue
    }
    before = substr(line, 1, i - 1)
    if (c == "R" && substr(rest, 2, 1) == "\"" && (before !~ /[A-Za-z0-9_]$/ || before ~ /(^|[^A-Za-z0-9_])(u8|u|U|L)$/)) {
      open_at = index(substr(rest, 3), "(")
      delim = substr(rest, 3, open_at - 1)
      close_at = index(substr(rest, 3 + open_at), ")" delim "\"")
      if (close_at == 0) {
        in_raw = 1
        raw_end = ")" delim "\""
        return
      }
      i += 2 + open_at + close_at + length(delim) + 1
      continue
    }
    if (c == "\"") {
      i++
      while (i <= n && substr(line, i, 1) != "\"") {
        if (substr(line, i, 1) == "\\") {
          i++
        }
        i++
      }
      i++
      continue
    }
    if (c == "'" && (i == 1 || substr(line, i - 1, 1) !~ /[0-9A-Fa-f]/)) {
      i++
      while (i <= n && substr(line, i, 1) != "'") {
        if (substr(line, i, 1) == "\\") {
          i++
        }
        i++
      }
      i++
      continue
    }
    i++
  }
}

FNR == 1 {
  prev = -1
  prev_text = ""
  in_block = 0
  in_raw = 0
}

{
  line = $0
  if (in_raw) {
    at = index(line, raw_end)
    if (at == 0) {
      next
    }
    in_raw = 0
    scan(substr(line, at + length(raw_end)))
    next
  }
  if (in_block) {
    at = index(line, "*/")
    if (at == 0) {
      next
    }
    in_block = 0
    scan(substr(line, at + 2))
    next
  }
  text = line
  sub(/^[ ]+/, "", text)
  if (text == "" || substr(text, 1, 1) == "#") {
    next
  }
  if (substr(text, 1, 2) == "//") {
    next
  }
  indent = length(line) - length(text)
  if (substr(text, 1, 2) != "/*" && prev >= 0 && indent > prev + 2 && !(substr(prev_text, 1, 2) == ": " && indent == prev + 4)) {
    report("indented " indent - prev " past the line above")
  }
  if (substr(text, 1, 2) != "/*") {
    prev = indent
    prev_text = text
  }
  scan(line)
}

END {
  exit found
}
