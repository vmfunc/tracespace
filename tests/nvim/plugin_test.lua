-- End-to-end test of tracespace.nvim against the real binary, in headless Neovim:
--
--   TRACESPACE_BIN=/path/to/tracespace nvim --headless --clean \
--       -c "set rtp+=nvim" -c "luafile tests/nvim/plugin_test.lua"
--
-- The diagram runs in a real :terminal buffer, so its screen can be read back.
-- The test body is a coroutine resumed by timers, so between steps Neovim's main
-- loop is idle exactly as it is for a user: typed keys are processed and the real
-- TextChanged / CursorMoved autocommands fire.

local api = vim.api
local uv = vim.uv or vim.loop
local binary = assert(os.getenv("TRACESPACE_BIN"), "set TRACESPACE_BIN")
local is_windows = uv.os_uname().sysname:find("Windows") ~= nil

local EXAMPLE = {
  "void a1() {}",
  "void a2() {}",
  "void funcA() {",
  "    for (int i = 0; i < 2; ++i) { a1(); a2(); }",
  "}",
  "void funcC() {}",
  "int main() { funcA(); funcC(); return 0; }",
}

local passed, failed = 0, 0

-- Results go to stdout, or to the file named by TRACESPACE_TEST_LOG. (On Windows,
-- run Neovim in its own console and use the log: when Neovim's stdout is a pipe,
-- terminal jobs write to that pipe instead of the terminal buffer.)
local log_path = os.getenv("TRACESPACE_TEST_LOG")
local function out(text)
  if log_path then
    local f = assert(io.open(log_path, "a"))
    f:write(text)
    f:close()
  else
    io.stdout:write(text)
    io.stdout:flush()
  end
end
local ts = require("tracespace")
local state = ts._state

local function term_text()
  if not (state.term_buf and api.nvim_buf_is_valid(state.term_buf)) then
    return ""
  end
  return table.concat(api.nvim_buf_get_lines(state.term_buf, 0, -1, false), "\n")
end

local co

-- Give control back to Neovim's main loop for a while.
local function sleep(ms)
  vim.defer_fn(function()
    local ok, err = coroutine.resume(co)
    if not ok then
      out("FATAL " .. tostring(err) .. "\n")
      vim.cmd("cquit 2")
    end
  end, ms)
  coroutine.yield()
end

local function wait(what, pred, timeout)
  local deadline = uv.hrtime() + (timeout or 8000) * 1e6
  while not pred() do
    if uv.hrtime() > deadline then
      error(("timed out waiting for %s; diagram was:\n%s"):format(what, term_text()), 0)
    end
    sleep(20)
  end
end

local function wait_text(needle, timeout)
  wait(("%q in the diagram"):format(needle), function()
    return term_text():find(needle, 1, true) ~= nil
  end, timeout)
end

-- Type keys the way a user would: queued as real input and processed by the
-- main loop while the test is suspended.
local function keys(k)
  api.nvim_input(k)
  sleep(120)
end

local function case(name, fn)
  local ok, err = pcall(fn)
  if ok then
    passed = passed + 1
    out("ok    " .. name .. "\n")
  else
    failed = failed + 1
    out("FAIL  " .. name .. "\n" .. tostring(err) .. "\n")
  end
end

local dir = vim.fn.tempname()
vim.fn.mkdir(dir, "p")
local file = dir .. (is_windows and "\\" or "/") .. "demo.cpp"
vim.fn.writefile(EXAMPLE, file)

ts.setup({ binary = binary, theme = "rounded", width = 70, args = { "--no-anim" } })
vim.cmd("edit " .. vim.fn.fnameescape(file))
local source_win = api.nvim_get_current_win()
local source_buf = api.nvim_get_current_buf()

-- Start each case from the same state, so one failure cannot cause the next.
local function reset()
  vim.cmd("stopinsert")
  if not api.nvim_win_is_valid(source_win) then
    source_win = api.nvim_get_current_win()
  end
  api.nvim_set_current_win(source_win)
  if api.nvim_win_get_buf(source_win) ~= source_buf then
    api.nvim_win_set_buf(source_win, source_buf)
  end
  api.nvim_buf_set_lines(source_buf, 0, -1, false, EXAMPLE)
  api.nvim_win_set_cursor(source_win, { 1, 0 })
  sleep(300)
end

local function reopen()
  if ts.is_open() then
    ts.close()
  end
  reset()
  ts.open()
end

