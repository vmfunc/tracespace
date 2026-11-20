-- Registers (or removes) an installed tracespace in the user's Neovim config.
-- Run through `make install-nvim` / `make uninstall`, or directly:
--
--   nvim --headless --clean -l scripts/nvim-config.lua install   <prefix>
--   nvim --headless --clean -l scripts/nvim-config.lua uninstall <prefix>
--
-- For a lazy.nvim config with a lua/plugins/ directory it writes one new file,
-- lua/plugins/tracespace.lua, and touches nothing else. Other setups get the
-- lines to add by hand.

local uv = vim.uv or vim.loop
local action, prefix = arg[1], arg[2]
local MARKER = "-- managed by tracespace (make install-nvim); `make uninstall` removes this file"

local function say(msg)
  io.stdout:write(msg .. "\n")
end

local function fail(msg)
  io.stderr:write("tracespace: " .. msg .. "\n")
  os.exit(1)
end

if (action ~= "install" and action ~= "uninstall") or not prefix or prefix == "" then
  fail("usage: nvim -l scripts/nvim-config.lua install|uninstall <prefix>")
end

prefix = vim.fs.normalize(vim.fn.fnamemodify(prefix, ":p")):gsub("/$", "")
local is_windows = uv.os_uname().sysname:find("Windows") ~= nil
local binary = prefix .. "/bin/tracespace" .. (is_windows and ".exe" or "")
local plugin_dir = prefix .. "/share/tracespace/nvim"
local config = vim.fs.normalize(vim.fn.stdpath("config"))
local plugins_dir = config .. "/lua/plugins"
local spec_path = plugins_dir .. "/tracespace.lua"

local function read(path)
  local f = io.open(path, "r")
  if not f then
    return nil
  end
  local text = f:read("*a")
  f:close()
  return text
end

local function exists(path)
  return uv.fs_stat(path) ~= nil
end

local spec = table.concat({
  MARKER,
  "-- tracespace: live execution-flow diagrams for C and C++. :Tracespace toggles it.",
  "return {",
  ("  dir = %q,"):format(plugin_dir),
  '  name = "tracespace.nvim",',
  '  main = "tracespace",',
  '  cmd = "Tracespace",',
  '  ft = { "c", "cpp" },',
  "  opts = {",
  ("    binary = %q,"):format(binary),
  "    width = 60,",
  '    position = "right",',
  '    theme = "braille",',
  "  },",
  "}",
  "",
}, "\n")

if action == "install" then
  if not exists(binary) then
    fail("nothing installed at " .. binary .. " (run `make install` first)")
  end
  if not exists(plugin_dir .. "/lua/tracespace/init.lua") then
    fail("plugin not found in " .. plugin_dir .. " (run `make install` first)")
  end
  -- :help tracespace
  vim.cmd("silent! helptags " .. vim.fn.fnameescape(plugin_dir .. "/doc"))

  local uses_lazy = exists(plugins_dir) and (read(config .. "/init.lua") or ""):find("lazy", 1, true) ~= nil
  if not uses_lazy then
    say("Installed, but this Neovim config is not a lazy.nvim setup with lua/plugins/.")
    say("Add this to " .. config .. "/init.lua yourself:")
    say("")
    say(("  vim.opt.runtimepath:append(%q)"):format(plugin_dir))
    say(("  require(\"tracespace\").setup({ binary = %q })"):format(binary))
    os.exit(0)
  end

  local current = read(spec_path)
  if current and not current:find(MARKER, 1, true) then
    fail(spec_path .. " already exists and was not written by this installer; leaving it alone")
  end
  local f = assert(io.open(spec_path, "w"))
  f:write(spec)
  f:close()
  say((current and "updated " or "wrote ") .. spec_path)
  say("binary  " .. binary)
  say("plugin  " .. plugin_dir)
  say("Restart Neovim, open a C or C++ file and run :Tracespace")
  os.exit(0)
end

-- uninstall: only remove what the installer created
local current = read(spec_path)
if current and current:find(MARKER, 1, true) then
  os.remove(spec_path)
  say("removed " .. spec_path)
elseif current then
  say("kept    " .. spec_path .. " (not written by this installer)")
end
if exists(binary) then
  os.remove(binary)
  say("removed " .. binary)
end
if exists(prefix .. "/share/tracespace") then
  vim.fn.delete(prefix .. "/share/tracespace", "rf")
  say("removed " .. prefix .. "/share/tracespace")
end
-- drop directories the install created, if they are now empty
for _, dir in ipairs({ prefix .. "/bin", prefix .. "/share", prefix }) do
  uv.fs_rmdir(dir)
end
os.exit(0)
