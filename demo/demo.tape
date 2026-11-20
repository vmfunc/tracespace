# Demo recording for the README, for VHS (https://github.com/charmbracelet/vhs).
#
#   sh demo/record.sh            # needs vhs, nvim and tracespace on PATH
#   sh demo/record.sh --docker   # records inside Docker, nothing else needed
#
# Run from the repository root. Nothing is saved: the edits stay in the buffer.

Output demo/demo.gif

Require nvim
Require tracespace

Set Shell "bash"
Set FontSize 15
Set Width 1500
Set Height 780
Set Padding 14
Set TypingSpeed 55ms

# Settings must come first. Without a UTF-8 locale tracespace falls back to
# the ascii theme.
Env LANG "C.UTF-8"
Env LC_ALL "C.UTF-8"

# --- open the example and the diagram next to it
Type "nvim -u demo/init.lua examples/basics.cpp"
Enter
Sleep 1.5s
Type ":Tracespace"
Enter
Sleep 3s
Screenshot demo/frames/01-open.png

# --- put the cursor inside funcA's loop: its box and arc light up, and a
# --- runner counts the passes along the arc
Type "/a1();"
Enter
Sleep 4s
Screenshot demo/frames/02-loop.png

# --- type new code; the diagram follows every keystroke, nothing is saved
Type "/funcC();"
Enter
Type "o"
Type "while (true) { funcA(); }"
Sleep 1.5s
Escape
Sleep 2.5s
Screenshot demo/frames/03-typed.png

# --- half-typed code keeps the diagram alive and flags the function
Type "o"
Type "funcA("
Sleep 2.5s
Screenshot demo/frames/04-half-typed.png
Type ");"
Escape
Sleep 2s

# --- move into the diagram and drive it with the keyboard
Ctrl+W
Type "l"
Sleep 1s
Right
Sleep 800ms
Down
Sleep 800ms
Right
Sleep 800ms
Up
Sleep 800ms
Space
Sleep 1.5s
Screenshot demo/frames/05-collapsed.png
Space
Sleep 1s

# --- source order, then the three arc themes
Tab
Sleep 2.5s
Tab
Sleep 1s
Type "T"
Sleep 2s
Screenshot demo/frames/06-rounded.png
Type "T"
Sleep 2s
Type "T"
Sleep 1.5s

# --- Enter jumps back to the code of the selected box
Right
Sleep 800ms
Enter
Sleep 2.5s
Screenshot demo/frames/07-jumped.png

Type ":qa!"
Enter
Sleep 500ms