co = coroutine.create(function()
case(":Tracespace opens a split and connects", function()
  assert(vim.fn.exists(":Tracespace") == 2, ":Tracespace is not defined")
  vim.cmd("Tracespace")
  assert(ts.is_open(), "split not opened")
  assert(api.nvim_get_current_win() == source_win, "focus should stay in the source window")
  assert(#api.nvim_tabpage_list_wins(0) == 2)
  wait("connection", ts.is_connected)
  wait_text("funcA")
  wait_text("nvim ●")
  wait_text("demo.cpp")
end)

case("unsaved edits appear in the diagram", function()
  reset()
  keys("ggOvoid typed_live() {}<Esc>")
  keys("G$F{a typed_live();<Esc>")
  assert(vim.bo[source_buf].modified, "buffer should be unsaved")
  wait_text("typed_live")
  assert(not table.concat(vim.fn.readfile(file), "\n"):find("typed_live"), "file on disk must be untouched")
  keys("u")
  keys("u")
  wait("the edit to be undone in the diagram", function()
    return not term_text():find("typed_live", 1, true)
  end)
end)

case("half-typed code keeps the diagram alive", function()
  reset()
  keys("G$F{a broken(<Esc>")
  wait_text("parse error")
  assert(term_text():find("funcA", 1, true), "diagram lost its boxes")
  keys("u")
  wait("the parse error to clear", function()
    return not term_text():find("parse error", 1, true)
  end)
end)

case("cursor position highlights the loop it is in", function()
  reset()
  keys("4G0fa") -- onto a1() inside the for loop
  wait_text("┏")
  keys("7G0")
  wait("the loop highlight to go away", function()
    return not term_text():find("┏", 1, true)
  end)
end)

case("Enter in the diagram jumps to the code", function()
  reset()
  keys("1G0")
  -- select funcC (main -> funcA -> funcC) and press Enter
  api.nvim_chan_send(state.job, "\27[C")
  api.nvim_chan_send(state.job, "\27[C")
  sleep(200)
  api.nvim_set_current_win(state.term_win) -- as if the user were in the diagram
  api.nvim_chan_send(state.job, "\r")
  wait("the cursor to land on funcC", function()
    return api.nvim_get_current_win() == source_win and api.nvim_win_get_cursor(source_win)[1] == 6
  end)
  assert(api.nvim_win_get_cursor(source_win)[2] == 0)
end)

case("only c and cpp buffers are sent; switching buffers follows", function()
  reset()
  vim.cmd("enew")
  vim.bo.filetype = "lua"
  keys("ilocal function not_c() end<Esc>")
  sleep(400)
  assert(term_text():find("funcA", 1, true), "a lua buffer must not replace the diagram")
  assert(not term_text():find("not_c", 1, true))

  local other = dir .. (is_windows and "\\" or "/") .. "other.c"
  vim.fn.writefile({ "void only_here(void) {}", "int main(void) { only_here(); return 0; }" }, other)
  vim.cmd("edit! " .. vim.fn.fnameescape(other))
  wait_text("only_here")
  wait_text("other.c")
  vim.cmd("buffer! " .. source_buf)
  wait_text("funcA")
  source_win = api.nvim_get_current_win()
end)

case(":Tracespace again closes the split and cleans up", function()
  local job, path = state.job, state.socket_path
  vim.cmd("Tracespace")
  assert(not ts.is_open(), "split still open")
  assert(not ts.is_connected())
  assert(#api.nvim_tabpage_list_wins(0) == 1, "terminal window still there")
  wait("the job to stop", function()
    return vim.fn.jobwait({ job }, 0)[1] ~= -1
  end)
  if not is_windows then
    assert(uv.fs_stat(path) == nil, "socket file left behind")
  end
  assert(not pcall(api.nvim_get_autocmds, { group = "tracespace" }), "autocommands left behind")
end)

case("quitting the diagram with q closes the split", function()
  reopen()
  wait("connection", ts.is_connected)
  wait_text("funcA")
  api.nvim_chan_send(state.job, "q")
  wait("the split to close", function()
    return not ts.is_open()
  end)
end)

case("VimLeavePre stops the program and removes the socket", function()
  reopen()
  wait("connection", ts.is_connected)
  local job, path = state.job, state.socket_path
  vim.cmd("doautocmd VimLeavePre")
  assert(not ts.is_open())
  wait("the job to stop", function()
    return vim.fn.jobwait({ job }, 0)[1] ~= -1
  end)
  if not is_windows then
    assert(uv.fs_stat(path) == nil, "socket file left behind")
  end
end)

case("a missing binary is reported, not crashed on", function()
  ts.setup({ binary = "tracespace-does-not-exist" })
  vim.cmd("Tracespace")
  assert(not ts.is_open())
  ts.setup({ binary = binary, theme = "rounded" })
end)

case(":help tracespace", function()
  local doc = vim.fn.fnamemodify(debug.getinfo(1, "S").source:sub(2), ":p:h:h:h") .. "/nvim/doc"
  local copy = dir .. "/doc"
  vim.fn.mkdir(copy, "p")
  vim.fn.writefile(vim.fn.readfile(doc .. "/tracespace.txt"), copy .. "/tracespace.txt")
  vim.cmd("helptags " .. vim.fn.fnameescape(copy))
  vim.opt.runtimepath:append(dir)
  vim.cmd("help tracespace")
  assert(api.nvim_buf_get_name(0):find("tracespace.txt", 1, true), "help page not opened")
  for _, tag in ipairs({ ":Tracespace", "tracespace.setup()", "tracespace-keys", "tracespace-protocol" }) do
    vim.cmd("help " .. tag)
  end
  vim.cmd("helpclose")
end)

out(("\n%d passed, %d failed\n"):format(passed, failed))
vim.fn.delete(dir, "rf")
vim.cmd(failed == 0 and "qall!" or "cquit 1")
end)

-- start once Neovim has finished starting up and is waiting for input
vim.defer_fn(function()
  local ok, err = coroutine.resume(co)
  if not ok then
    out("FATAL " .. tostring(err) .. "\n")
    vim.cmd("cquit 2")
  end
end, 100)
