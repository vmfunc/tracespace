-- tracespace.nvim: runs the `tracespace` execution-flow visualizer in a split and
-- streams the current C/C++ buffer to it, including unsaved changes.
--
-- Protocol (newline-delimited JSON over a Unix socket, or a named pipe on Windows):
--   nvim -> app  {"type":"buffer","path":...,"text":...,"cursor":[line,col]}
--                {"type":"cursor","cursor":[line,col]}
--   app -> nvim  {"type":"jump","path":...,"line":N,"col":M}
-- Lines are 1-based and columns 0-based, as in nvim_win_get_cursor().

local M = {}

local uv = vim.uv or vim.loop
local api = vim.api

local defaults = {
  binary = "tracespace", -- name or full path of the executable
  width = 60, -- columns, for position = "left" / "right"
  height = 15, -- rows, for position = "top" / "bottom"
  position = "right", -- "right", "left", "top" or "bottom"
  theme = "braille", -- "braille", "rounded" or "ascii"
  debounce_ms = 50, -- delay before sending a changed buffer
  args = {}, -- extra command-line arguments for the binary
}

M.config = vim.deepcopy(defaults)

local state = {
  term_buf = nil,
  term_win = nil,
  job = nil,
  pipe = nil,
  connected = false,
  socket_path = nil,
  source_win = nil, -- window to jump in
  source_buf = nil, -- buffer last sent
  timer = nil, -- debounce timer
  retry = nil, -- connect retry timer
  pending = "", -- partial line received from the app
  augroup = nil,
  generation = 0, -- bumped on every open/close so stale callbacks do nothing
}

local is_windows = uv.os_uname().sysname:find("Windows") ~= nil
local counter = 0

local function notify(msg, level)
  vim.schedule(function()
    vim.notify("tracespace: " .. msg, level or vim.log.levels.WARN)
  end)
end

local function new_socket_path()
  counter = counter + 1
  if is_windows then
    return ([[\\.\pipe\tracespace-%d-%d]]):format(uv.os_getpid(), counter)
  end
  -- tempname() lives in Neovim's private temp directory and is short enough
  -- for sockaddr_un
  return vim.fn.tempname() .. ".sock"
end

local function is_source(buf)
  if not buf or not api.nvim_buf_is_valid(buf) then
    return false
  end
  local ft = vim.bo[buf].filetype
  return ft == "c" or ft == "cpp"
end

--- Path reported for a buffer. Unnamed buffers get a name whose extension
--- selects the right grammar.
local function buffer_path(buf)
  local name = api.nvim_buf_get_name(buf)
  if name ~= "" then
    return name
  end
  return vim.bo[buf].filetype == "c" and "untitled.c" or "untitled.cpp"
end

local function write(message)
  if state.pipe and state.connected and not state.pipe:is_closing() then
    state.pipe:write(vim.json.encode(message) .. "\n")
  end
end

local function cursor_in(win)
  local c = api.nvim_win_get_cursor(win)
  return { c[1], c[2] }
end

local function window_for(buf)
  local win = api.nvim_get_current_win()
  if api.nvim_win_get_buf(win) == buf then
    return win
  end
  local wins = vim.fn.win_findbuf(buf)
  return wins[1]
end

local function send_buffer(buf)
  if not state.connected or not is_source(buf) then
    return
  end
  local lines = api.nvim_buf_get_lines(buf, 0, -1, false)
  local message = {
    type = "buffer",
    path = buffer_path(buf),
    text = table.concat(lines, "\n") .. "\n",
  }
  local win = window_for(buf)
  if win then
    message.cursor = cursor_in(win)
    state.source_win = win
  end
  state.source_buf = buf
  write(message)
end

local function stop_timer(name)
  local t = state[name]
  if t then
    t:stop()
    if not t:is_closing() then
      t:close()
    end
    state[name] = nil
  end
end

local function schedule_buffer(buf)
  stop_timer("timer")
  state.timer = uv.new_timer()
  state.timer:start(
    M.config.debounce_ms,
    0,
    vim.schedule_wrap(function()
      stop_timer("timer")
      send_buffer(buf)
    end)
  )
end

local function handle_jump(msg)
  if type(msg.line) ~= "number" then
    return
  end
  local buf = state.source_buf
  if type(msg.path) == "string" and msg.path ~= "" then
    local named = vim.fn.bufnr(msg.path)
    if named ~= -1 then
      buf = named
    end
  end
  if not buf or not api.nvim_buf_is_valid(buf) then
    return
  end
  -- prefer the window we were streaming from, then any window showing the buffer
  local win = state.source_win
  if not (win and api.nvim_win_is_valid(win) and win ~= state.term_win) then
    win = vim.fn.win_findbuf(buf)[1]
  end
  if not win then
    for _, w in ipairs(api.nvim_tabpage_list_wins(0)) do
      if w ~= state.term_win then
        win = w
        break
      end
    end
  end
  if not win then
    return
  end
  if api.nvim_win_get_buf(win) ~= buf then
    api.nvim_win_set_buf(win, buf)
  end
  vim.cmd("stopinsert")
  api.nvim_set_current_win(win)
  local last = api.nvim_buf_line_count(buf)
  local line = math.max(1, math.min(msg.line, last))
  local text = api.nvim_buf_get_lines(buf, line - 1, line, false)[1] or ""
  local col = math.max(0, math.min(tonumber(msg.col) or 0, math.max(0, #text - 1)))
  api.nvim_win_set_cursor(win, { line, col })
  vim.cmd("normal! zv")
end

local function on_data(chunk)
  state.pending = state.pending .. chunk
  while true do
    local nl = state.pending:find("\n", 1, true)
    if not nl then
      break
    end
    local line = state.pending:sub(1, nl - 1)
    state.pending = state.pending:sub(nl + 1)
    if line ~= "" then
      local ok, msg = pcall(vim.json.decode, line)
      if ok and type(msg) == "table" and msg.type == "jump" then
        handle_jump(msg)
      end
    end
  end
end

local function close_pipe()
  state.connected = false
  local p = state.pipe
  state.pipe = nil
  if p and not p:is_closing() then
    p:read_stop()
    p:close()
  end
end

--- Connect to the app's socket, retrying while it starts up.
local function connect(attempt)
  local generation = state.generation
  local pipe = uv.new_pipe(false)
  state.pipe = pipe
  pipe:connect(state.socket_path, function(err)
    if generation ~= state.generation then
      if not pipe:is_closing() then
        pipe:close()
      end
      return
    end
    if err then
      if not pipe:is_closing() then
        pipe:close()
      end
      state.pipe = nil
      if attempt >= 100 then
        notify("could not connect to " .. state.socket_path .. " (" .. tostring(err) .. ")")
        return
      end
      stop_timer("retry")
      state.retry = uv.new_timer()
      state.retry:start(50, 0, function()
        vim.schedule(function()
          stop_timer("retry")
          if generation == state.generation then
            connect(attempt + 1)
          end
        end)
      end)
      return
    end
    state.connected = true
    pipe:read_start(function(read_err, chunk)
      if read_err or not chunk then
        vim.schedule(function()
          if generation == state.generation then
            close_pipe()
          end
        end)
        return
      end
      vim.schedule(function()
        if generation == state.generation then
          on_data(chunk)
        end
      end)
    end)
    vim.schedule(function()
      if generation ~= state.generation then
        return
      end
      -- start streaming: the buffer we opened from, or whatever is current now
      local buf = state.source_buf
      if not is_source(buf) then
        buf = api.nvim_get_current_buf()
      end
      send_buffer(buf)
    end)
  end)
end

local function create_autocmds()
  state.augroup = api.nvim_create_augroup("tracespace", { clear = true })
  local group = state.augroup

  api.nvim_create_autocmd({ "TextChanged", "TextChangedI", "TextChangedP" }, {
    group = group,
    callback = function(ev)
      if is_source(ev.buf) then
        schedule_buffer(ev.buf)
      end
    end,
  })

  api.nvim_create_autocmd({ "CursorMoved", "CursorMovedI" }, {
    group = group,
    callback = function(ev)
      if not is_source(ev.buf) then
        return
      end
      if ev.buf ~= state.source_buf then
        send_buffer(ev.buf) -- a buffer we have not sent yet
        return
      end
      local win = api.nvim_get_current_win()
      state.source_win = win
      write({ type = "cursor", cursor = cursor_in(win) })
    end,
  })

  -- switching buffers (or a buffer becoming C/C++) sends the new buffer
  api.nvim_create_autocmd({ "BufEnter", "FileType" }, {
    group = group,
    callback = function(ev)
      if ev.buf == state.term_buf then
        if ev.event == "BufEnter" then
          vim.cmd("startinsert") -- keys go straight to the diagram
        end
        return
      end
      if is_source(ev.buf) and api.nvim_get_current_buf() == ev.buf then
        stop_timer("timer")
        send_buffer(ev.buf)
      end
    end,
  })

  api.nvim_create_autocmd("VimLeavePre", {
    group = group,
    callback = function()
      M.close()
    end,
  })
end

function M.is_open()
  return state.term_buf ~= nil and api.nvim_buf_is_valid(state.term_buf)
end

function M.is_connected()
  return state.connected
end

--- Stop the app and remove everything the plugin created.
function M.close()
  state.generation = state.generation + 1
  stop_timer("timer")
  stop_timer("retry")
  close_pipe()
  state.pending = ""
  if state.augroup then
    pcall(api.nvim_del_augroup_by_id, state.augroup)
    state.augroup = nil
  end
  local job, buf, path = state.job, state.term_buf, state.socket_path
  state.job, state.term_buf, state.term_win, state.socket_path = nil, nil, nil, nil
  state.source_buf, state.source_win = nil, nil
  if job then
    pcall(vim.fn.jobstop, job)
  end
  if buf and api.nvim_buf_is_valid(buf) then
    pcall(api.nvim_buf_delete, buf, { force = true })
  end
  if path and not is_windows then
    pcall(uv.fs_unlink, path) -- the app removes it on a clean exit; make sure
  end
end

--- Open the diagram for the current buffer.
function M.open()
  if M.is_open() then
    return
  end
  local cfg = M.config
  -- accept a bare name on $PATH as well as paths starting with ~
  local binary = vim.fn.exepath(vim.fs.normalize(cfg.binary))
  if binary == "" then
    notify(("executable not found: %s (set `binary` in setup())"):format(cfg.binary), vim.log.levels.ERROR)
    return
  end
  state.generation = state.generation + 1
  local generation = state.generation
  local source_buf = api.nvim_get_current_buf()
  local source_win = api.nvim_get_current_win()
  state.source_win = source_win
  state.source_buf = is_source(source_buf) and source_buf or nil
  state.socket_path = new_socket_path()

  local cmd = { binary, "--socket", state.socket_path, "--theme", cfg.theme }
  vim.list_extend(cmd, cfg.args or {})
  local file = api.nvim_buf_get_name(source_buf)
  if state.source_buf and file ~= "" and vim.fn.filereadable(file) == 1 then
    table.insert(cmd, file) -- shown until the first buffer arrives, and again if we disconnect
  end

  local splits = {
    right = ("botright vertical %dnew"):format(cfg.width),
    left = ("topleft vertical %dnew"):format(cfg.width),
    bottom = ("botright %dnew"):format(cfg.height),
    top = ("topleft %dnew"):format(cfg.height),
  }
  vim.cmd(splits[cfg.position] or splits.right)
  state.term_win = api.nvim_get_current_win()
  state.term_buf = api.nvim_get_current_buf()

  local on_exit = function()
    vim.schedule(function()
      if generation == state.generation then
        M.close() -- the app quit (for example with `q`)
      end
    end)
  end
  if vim.fn.has("nvim-0.11") == 1 then
    state.job = vim.fn.jobstart(cmd, { term = true, on_exit = on_exit })
  else
    state.job = vim.fn.termopen(cmd, { on_exit = on_exit })
  end
  if not state.job or state.job <= 0 then
    notify("failed to start " .. binary, vim.log.levels.ERROR)
    M.close()
    return
  end

  local wo = vim.wo[state.term_win]
  wo.number = false
  wo.relativenumber = false
  wo.signcolumn = "no"
  wo.winfixwidth = true
  wo.winfixheight = true
  vim.bo[state.term_buf].buflisted = false
  pcall(api.nvim_buf_set_name, state.term_buf, "tracespace")

  create_autocmds()
  if api.nvim_win_is_valid(source_win) then
    api.nvim_set_current_win(source_win)
  end
  vim.cmd("stopinsert")
  connect(1)
end

--- :Tracespace opens the split, or closes it when it is already open.
function M.toggle()
  if M.is_open() then
    M.close()
  else
    M.open()
  end
end

function M.setup(opts)
  M.config = vim.tbl_deep_extend("force", vim.deepcopy(defaults), opts or {})
  api.nvim_create_user_command("Tracespace", function()
    M.toggle()
  end, { desc = "Toggle the tracespace execution-flow diagram" })
end

-- exposed for tests
M._state = state

return M
